#!/usr/bin/env python3
"""Compile tools/level.json into world/level_data.cpp for tomb-explorer.

Rooms are sector grids. From the per-corner floor and ceiling heights the
generator emits floor, ceiling, wall, step and ceiling-drop quads with baked
vertex light, the portal quads between rooms and the collision sector table.
Use --check to verify the checked-in C++ matches the description.

Coordinates: x east, y up, z south (north is -z). Faces are listed
counter-clockwise as seen from inside the room, which is what the SDK
MeshRenderer treats as the front.
"""
import argparse
import json
import math
from pathlib import Path

APP = Path(__file__).resolve().parents[1]
TEXELS_PER_UNIT = 64
MAX_WALL_PIECE = 2.0     # units of height per wall quad, keeps texel spans under 256
WATER_DEPTH = 0.3
MAX_VERTICES = 1024      # micropixel::MeshRenderer::kMaxMeshVertices

# Corner order used everywhere: north-west, north-east, south-east, south-west.
NW, NE, SE, SW = range(4)
# Edge -> (left corner, right corner) as seen from inside the sector facing the edge.
EDGES = {
    'north': (NE, NW, 0, -1),
    'east': (SE, NE, 1, 0),
    'south': (SW, SE, 0, 1),
    'west': (NW, SW, -1, 0),
}
OPPOSITE = {'north': 'south', 'south': 'north', 'east': 'west', 'west': 'east'}


class Sector:
    def __init__(self, x, z, solid, floor, ceiling, floor_texture):
        self.x = x
        self.z = z
        self.solid = solid
        self.floor = floor        # four corner heights
        self.ceiling = ceiling
        self.floor_texture = floor_texture

    def corner(self, index):
        dx = 1 if index in (NE, SE) else 0
        dz = 1 if index in (SE, SW) else 0
        return self.x + dx, self.z + dz


class Room:
    def __init__(self, spec, defaults):
        self.name = spec['name']
        self.origin_x, self.origin_z = spec['origin']
        rows = spec['map']
        self.width = len(rows[0])
        self.depth = len(rows)
        if any(len(row) != self.width for row in rows):
            raise ValueError(f'{self.name}: map rows differ in length')
        self.ambient = spec.get('ambient', 60)
        self.lights = spec.get('lights', [])
        self.frieze = spec.get('frieze')
        self.textures = dict(defaults)
        self.textures.update(spec.get('textures', {}))
        self.portal_specs = spec.get('portals', [])
        base_floor = float(spec['floor'])
        base_ceiling = float(spec['ceiling'])
        slope = spec.get('slope')
        self.sectors = []
        for sz, row in enumerate(rows):
            for sx, char in enumerate(row):
                x = self.origin_x + sx
                z = self.origin_z + sz
                offset = 0.0
                floor_texture = self.textures['floor']
                if char.isdigit():
                    offset = int(char) * 0.25
                elif char == '~':
                    offset = -WATER_DEPTH
                    floor_texture = self.textures['water']
                elif char not in '.#':
                    raise ValueError(f'{self.name}: unknown map char {char!r}')
                floor = []
                ceiling = []
                for index in range(4):
                    cx = x + (1 if index in (NE, SE) else 0)
                    cz = z + (1 if index in (SE, SW) else 0)
                    f = base_floor + offset
                    c = base_ceiling
                    if slope:
                        t = self.slope_parameter(slope, cx, cz)
                        f = base_floor + (slope['floor_to'] - base_floor) * t + offset
                        c = base_ceiling + (slope.get('ceiling_to', base_ceiling) - base_ceiling) * t
                    floor.append(f)
                    ceiling.append(c)
                solid = char == '#'
                if solid:
                    ceiling = list(floor)
                self.sectors.append(Sector(x, z, solid, floor, ceiling, floor_texture))

    def slope_parameter(self, slope, cx, cz):
        if slope['axis'] == 'z':
            return (cz - self.origin_z) / self.depth
        return (cx - self.origin_x) / self.width

    def contains(self, x, z):
        return self.origin_x <= x < self.origin_x + self.width and self.origin_z <= z < self.origin_z + self.depth

    def sector_at(self, x, z):
        if not self.contains(x, z):
            return None
        return self.sectors[(z - self.origin_z) * self.width + (x - self.origin_x)]

    def boundary_portal(self, side, sx, sz):
        """Portal spec covering the sector (sx, sz) on room boundary `side`, or None."""
        for spec in self.portal_specs:
            if spec['side'] != side:
                continue
            lo, hi = spec['range']
            along = sx if side in ('north', 'south') else sz
            edge_at_boundary = ((side == 'north' and sz == 0) or (side == 'south' and sz == self.depth - 1) or
                                (side == 'west' and sx == 0) or (side == 'east' and sx == self.width - 1))
            if edge_at_boundary and lo <= along <= hi:
                return spec
        return None


class MeshBuilder:
    def __init__(self, room, level):
        self.room = room
        self.level = level
        self.vertices = []
        self.vertex_index = {}
        self.faces = []

    def vertex(self, x, y, z):
        key = (round(x, 4), round(y, 4), round(z, 4))
        index = self.vertex_index.get(key)
        if index is None:
            index = len(self.vertices)
            self.vertices.append(key)
            self.vertex_index[key] = index
        return index

    def light(self, x, y, z):
        value = self.room.ambient
        for light in self.room.lights:
            dx, dy, dz = x - light['x'], y - light['y'], z - light['z']
            distance = math.sqrt(dx * dx + dy * dy + dz * dz)
            if distance < light['radius']:
                value += light['intensity'] * (1.0 - distance / light['radius'])
        return max(0, min(255, int(round(value))))

    def face(self, corners, uvs, texture, flags=0):
        """corners: four (x, y, z); uvs: four (u, v) texels."""
        us = [uv[0] for uv in uvs]
        vs = [uv[1] for uv in uvs]
        if max(us) - min(us) >= 256 or max(vs) - min(vs) >= 256:
            raise ValueError(f'{self.room.name}: face texel span exceeds 255')
        # Shift into 0..65535 wire range; the renderer rebases on 256 anyway.
        shift_u = math.floor(min(us) / 256) * 256
        shift_v = math.floor(min(vs) / 256) * 256
        self.faces.append({
            'vertex': [self.vertex(*c) for c in corners],
            'u': [int(round(u - shift_u)) for u in us],
            'v': [int(round(v - shift_v)) for v in vs],
            'light': [self.light(*c) for c in corners],
            'texture': texture,
            'flags': flags,
        })

    def horizontal(self, sector, heights, texture, facing_up):
        pts = []
        for index in range(4):
            cx, cz = sector.corner(index)
            pts.append((float(cx), heights[index], float(cz)))
        uvs = [((cx % 4) * TEXELS_PER_UNIT, (cz % 4) * TEXELS_PER_UNIT) for (cx, _, cz) in pts]
        # Texel tiles wrap on 4 sectors; keep the eastern/southern edge at 256 rather than 0.
        uvs = self.continuous_tiles(pts, uvs)
        if facing_up:
            self.face(pts, uvs, texture)
        else:
            self.face(list(reversed(pts)), list(reversed(uvs)), texture)

    @staticmethod
    def continuous_tiles(pts, uvs):
        fixed = []
        base_u = uvs[NW][0]
        base_v = uvs[NW][1]
        for (x, _, z), (u, v) in zip(pts, uvs):
            if u < base_u:
                u += 4 * TEXELS_PER_UNIT
            if v < base_v:
                v += 4 * TEXELS_PER_UNIT
            fixed.append((u, v))
        return fixed

    def vertical(self, left, right, bottom_left, bottom_right, top_left, top_right, texture, frieze=None):
        """Wall between two edge points, split into pieces of at most MAX_WALL_PIECE height."""
        low = min(bottom_left, bottom_right)
        high = max(top_left, top_right)
        if high - low <= 1e-4:
            return
        # Piece boundaries at absolute heights so neighbouring walls line up.
        cuts = [low]
        level = math.floor(low / MAX_WALL_PIECE) * MAX_WALL_PIECE + MAX_WALL_PIECE
        while level < high - 1e-4:
            if level > low + 1e-4:
                cuts.append(level)
            level += MAX_WALL_PIECE
        cuts.append(high)
        (lx, lz), (rx, rz) = left, right
        for lower, upper in zip(cuts, cuts[1:]):
            bl = max(lower, bottom_left)
            br = max(lower, bottom_right)
            tl = min(upper, top_left)
            tr = min(upper, top_right)
            if tl - bl <= 1e-4 and tr - br <= 1e-4:
                continue
            piece_texture = texture
            if frieze is not None and lower >= frieze - 1e-4:
                piece_texture = self.room.textures['frieze']
            # v runs down the wall from the piece top so every 1 unit is one texture.
            top = upper
            pts = [(lx, bl, lz), (rx, br, rz), (rx, tr, rz), (lx, tl, lz)]
            uvs = [(0, (top - bl) * TEXELS_PER_UNIT), (TEXELS_PER_UNIT, (top - br) * TEXELS_PER_UNIT),
                   (TEXELS_PER_UNIT, (top - tr) * TEXELS_PER_UNIT), (0, (top - tl) * TEXELS_PER_UNIT)]
            self.face(pts, uvs, piece_texture)

    def build(self):
        room = self.room
        for sector in room.sectors:
            if sector.solid:
                continue
            self.horizontal(sector, sector.floor, sector.floor_texture, True)
            self.horizontal(sector, sector.ceiling, room.textures['ceiling'], False)
            sx = sector.x - room.origin_x
            sz = sector.z - room.origin_z
            for side, (left_corner, right_corner, dx, dz) in EDGES.items():
                left = sector.corner(left_corner)
                right = sector.corner(right_corner)
                neighbour = room.sector_at(sector.x + dx, sector.z + dz)
                portal = None
                if neighbour is None:
                    portal = room.boundary_portal(side, sx, sz)
                    if portal is not None:
                        target = self.level.room(portal['to'])
                        neighbour = target.sector_at(sector.x + dx, sector.z + dz)
                        if neighbour is None:
                            raise ValueError(f'{room.name}: portal to {portal["to"]} opens onto nothing')
                fl, fr = sector.floor[left_corner], sector.floor[right_corner]
                cl, cr = sector.ceiling[left_corner], sector.ceiling[right_corner]
                if neighbour is None or neighbour.solid:
                    self.vertical(left, right, fl, fr, cl, cr, room.textures['wall'], room.frieze)
                    continue
                # The neighbour shares this edge; its corners on the edge are the mirrored pair.
                n_left, n_right = self.matching_corners(neighbour, left, right)
                nfl, nfr = neighbour.floor[n_left], neighbour.floor[n_right]
                ncl, ncr = neighbour.ceiling[n_left], neighbour.ceiling[n_right]
                if nfl > fl + 1e-4 or nfr > fr + 1e-4:
                    if nfl < fl - 1e-4 or nfr < fr - 1e-4:
                        raise ValueError(f'{room.name}: crossing floor edge at {sector.x},{sector.z} {side}')
                    self.vertical(left, right, fl, fr, max(nfl, fl), max(nfr, fr), room.textures['step'])
                if ncl < cl - 1e-4 or ncr < cr - 1e-4:
                    if ncl > cl + 1e-4 or ncr > cr + 1e-4:
                        raise ValueError(f'{room.name}: crossing ceiling edge at {sector.x},{sector.z} {side}')
                    self.vertical(left, right, min(ncl, cl), min(ncr, cr), cl, cr, room.textures['wall'], room.frieze)
        if len(self.vertices) > MAX_VERTICES:
            raise ValueError(f'{room.name}: {len(self.vertices)} vertices exceed the renderer limit {MAX_VERTICES}')

    @staticmethod
    def matching_corners(neighbour, left, right):
        corners = [neighbour.corner(i) for i in range(4)]
        return corners.index(left), corners.index(right)


class Level:
    def __init__(self, spec):
        self.spec = spec
        defaults = spec['textures']
        self.rooms = [Room(room_spec, defaults) for room_spec in spec['rooms']]
        names = [room.name for room in self.rooms]
        if len(set(names)) != len(names):
            raise ValueError('duplicate room names')
        for a in self.rooms:
            for b in self.rooms:
                if a is not b and self.overlaps(a, b):
                    raise ValueError(f'rooms {a.name} and {b.name} overlap')

    @staticmethod
    def overlaps(a, b):
        return (a.origin_x < b.origin_x + b.width and b.origin_x < a.origin_x + a.width and
                a.origin_z < b.origin_z + b.depth and b.origin_z < a.origin_z + a.depth)

    def room(self, name):
        for room in self.rooms:
            if room.name == name:
                return room
        raise ValueError(f'unknown room {name}')

    def room_index(self, name):
        return [room.name for room in self.rooms].index(name)

    def portals(self, room):
        """Portal quads of `room`: floor-to-ceiling openings, one per portal spec."""
        result = []
        for spec in room.portal_specs:
            side = spec['side']
            lo, hi = spec['range']
            target = self.room(spec['to'])
            left_corner, right_corner, dx, dz = EDGES[side]
            # Sectors along the boundary from left to right as seen from inside.
            along = list(range(lo, hi + 1))
            if side in ('north', 'east'):
                along.reverse()
            sectors = []
            for a in along:
                sx, sz = (a, 0) if side == 'north' else (a, room.depth - 1) if side == 'south' else \
                    (room.width - 1, a) if side == 'east' else (0, a)
                sector = room.sectors[sz * room.width + sx]
                if sector.solid:
                    raise ValueError(f'{room.name}: portal through a solid sector')
                other = target.sector_at(sector.x + dx, sector.z + dz)
                if other is None or other.solid:
                    raise ValueError(f'{room.name}: portal to {spec["to"]} does not meet an open sector')
                if target.boundary_portal(OPPOSITE[side], other.x - target.origin_x, other.z - target.origin_z) is None:
                    raise ValueError(f'{room.name}: {spec["to"]} has no matching portal')
                sectors.append(sector)
            first, last = sectors[0], sectors[-1]
            left = first.corner(left_corner)
            right = last.corner(right_corner)
            floor_l = first.floor[left_corner]
            floor_r = last.floor[right_corner]
            ceil_l = first.ceiling[left_corner]
            ceil_r = last.ceiling[right_corner]
            result.append({
                'corners': [(left[0], floor_l, left[1]), (right[0], floor_r, right[1]),
                            (right[0], ceil_r, right[1]), (left[0], ceil_l, left[1])],
                'target': self.room_index(spec['to']),
            })
        return result


def cpp_float(value):
    text = f'{value:.4f}'.rstrip('0')
    if text.endswith('.'):
        text += '0'
    return text + 'F'


def ident(name):
    return ''.join(part.capitalize() for part in name.replace('-', '_').split('_'))


def render(level):
    out = [
        '// Generated by tools/generate_level.py from tools/level.json; do not edit.',
        '#include "apps/tomb-explorer/gfx/textures.hpp"',
        '#include "apps/tomb-explorer/world/level.hpp"',
        '',
        'namespace tomb::world {',
        'namespace {',
        '',
        'using micropixel::MeshFace;',
        'using micropixel::MeshVertex;',
        '',
        '// clang-format off',
    ]
    stats = []
    for room in level.rooms:
        builder = MeshBuilder(room, level)
        builder.build()
        name = ident(room.name)
        out.append(f'constexpr MeshVertex k{name}Vertices[{len(builder.vertices)}] = {{')
        for x, y, z in builder.vertices:
            out.append(f'    {{{cpp_float(x)}, {cpp_float(y)}, {cpp_float(z)}}},')
        out.append('};')
        out.append(f'constexpr MeshFace k{name}Faces[{len(builder.faces)}] = {{')
        for face in builder.faces:
            v = ', '.join(f'{i}U' for i in face['vertex'])
            u = ', '.join(f'{i}U' for i in face['u'])
            vv = ', '.join(f'{i}U' for i in face['v'])
            light = ', '.join(f'{i}U' for i in face['light'])
            out.append(f'    {{{{{v}}}, {{{u}}}, {{{vv}}}, {{{light}}}, gfx::{face["texture"]}, {face["flags"]}U}},')
        out.append('};')
        out.append(f'constexpr Sector k{name}Sectors[{len(room.sectors)}] = {{')
        for sector in room.sectors:
            floor = ', '.join(cpp_float(h) for h in sector.floor)
            ceiling = ', '.join(cpp_float(h) for h in sector.ceiling)
            out.append(f'    {{{{{floor}}}, {{{ceiling}}}, {"true" if sector.solid else "false"}}},')
        out.append('};')
        portals = level.portals(room)
        if portals:
            out.append(f'constexpr Portal k{name}Portals[{len(portals)}] = {{')
            for portal in portals:
                corners = ', '.join(f'{{{cpp_float(x)}, {cpp_float(y)}, {cpp_float(z)}}}' for x, y, z in portal['corners'])
                out.append(f'    {{{{{corners}}}, {portal["target"]}U}},')
            out.append('};')
        out.append('')
        stats.append((room.name, len(builder.vertices), len(builder.faces), len(portals)))
    out.append(f'constexpr Room kRooms[{len(level.rooms)}] = {{')
    for room in level.rooms:
        name = ident(room.name)
        portals = f'k{name}Portals' if level.portals(room) else '{}'
        out.append(f'    {{{cpp_float(room.origin_x)}, {cpp_float(room.origin_z)}, {room.width}U, {room.depth}U, '
                   f'{room.ambient}U, k{name}Sectors, k{name}Vertices, k{name}Faces, {portals}}},')
    out.append('};')
    start = level.spec['start']
    start_room = level.room(start['room'])
    start_sector = start_room.sector_at(int(math.floor(start['x'])), int(math.floor(start['z'])))
    if start_sector is None or start_sector.solid:
        raise ValueError('start position is not on an open sector')
    start_y = max(start_sector.floor)
    out += [
        '',
        f'constexpr Level kLevel = {{kRooms, {{{cpp_float(start["x"])}, {cpp_float(start_y)}, {cpp_float(start["z"])}}}, '
        f'{cpp_float(start["yaw"])}, {level.room_index(start["room"])}U}};',
        '// clang-format on',
        '',
        '}  // namespace',
        '',
        'const Level& TombLevel() { return kLevel; }',
        '',
        '}  // namespace tomb::world',
        '',
    ]
    return '\n'.join(out), stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='verify world/level_data.cpp is up to date')
    args = parser.parse_args()
    level = Level(json.loads((APP / 'tools/level.json').read_text()))
    text, stats = render(level)
    output = APP / 'world/level_data.cpp'
    if args.check:
        if not output.exists() or output.read_text() != text:
            raise SystemExit('stale: world/level_data.cpp')
        print('tomb-explorer level up to date')
        return
    output.write_text(text)
    for name, vertices, faces, portals in stats:
        print(f'{name}: {vertices} vertices, {faces} faces, {portals} portals')
    print(f'wrote {output.relative_to(APP)}')


if __name__ == '__main__':
    main()
