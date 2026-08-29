#!/usr/bin/env python3
"""Prepare immutable resources and build a 64 KiB-aligned MicroPixel App Bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path


MAGIC = b"MPXBNDL\0"
VERSION = 1
FRAMEWORK_ABI_VERSION = 1
EXTENT_ALIGNMENT = 64 * 1024
APP_ID_MAX_LENGTH = 64
DISPLAY_NAME_MAX_LENGTH = 64
LOCALE_MAX_LENGTH = 31
PACKAGE_METADATA_VERSION = 1
FORMAT_PACKAGE_METADATA_JSON = 7
HEADER = struct.Struct("<8sIIII64sIIIIIIIIII")
SECTION = struct.Struct("<IIIIIIIIIIII")
RESOURCE_PACK_MAGIC = b"MPXRPAK\0"
RESOURCE_PACK_VERSION = 1
RESOURCE_PACK_HEADER = struct.Struct("<8sIIIIII32s")
RESOURCE_PACK_ALIGNMENT = 64
KIND_AOT = 1
KIND_ASSET = 2
KIND_APP_METADATA = 3
KIND_FONT = 4
FORMAT_UTF8 = 6
FORMATS = {
    "aot": 1,
    "raw_rgb888": 2,
    "jpeg": 3,
    "png": 4,
    "raw_argb8888": 5,
    "font_cbin": 8,
    "ogg_opus": 9,
}
ASSET_FORMAT_IDS = frozenset(FORMATS.values()) - {FORMATS["aot"], FORMATS["font_cbin"]}
PNG_TO_RAW_RGB888 = "png_to_raw_rgb888"
LAUNCH_FORMATS = frozenset({FORMATS["jpeg"], FORMATS["png"]})
OGG_OPUS_MAX_TAG_BYTES = 64 * 1024
OGG_OPUS_MAX_PACKET_BYTES = 61_440
CPP_KEYWORDS = frozenset({
    "alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel",
    "atomic_commit", "atomic_noexcept", "auto", "bitand", "bitor", "bool",
    "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
    "class", "compl", "concept", "const", "consteval", "constexpr", "constinit",
    "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
    "default", "delete", "do", "double", "dynamic_cast", "else", "enum",
    "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
    "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
    "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private",
    "protected", "public", "reflexpr", "register", "reinterpret_cast", "requires",
    "return", "short", "signed", "sizeof", "static", "static_assert",
    "static_cast", "struct", "switch", "synchronized", "template", "this",
    "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
    "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t",
    "while", "xor", "xor_eq",
})


@dataclass(frozen=True)
class AtlasFrame:
    x: int
    y: int
    width: int
    height: int
    canvas_x: int
    canvas_y: int


@dataclass(frozen=True)
class AtlasSpec:
    group: str
    index: int
    canvas_width: int
    canvas_height: int
    frames: tuple[AtlasFrame, ...]


@dataclass(frozen=True)
class InputSection:
    kind: int
    section_id: int
    format: int
    width: int
    height: int
    stride: int
    data: bytes
    name: str | None = None
    atlas: AtlasSpec | None = None


@dataclass(frozen=True)
class ResourcePack:
    sections: list[InputSection]
    launch_asset_id: int
    digest: bytes


@dataclass(frozen=True)
class LocalizedDisplayNames:
    default_locale: str
    values: dict[str, str]


@dataclass(frozen=True)
class PackageManifest:
    package_id: str
    display_names: LocalizedDisplayNames
    launch_asset: str
    package_type: str = "app"
    component_type: str = ""
    version: str = ""
    languages: tuple[str, ...] = ()
    font_bundle: str = ""
    charset: str = ""
    font_roles: dict[str, dict[str, object]] | None = None
    display: str = "square"


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def fnv1a32(data: bytes) -> int:
    value = 0x811C9DC5
    for byte in data:
        value ^= byte
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value


def png_size(data: bytes) -> tuple[int, int]:
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError("invalid PNG asset")
    return struct.unpack(">II", data[16:24])


def jpeg_size(data: bytes) -> tuple[int, int]:
    if len(data) < 4 or data[:2] != b"\xff\xd8":
        raise ValueError("invalid JPEG asset")
    offset = 2
    while offset + 4 <= len(data):
        if data[offset] != 0xFF:
            offset += 1
            continue
        marker = data[offset + 1]
        offset += 2
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            continue
        if offset + 2 > len(data):
            break
        length = int.from_bytes(data[offset : offset + 2], "big")
        if length < 2 or offset + length > len(data):
            break
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            if length < 7:
                break
            return (
                int.from_bytes(data[offset + 5 : offset + 7], "big"),
                int.from_bytes(data[offset + 3 : offset + 5], "big"),
            )
        offset += length
    raise ValueError("JPEG dimensions not found")


def paeth_predictor(left: int, above: int, upper_left: int) -> int:
    estimate = left + above - upper_left
    left_distance = abs(estimate - left)
    above_distance = abs(estimate - above)
    upper_left_distance = abs(estimate - upper_left)
    if left_distance <= above_distance and left_distance <= upper_left_distance:
        return left
    if above_distance <= upper_left_distance:
        return above
    return upper_left


def decode_rgba8_png(data: bytes) -> tuple[int, int, bytes]:
    if len(data) < 8 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("invalid PNG asset")

    offset = 8
    width = 0
    height = 0
    compressed = bytearray()
    saw_header = False
    saw_end = False
    while offset + 12 <= len(data):
        length = int.from_bytes(data[offset : offset + 4], "big")
        chunk_type = data[offset + 4 : offset + 8]
        chunk_begin = offset + 8
        chunk_end = chunk_begin + length
        if chunk_end + 4 > len(data):
            raise ValueError("truncated PNG chunk")
        chunk = data[chunk_begin:chunk_end]
        expected_crc = int.from_bytes(data[chunk_end : chunk_end + 4], "big")
        actual_crc = zlib.crc32(chunk_type)
        actual_crc = zlib.crc32(chunk, actual_crc) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise ValueError("PNG chunk CRC mismatch")
        offset = chunk_end + 4

        if chunk_type == b"IHDR":
            if saw_header or length != 13:
                raise ValueError("invalid PNG header")
            width, height, bit_depth, color_type, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", chunk
            )
            if (
                width <= 0
                or height <= 0
                or bit_depth != 8
                or color_type != 6
                or compression != 0
                or filtering != 0
                or interlace != 0
            ):
                raise ValueError("PNG-to-raw requires non-interlaced 8-bit RGBA")
            saw_header = True
        elif chunk_type == b"IDAT":
            if not saw_header or saw_end:
                raise ValueError("invalid PNG chunk order")
            compressed.extend(chunk)
        elif chunk_type == b"IEND":
            if length != 0:
                raise ValueError("invalid PNG end chunk")
            saw_end = True
            break

    if not saw_header or not saw_end or not compressed:
        raise ValueError("incomplete PNG asset")

    bytes_per_pixel = 4
    row_bytes = width * bytes_per_pixel
    filtered = zlib.decompress(bytes(compressed))
    if len(filtered) != (row_bytes + 1) * height:
        raise ValueError("PNG decompressed size disagrees with dimensions")

    rgba = bytearray(row_bytes * height)
    source = 0
    for row in range(height):
        filter_type = filtered[source]
        source += 1
        destination = row * row_bytes
        previous = destination - row_bytes
        for column in range(row_bytes):
            raw = filtered[source + column]
            left = rgba[destination + column - bytes_per_pixel] if column >= bytes_per_pixel else 0
            above = rgba[previous + column] if row != 0 else 0
            upper_left = (
                rgba[previous + column - bytes_per_pixel]
                if row != 0 and column >= bytes_per_pixel
                else 0
            )
            if filter_type == 0:
                value = raw
            elif filter_type == 1:
                value = raw + left
            elif filter_type == 2:
                value = raw + above
            elif filter_type == 3:
                value = raw + ((left + above) // 2)
            elif filter_type == 4:
                value = raw + paeth_predictor(left, above, upper_left)
            else:
                raise ValueError("unsupported PNG row filter")
            rgba[destination + column] = value & 0xFF
        source += row_bytes

    return width, height, bytes(rgba)


def parse_rgb888_color(value: object, label: str) -> tuple[int, int, int]:
    if not isinstance(value, str) or not re.fullmatch(r"#[0-9A-Fa-f]{6}", value):
        raise ValueError(f"{label} must use #RRGGBB")
    color = int(value[1:], 16)
    return (color >> 16, (color >> 8) & 0xFF, color & 0xFF)


def rgba_to_raw_rgb888(rgba: bytes, background: tuple[int, int, int] | None) -> bytes:
    if len(rgba) % 4 != 0:
        raise ValueError("RGBA byte length must be divisible by four")
    rgb = bytearray((len(rgba) // 4) * 3)
    destination = 0
    for offset in range(0, len(rgba), 4):
        red, green, blue, alpha = rgba[offset : offset + 4]
        if alpha != 0xFF:
            if background is None:
                raise ValueError("PNG-to-RGB requires opaque pixels or a background color")
            inverse_alpha = 0xFF - alpha
            red = (red * alpha + background[0] * inverse_alpha + 127) // 255
            green = (green * alpha + background[1] * inverse_alpha + 127) // 255
            blue = (blue * alpha + background[2] * inverse_alpha + 127) // 255
        # LVGL's little-endian RGB888 storage is B, G, R in memory.
        rgb[destination : destination + 3] = bytes((blue, green, red))
        destination += 3
    return bytes(rgb)


def ogg_crc32(data: bytes) -> int:
    """Return the non-reflected CRC used by Ogg pages (RFC 3533)."""
    crc = 0
    for value in data:
        crc ^= value << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def validate_ogg_opus(data: bytes) -> tuple[int, int]:
    """Validate the bounded, single-stream Ogg Opus profile accepted by MicroPixel.

    Returns (channels, playable samples at the mandatory 48 kHz granule rate).
    """
    offset = 0
    serial: int | None = None
    expected_sequence = 0
    packet = bytearray()
    packets: list[bytes] = []
    packet_count = 0
    pre_skip = 0
    final_granule: int | None = None
    eos_seen = False

    while offset < len(data):
        if eos_seen or offset + 27 > len(data) or data[offset : offset + 4] != b"OggS":
            raise ValueError("ogg_opus contains a truncated or trailing Ogg page")
        version = data[offset + 4]
        header_type = data[offset + 5]
        granule = struct.unpack_from("<Q", data, offset + 6)[0]
        page_serial = struct.unpack_from("<I", data, offset + 14)[0]
        sequence = struct.unpack_from("<I", data, offset + 18)[0]
        expected_crc = struct.unpack_from("<I", data, offset + 22)[0]
        segment_count = data[offset + 26]
        segment_table_end = offset + 27 + segment_count
        if version != 0 or header_type & ~0x07 or segment_table_end > len(data):
            raise ValueError("ogg_opus contains an invalid Ogg page header")
        segments = data[offset + 27 : segment_table_end]
        page_end = segment_table_end + sum(segments)
        if page_end > len(data):
            raise ValueError("ogg_opus contains a truncated Ogg page body")
        page = bytearray(data[offset:page_end])
        page[22:26] = bytes(4)
        if ogg_crc32(page) != expected_crc:
            raise ValueError("ogg_opus Ogg page CRC failed")

        if serial is None:
            if header_type & 0x02 == 0 or header_type & 0x01 or sequence != 0:
                raise ValueError("ogg_opus must begin with a BOS page at sequence zero")
            serial = page_serial
        elif page_serial != serial or header_type & 0x02:
            raise ValueError("ogg_opus must contain exactly one logical stream")
        if sequence != expected_sequence:
            raise ValueError("ogg_opus Ogg page sequence is discontinuous")
        expected_sequence += 1
        if bool(header_type & 0x01) != bool(packet):
            raise ValueError("ogg_opus packet continuation flags are inconsistent")

        body_offset = segment_table_end
        packets_on_page = 0
        for lace in segments:
            packet.extend(data[body_offset : body_offset + lace])
            body_offset += lace
            if len(packet) > max(OGG_OPUS_MAX_TAG_BYTES, OGG_OPUS_MAX_PACKET_BYTES):
                raise ValueError("ogg_opus contains an oversized packet")
            if lace < 255:
                completed = bytes(packet)
                packet.clear()
                packets_on_page += 1
                packet_count += 1
                if packet_count <= 2:
                    packets.append(completed)
                elif len(completed) == 0 or len(completed) > OGG_OPUS_MAX_PACKET_BYTES:
                    raise ValueError("ogg_opus contains an invalid audio packet")

        if packet_count == 1:
            if packets_on_page != 1 or packet or granule != 0:
                raise ValueError("ogg_opus OpusHead must be isolated on its BOS page")
            head = packets[0]
            if len(head) < 19 or head[:8] != b"OpusHead" or head[8] == 0 or head[8] > 15:
                raise ValueError("ogg_opus has an invalid OpusHead packet")
            channels = head[9]
            pre_skip = struct.unpack_from("<H", head, 10)[0]
            mapping_family = head[18]
            if channels not in (1, 2) or mapping_family != 0 or len(head) != 19:
                raise ValueError("ogg_opus supports only mono/stereo mapping-family-0 streams")

        if packet_count >= 2 and len(packets) >= 2:
            tags = packets[1]
            if len(tags) < 16 or len(tags) > OGG_OPUS_MAX_TAG_BYTES or tags[:8] != b"OpusTags":
                raise ValueError("ogg_opus has an invalid or oversized OpusTags packet")
            vendor_length = struct.unpack_from("<I", tags, 8)[0]
            comment_count_offset = 12 + vendor_length
            if comment_count_offset + 4 > len(tags):
                raise ValueError("ogg_opus has a truncated OpusTags vendor field")
            comment_count = struct.unpack_from("<I", tags, comment_count_offset)[0]
            cursor = comment_count_offset + 4
            if comment_count > 1024:
                raise ValueError("ogg_opus has too many OpusTags comments")
            for _ in range(comment_count):
                if cursor + 4 > len(tags):
                    raise ValueError("ogg_opus has a truncated OpusTags comment")
                comment_length = struct.unpack_from("<I", tags, cursor)[0]
                cursor += 4
                if cursor + comment_length > len(tags):
                    raise ValueError("ogg_opus has a truncated OpusTags comment")
                cursor += comment_length

        if header_type & 0x04:
            if packet or packet_count < 3 or granule == 0xFFFFFFFFFFFFFFFF:
                raise ValueError("ogg_opus has an invalid EOS page")
            final_granule = granule
            eos_seen = True
        offset = page_end

    if not eos_seen or packet or len(packets) < 2 or final_granule is None:
        raise ValueError("ogg_opus stream is incomplete")
    if final_granule < pre_skip:
        raise ValueError("ogg_opus final granule precedes pre-skip")
    return packets[0][9], final_granule - pre_skip


def parse_asset(spec: str, background: tuple[int, int, int] | None = None) -> InputSection:
    parts = spec.split(":", 4)
    if len(parts) != 5:
        raise ValueError("asset must be ID:FORMAT:WIDTH:HEIGHT:PATH")
    raw_id, format_name, raw_width, raw_height, raw_path = parts
    if (format_name not in FORMATS or format_name == "aot") and format_name != PNG_TO_RAW_RGB888:
        raise ValueError(f"unsupported asset format: {format_name}")
    section_id = int(raw_id, 0)
    if section_id <= 0:
        raise ValueError("asset ID must be positive")
    data = Path(raw_path).read_bytes()
    if not data:
        raise ValueError(f"asset is empty: {raw_path}")
    width = int(raw_width, 0)
    height = int(raw_height, 0)
    output_format_name = format_name
    if format_name == PNG_TO_RAW_RGB888:
        actual_width, actual_height, rgba = decode_rgba8_png(data)
        width, height = (
            (actual_width, actual_height)
            if width == 0 or height == 0
            else (width, height)
        )
        if (width, height) != (actual_width, actual_height):
            raise ValueError("PNG dimensions disagree with asset spec")
        data = rgba_to_raw_rgb888(rgba, background)
        output_format_name = "raw_rgb888"
    elif format_name == "png":
        actual = png_size(data)
        width, height = actual if width == 0 or height == 0 else (width, height)
        if (width, height) != actual:
            raise ValueError("PNG dimensions disagree with asset spec")
    elif format_name == "jpeg":
        actual = jpeg_size(data)
        width, height = actual if width == 0 or height == 0 else (width, height)
        if (width, height) != actual:
            raise ValueError("JPEG dimensions disagree with asset spec")
    elif format_name == "raw_rgb888":
        if width <= 0 or height <= 0 or len(data) != width * height * 3:
            raise ValueError("raw_rgb888 size must equal WIDTH*HEIGHT*3")
    elif format_name == "raw_argb8888":
        if width <= 0 or height <= 0 or len(data) != width * height * 4:
            raise ValueError("raw_argb8888 size must equal WIDTH*HEIGHT*4")
    elif format_name == "font_cbin":
        if width != 0 or height != 0 or len(data) < 160 or not data.startswith(b"MPXFCBN\0"):
            raise ValueError("font_cbin must be a wrapped MicroPixel font cbin with zero dimensions")
    elif format_name == "ogg_opus":
        if width != 0 or height != 0:
            raise ValueError("ogg_opus assets must use zero dimensions")
        validate_ogg_opus(data)
    stride = (
        width * 3
        if output_format_name == "raw_rgb888"
        else width * 4
        if output_format_name == "raw_argb8888"
        else 0
    )
    kind = KIND_FONT if output_format_name == "font_cbin" else KIND_ASSET
    return InputSection(kind, section_id, FORMATS[output_format_name], width, height, stride, data)


def validate_asset_name(value: object, index: int) -> str:
    name = str(value)
    if not re.fullmatch(r"[a-z][a-z0-9]*(?:[._-][a-z0-9]+)*", name) or len(name) > 63:
        raise ValueError(
            f"asset manifest entry {index} name must start with a lowercase letter "
            "and contain at most 63 lowercase letters, digits, '.', '_' or '-'"
        )
    return name


def integer_list(value: object, length: int, label: str) -> list[int]:
    if (
        not isinstance(value, list)
        or len(value) != length
        or not all(isinstance(item, int) and not isinstance(item, bool) for item in value)
    ):
        raise ValueError(f"{label} must contain {length} integers")
    return list(value)


def parse_atlas(record: dict[str, object], section: InputSection, name: str, index: int) -> AtlasSpec | None:
    raw_atlas = record.get("atlas")
    if raw_atlas is None:
        return None
    if not isinstance(raw_atlas, dict):
        raise ValueError(f"asset manifest entry {index} atlas must be an object")
    if section.format != FORMATS["png"]:
        raise ValueError(f"asset manifest entry {index} atlas requires a PNG asset")
    if section.width > 65535 or section.height > 65535:
        raise ValueError(f"atlas asset {name} PNG dimensions exceed generated metadata limits")
    name_parts = name.rsplit(".", 1)
    if len(name_parts) != 2:
        raise ValueError(f"atlas asset {name} must use a group.variant semantic name")
    raw_index = raw_atlas.get("index")
    if not isinstance(raw_index, int) or isinstance(raw_index, bool):
        raise ValueError(f"atlas asset {name} requires a non-negative index")
    try:
        atlas_index = int(raw_index)
    except (TypeError, ValueError) as error:
        raise ValueError(f"atlas asset {name} requires a non-negative index") from error
    if atlas_index < 0:
        raise ValueError(f"atlas asset {name} requires a non-negative index")
    canvas_width, canvas_height = integer_list(raw_atlas.get("canvas"), 2, f"atlas asset {name} canvas")
    if not (0 < canvas_width <= 32767 and 0 < canvas_height <= 32767):
        raise ValueError(f"atlas asset {name} canvas dimensions are invalid")
    raw_frames = raw_atlas.get("frames")
    if not isinstance(raw_frames, list) or not raw_frames or len(raw_frames) > 65535:
        raise ValueError(f"atlas asset {name} frames must be a non-empty list")
    frames: list[AtlasFrame] = []
    for frame_index, raw_frame in enumerate(raw_frames):
        if not isinstance(raw_frame, dict):
            raise ValueError(f"atlas asset {name} frame {frame_index} must be an object")
        x, y, width, height = integer_list(
            raw_frame.get("region"), 4, f"atlas asset {name} frame {frame_index} region"
        )
        canvas_x, canvas_y = integer_list(
            raw_frame.get("canvas_position"),
            2,
            f"atlas asset {name} frame {frame_index} canvas_position",
        )
        if x < 0 or y < 0 or width <= 0 or height <= 0:
            raise ValueError(f"atlas asset {name} frame {frame_index} has an invalid region")
        if x + width > section.width or y + height > section.height:
            raise ValueError(
                f"atlas asset {name} frame {frame_index} exceeds the {section.width}x{section.height} PNG"
            )
        if canvas_x < 0 or canvas_y < 0 or canvas_x > 32767 or canvas_y > 32767:
            raise ValueError(f"atlas asset {name} frame {frame_index} has an invalid canvas position")
        if canvas_x + width > canvas_width or canvas_y + height > canvas_height:
            raise ValueError(
                f"atlas asset {name} frame {frame_index} exceeds the {canvas_width}x{canvas_height} canvas"
            )
        frames.append(AtlasFrame(x, y, width, height, canvas_x, canvas_y))
    return AtlasSpec(name_parts[0], atlas_index, canvas_width, canvas_height, tuple(frames))


def load_asset_manifest(path: Path) -> list[InputSection]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise ValueError("asset manifest must use schema_version 1")
    records = value.get("assets")
    if not isinstance(records, list):
        raise ValueError("asset manifest assets must be a list")

    sections: list[InputSection] = []
    names: set[str] = set()
    for index, record in enumerate(records):
        if not isinstance(record, dict):
            raise ValueError(f"asset manifest entry {index} must be an object")
        if "name" not in record:
            raise ValueError(f"asset manifest entry {index} requires a semantic name")
        name = validate_asset_name(record["name"], index)
        if name in names:
            raise ValueError(f"duplicate asset name: {name}")
        names.add(name)
        section_id = fnv1a32(name.encode("ascii"))
        if section_id == 0:
            raise ValueError(f"asset name hashes to reserved ID zero: {name}")
        try:
            asset_path = path.parent / str(record["path"])
            spec = (
                f'{section_id}:{record["format"]}:'
                f'{int(record.get("width", 0))}:{int(record.get("height", 0))}:'
                f'{asset_path}'
            )
        except (KeyError, TypeError, ValueError) as error:
            raise ValueError(f"invalid asset manifest entry {index}") from error
        background = None
        if "background" in record:
            if record.get("format") != PNG_TO_RAW_RGB888:
                raise ValueError(f"asset manifest entry {index} background requires {PNG_TO_RAW_RGB888}")
            background = parse_rgb888_color(record["background"], f"asset manifest entry {index} background")
        section = parse_asset(spec, background)
        atlas = parse_atlas(record, section, name, index)
        sections.append(InputSection(
            section.kind, section.section_id, section.format, section.width,
            section.height, section.stride, section.data, name, atlas,
        ))
    return sections


def validate_display_name(value: object) -> str:
    if not isinstance(value, str) or not value or value.startswith(" ") or value.endswith(" "):
        raise ValueError("display_name must be a non-empty string without surrounding whitespace")
    encoded = value.encode("utf-8")
    if len(encoded) > DISPLAY_NAME_MAX_LENGTH:
        raise ValueError(f"display_name must be at most {DISPLAY_NAME_MAX_LENGTH} UTF-8 bytes")
    if any(ord(character) < 0x20 or ord(character) == 0x7F for character in value):
        raise ValueError("display_name must not contain control characters")
    return value


def normalize_locale_tag(value: object) -> str:
    if not isinstance(value, str) or not value or len(value.encode("utf-8")) > LOCALE_MAX_LENGTH:
        raise ValueError("Locale tag must be a non-empty ASCII string of at most 31 bytes")
    if value.startswith("-") or value.endswith("-") or "--" in value:
        raise ValueError(f"invalid Locale tag: {value!r}")
    subtags = value.split("-")
    if not 2 <= len(subtags[0]) <= 3 or not subtags[0].isascii() or not subtags[0].isalpha():
        raise ValueError(f"invalid Locale language subtag: {value!r}")
    normalized = [subtags[0].lower()]
    has_script = False
    has_region = False
    for subtag in subtags[1:]:
        if not has_script and not has_region and len(subtag) == 4 and subtag.isascii() and subtag.isalpha():
            normalized.append(subtag.title())
            has_script = True
            continue
        is_region = (
            len(subtag) == 2 and subtag.isascii() and subtag.isalpha()
        ) or (
            len(subtag) == 3 and subtag.isascii() and subtag.isdigit()
        )
        if not has_region and is_region:
            normalized.append(subtag.upper() if subtag.isalpha() else subtag)
            has_region = True
            continue
        raise ValueError(f"unsupported Locale subtag in {value!r}")
    return "-".join(normalized)


def parse_display_names(value: object, fallback_default: object = "en") -> LocalizedDisplayNames:
    if isinstance(value, str):
        default_locale = normalize_locale_tag(fallback_default)
        return LocalizedDisplayNames(default_locale, {default_locale: validate_display_name(value)})
    if not isinstance(value, dict):
        raise ValueError("display_name must be a string or localized display-name object")
    default_locale = normalize_locale_tag(value.get("default"))
    raw_values = value.get("values")
    if not isinstance(raw_values, dict) or not raw_values:
        raise ValueError("display_name.values must be a non-empty object")
    values: dict[str, str] = {}
    for raw_locale, raw_name in raw_values.items():
        locale = normalize_locale_tag(raw_locale)
        if raw_locale != locale:
            raise ValueError(f"display_name Locale must use canonical spelling: {raw_locale!r} -> {locale!r}")
        if locale in values:
            raise ValueError(f"duplicate display_name Locale: {locale}")
        values[locale] = validate_display_name(raw_name)
    if default_locale not in values:
        raise ValueError(f"display_name default Locale {default_locale!r} has no value")
    return LocalizedDisplayNames(default_locale, values)


def serialize_package_metadata(manifest: PackageManifest) -> bytes:
    payload = {
        "schema_version": PACKAGE_METADATA_VERSION,
        "package_type": "app",
        "display": manifest.display,
        "display_name": {
            "default": manifest.display_names.default_locale,
            "values": manifest.display_names.values,
        },
    }
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def validate_component_identifier(value: object, label: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[a-z][a-z0-9]*(?:[._-][a-z0-9]+)*", value):
        raise ValueError(f"{label} must be a lowercase dotted identifier")
    if len(value.encode("ascii")) > 63:
        raise ValueError(f"{label} must be at most 63 ASCII bytes")
    return value


def load_package_manifest(path: Path) -> PackageManifest:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise ValueError("package manifest must use schema_version 1")
    package_type = value.get("package_type", "app")
    if package_type == "app":
        try:
            app_id = value["app_id"]
            if not isinstance(app_id, str):
                raise TypeError
            localization = value.get("localization")
            fallback_default = localization.get("default", "en") if isinstance(localization, dict) else "en"
            display_name = parse_display_names(value["display_name"], fallback_default)
            display = value["display"]
            if display not in ("square", "landscape", "portrait"):
                raise ValueError("display must be one of: square, landscape, portrait")
            launch_asset = str(value.get("launch_asset", ""))
            if launch_asset:
                validate_asset_name(launch_asset, 0)
            return PackageManifest(app_id, display_name, launch_asset, display=display)
        except (KeyError, TypeError) as error:
            raise ValueError(
                "app manifest requires app_id, display_name, display and a valid launch_asset name"
            ) from error
    if package_type != "component" or value.get("component_type") != "font":
        raise ValueError("only package_type=component with component_type=font is supported")
    try:
        package_id = validate_component_identifier(value["id"], "component id")
        display_names = parse_display_names(value["display_name"])
        version = str(value["version"])
        if not re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", version):
            raise ValueError("component version must be canonical major.minor.patch")
        raw_languages = value["languages"]
        if not isinstance(raw_languages, list) or not raw_languages or len(raw_languages) > 16:
            raise ValueError("font component languages must contain 1..16 Locale tags")
        languages: list[str] = []
        for raw_locale in raw_languages:
            locale = normalize_locale_tag(raw_locale)
            if raw_locale != locale or locale in languages:
                raise ValueError("font component languages must be canonical and unique")
            languages.append(locale)
        font_bundle = validate_component_identifier(value["font_bundle"], "font_bundle")
        charset = validate_component_identifier(value["charset"], "charset")
        raw_fonts = value["fonts"]
        role_names = ("small", "medium", "large", "title")
        if not isinstance(raw_fonts, dict) or set(raw_fonts) != set(role_names):
            raise ValueError("font component fonts must define small, medium, large and title")
        font_roles: dict[str, dict[str, object]] = {}
        asset_names: set[str] = set()
        for role in role_names:
            record = raw_fonts[role]
            if not isinstance(record, dict):
                raise ValueError(f"font component role {role} must be an object")
            asset = validate_asset_name(record.get("asset"), 0)
            style = record.get("style")
            size = record.get("size")
            bpp = record.get("bpp")
            if style != "regular" or not isinstance(size, int) or isinstance(size, bool) or not 1 <= size <= 4096:
                raise ValueError(f"font component role {role} has invalid style or size")
            if not isinstance(bpp, int) or isinstance(bpp, bool) or bpp not in (1, 2, 4, 8):
                raise ValueError(f"font component role {role} has invalid bpp")
            if asset in asset_names:
                raise ValueError("font component roles must reference distinct assets")
            asset_names.add(asset)
            font_roles[role] = {"asset": asset, "style": style, "size": size, "bpp": bpp}
        return PackageManifest(package_id, display_names, "", "component", "font", version, tuple(languages),
                               font_bundle, charset, font_roles)
    except KeyError as error:
        raise ValueError(f"font component manifest is missing {error.args[0]}") from error


def serialize_component_metadata(manifest: PackageManifest) -> bytes:
    if manifest.package_type != "component" or manifest.component_type != "font" or manifest.font_roles is None:
        raise ValueError("component metadata requires a validated font component manifest")
    payload = {
        "schema_version": PACKAGE_METADATA_VERSION,
        "package_type": "component",
        "component_type": "font",
        "version": manifest.version,
        "display_name": {
            "default": manifest.display_names.default_locale,
            "values": manifest.display_names.values,
        },
        "languages": list(manifest.languages),
        "font_bundle": manifest.font_bundle,
        "charset": manifest.charset,
        "fonts": manifest.font_roles,
    }
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def load_app_manifest(path: Path) -> tuple[str, LocalizedDisplayNames, str]:
    manifest = load_package_manifest(path)
    if manifest.package_type != "app":
        raise ValueError("app manifest cannot describe a Component Package")
    return manifest.package_id, manifest.display_names, manifest.launch_asset


def resource_pack_digest(sections: list[InputSection], launch_asset_id: int) -> bytes:
    digest = hashlib.sha256()
    digest.update(b"MicroPixel resource pack catalog v1\0")
    digest.update(struct.pack("<I", launch_asset_id))
    for section in sorted(sections, key=lambda value: value.section_id):
        digest.update(struct.pack(
            "<IIIIII", section.section_id, section.format, section.width,
            section.height, section.stride, len(section.data),
        ))
        digest.update(hashlib.sha256(section.data).digest())
    return digest.digest()


def build_resource_pack(sections: list[InputSection], launch_asset: str) -> ResourcePack:
    ids = [section.section_id for section in sections]
    if len(ids) != len(set(ids)):
        raise ValueError("named assets have a 32-bit ID hash collision")
    names_to_ids = {
        section.name: section.section_id for section in sections if section.name is not None
    }
    if launch_asset and launch_asset not in names_to_ids:
        raise ValueError("launch_asset does not name a packaged resource")
    launch_asset_id = names_to_ids.get(launch_asset, 0)
    if launch_asset_id:
        launch_section = next(section for section in sections if section.section_id == launch_asset_id)
        if launch_section.format not in LAUNCH_FORMATS:
            raise ValueError("launch_asset must be JPEG or PNG")
    digest = resource_pack_digest(sections, launch_asset_id)
    return ResourcePack(sections, launch_asset_id, digest)


def serialize_resource_pack(resource_pack: ResourcePack) -> bytes:
    toc_offset = RESOURCE_PACK_HEADER.size
    cursor = align(
        toc_offset + len(resource_pack.sections) * SECTION.size,
        RESOURCE_PACK_ALIGNMENT,
    )
    entries: list[bytes] = []
    placements: list[tuple[int, bytes]] = []
    for section in resource_pack.sections:
        cursor = align(cursor, RESOURCE_PACK_ALIGNMENT)
        entries.append(SECTION.pack(
            section.kind, section.section_id, cursor, len(section.data),
            fnv1a32(section.data), section.format, section.width, section.height,
            section.stride, 0, 0, 0,
        ))
        placements.append((cursor, section.data))
        cursor += len(section.data)
    pack_size = align(cursor, RESOURCE_PACK_ALIGNMENT)
    header = RESOURCE_PACK_HEADER.pack(
        RESOURCE_PACK_MAGIC, RESOURCE_PACK_VERSION, RESOURCE_PACK_HEADER.size,
        pack_size, len(resource_pack.sections), toc_offset,
        resource_pack.launch_asset_id, resource_pack.digest,
    )
    image = bytearray(pack_size)
    image[:RESOURCE_PACK_HEADER.size] = header
    for index, entry in enumerate(entries):
        begin = toc_offset + index * SECTION.size
        image[begin : begin + SECTION.size] = entry
    for offset, data in placements:
        image[offset : offset + len(data)] = data
    return bytes(image)


def load_resource_pack(path: Path) -> ResourcePack:
    image = path.read_bytes()
    if len(image) < RESOURCE_PACK_HEADER.size:
        raise ValueError("resource pack is truncated")
    (magic, version, header_size, pack_size, section_count, toc_offset,
     launch_asset_id, expected_digest) = RESOURCE_PACK_HEADER.unpack_from(image)
    if magic != RESOURCE_PACK_MAGIC or version != RESOURCE_PACK_VERSION:
        raise ValueError("unsupported resource pack format")
    if (
        header_size != RESOURCE_PACK_HEADER.size
        or pack_size != len(image)
        or pack_size % RESOURCE_PACK_ALIGNMENT != 0
    ):
        raise ValueError("resource pack header or size is invalid")
    toc_end = toc_offset + section_count * SECTION.size
    payload_begin = align(toc_end, RESOURCE_PACK_ALIGNMENT)
    if toc_offset != header_size or toc_end > len(image):
        raise ValueError("resource pack TOC is invalid")

    sections: list[InputSection] = []
    occupied: list[tuple[int, int]] = []
    for index in range(section_count):
        fields = SECTION.unpack_from(image, toc_offset + index * SECTION.size)
        (kind, section_id, offset, size, content_hash, format_id, width,
         height, stride, reserved0, reserved1, reserved2) = fields
        if (
            kind not in (KIND_ASSET, KIND_FONT)
            or section_id == 0
            or size == 0
            or (kind == KIND_ASSET and format_id not in ASSET_FORMAT_IDS)
            or (kind == KIND_FONT and format_id != FORMATS["font_cbin"])
            or (kind == KIND_FONT and (width != 0 or height != 0 or stride != 0))
        ):
            raise ValueError("resource pack contains an invalid section")
        if reserved0 != 0 or reserved1 != 0 or reserved2 != 0:
            raise ValueError("resource pack contains unsupported section fields")
        if offset < payload_begin or offset % RESOURCE_PACK_ALIGNMENT != 0 or offset + size > len(image):
            raise ValueError("resource pack section range is invalid")
        data = image[offset : offset + size]
        if fnv1a32(data) != content_hash:
            raise ValueError("resource pack section content hash failed")
        occupied.append((offset, offset + size))
        sections.append(InputSection(
            kind, section_id, format_id, width, height, stride, data,
        ))

    ids = [section.section_id for section in sections]
    if len(ids) != len(set(ids)):
        raise ValueError("resource pack contains duplicate asset IDs")
    occupied.sort()
    if any(previous[1] > current[0] for previous, current in zip(occupied, occupied[1:])):
        raise ValueError("resource pack sections overlap")
    if launch_asset_id != 0:
        launch_sections = [section for section in sections if section.section_id == launch_asset_id]
        if not launch_sections:
            raise ValueError("resource pack launch asset is missing")
        if launch_sections[0].format not in LAUNCH_FORMATS:
            raise ValueError("resource pack launch asset must be JPEG or PNG")
    actual_digest = resource_pack_digest(sections, launch_asset_id)
    if actual_digest != expected_digest:
        raise ValueError("resource pack catalog digest failed")
    return ResourcePack(sections, launch_asset_id, actual_digest)


def cpp_identifier(name: str) -> str:
    return re.sub(r"[._-]", "_", name)


def render_asset_header(sections: list[InputSection], namespace: str, digest: bytes) -> str:
    if (
        not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", namespace)
        or namespace in CPP_KEYWORDS
    ):
        raise ValueError("asset C++ namespace must be a valid identifier")

    scalars: list[tuple[str, InputSection]] = []
    indexed: dict[str, list[tuple[int, InputSection]]] = {}
    for section in sections:
        if section.name is None:
            raise ValueError("C++ asset bindings require named manifest assets")
        match = re.fullmatch(r"(.+)[._-]([0-9]+)", section.name)
        if match is None:
            scalars.append((cpp_identifier(section.name), section))
            continue
        base, raw_index = match.groups()
        indexed.setdefault(cpp_identifier(base), []).append((int(raw_index), section))

    identifiers = {identifier for identifier, _ in scalars}
    if len(identifiers) != len(scalars):
        raise ValueError("asset names collapse to duplicate C++ identifiers")
    for identifier in identifiers:
        if identifier in CPP_KEYWORDS or identifier == "detail":
            raise ValueError(f"asset name produces reserved C++ identifier: {identifier}")
    for identifier, entries in indexed.items():
        if identifier in identifiers:
            raise ValueError(f"asset names collapse to duplicate C++ identifier: {identifier}")
        if identifier in CPP_KEYWORDS or identifier == "detail":
            raise ValueError(f"asset name produces reserved C++ identifier: {identifier}")
        identifiers.add(identifier)
        entries.sort(key=lambda item: item[0])
        expected = list(range(len(entries)))
        actual = [index for index, _ in entries]
        if actual != expected:
            raise ValueError(f"indexed asset group {identifier} must be contiguous from 0")

    atlas_groups: dict[str, list[InputSection]] = {}
    for section in sections:
        if section.atlas is None:
            continue
        group_identifier = cpp_identifier(section.atlas.group)
        if group_identifier in CPP_KEYWORDS or group_identifier == "detail":
            raise ValueError(f"atlas group produces reserved C++ identifier: {group_identifier}")
        atlas_groups.setdefault(group_identifier, []).append(section)
    for group_identifier, entries in atlas_groups.items():
        entries.sort(key=lambda section: section.atlas.index if section.atlas is not None else -1)
        actual = [section.atlas.index for section in entries if section.atlas is not None]
        if actual != list(range(len(entries))):
            raise ValueError(f"atlas group {group_identifier} indices must be contiguous from 0")
        canvases = {
            (section.atlas.canvas_width, section.atlas.canvas_height)
            for section in entries if section.atlas is not None
        }
        if len(canvases) != 1:
            raise ValueError(f"atlas group {group_identifier} must use one canvas size")
        frame_counts = {
            len(section.atlas.frames) for section in entries if section.atlas is not None
        }
        if len(frame_counts) != 1:
            raise ValueError(f"atlas group {group_identifier} must use one frame count")

    guard = f"MICROPIXEL_GENERATED_{namespace.upper()}_HPP"
    lines = [
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "#include <stdint.h>",
        "",
        '#include "sdk/resources.hpp"',
        "",
        f"namespace {namespace} {{",
        "",
    ]
    if atlas_groups:
        lines.extend([
            "struct AtlasFrame final {",
            "    uint16_t x;",
            "    uint16_t y;",
            "    uint16_t width;",
            "    uint16_t height;",
            "    int16_t canvas_x;",
            "    int16_t canvas_y;",
            "};",
            "",
            "struct Atlas final {",
            "    micropixel::AssetId asset;",
            "    uint16_t width;",
            "    uint16_t height;",
            "    const AtlasFrame* frames;",
            "};",
            "",
        ])
    lines.extend([
        "namespace detail {",
        "inline constexpr uint8_t resource_pack_digest[] = {",
    ])
    for offset in range(0, len(digest), 8):
        chunk = ", ".join(f"0x{value:02x}U" for value in digest[offset : offset + 8])
        lines.append(f"    {chunk},")
    lines.extend([
        "};",
        "}  // namespace detail",
        "",
    ])
    for identifier, section in scalars:
        lines.append(
            f"inline constexpr micropixel::AssetId {identifier}{{{section.section_id}U}};  "
            f'// "{section.name}"'
        )
    if scalars and indexed:
        lines.append("")
    for group_index, (identifier, entries) in enumerate(indexed.items()):
        lines.append(f"inline constexpr micropixel::AssetId {identifier}[] = {{")
        for _, section in entries:
            lines.append(
                f"    micropixel::AssetId{{{section.section_id}U}},  // \"{section.name}\""
            )
        lines.append("};")
        if group_index + 1 != len(indexed):
            lines.append("")
    if atlas_groups:
        lines.append("")
    for group_index, (group_identifier, entries) in enumerate(atlas_groups.items()):
        atlas = entries[0].atlas
        if atlas is None:
            raise ValueError("atlas metadata unexpectedly missing")
        lines.extend([
            f"inline constexpr uint32_t {group_identifier}_atlas_count = {len(entries)}U;",
            f"inline constexpr uint32_t {group_identifier}_atlas_frame_count = {len(atlas.frames)}U;",
            f"inline constexpr uint32_t {group_identifier}_canvas_width = {atlas.canvas_width}U;",
            f"inline constexpr uint32_t {group_identifier}_canvas_height = {atlas.canvas_height}U;",
            "",
        ])
        for section in entries:
            if section.atlas is None or section.name is None:
                raise ValueError("atlas bindings require named atlas assets")
            frames_identifier = f"{cpp_identifier(section.name)}_frames"
            lines.append(f"inline constexpr AtlasFrame {frames_identifier}[] = {{")
            for frame in section.atlas.frames:
                lines.append(
                    f"    {{{frame.x}U, {frame.y}U, {frame.width}U, {frame.height}U, "
                    f"{frame.canvas_x}, {frame.canvas_y}}},"
                )
            lines.extend([
                "};",
                f"static_assert(sizeof({frames_identifier}) / sizeof({frames_identifier}[0]) == "
                f"{group_identifier}_atlas_frame_count);",
                "",
            ])
        lines.append(f"inline constexpr Atlas {group_identifier}_atlases[] = {{")
        for section in entries:
            if section.name is None:
                raise ValueError("atlas bindings require named atlas assets")
            frames_identifier = f"{cpp_identifier(section.name)}_frames"
            lines.append(
                f"    {{{cpp_identifier(section.name)}, {section.width}U, {section.height}U, {frames_identifier}}},"
            )
        lines.extend([
            "};",
            f"static_assert(sizeof({group_identifier}_atlases) / sizeof({group_identifier}_atlases[0]) == "
            f"{group_identifier}_atlas_count);",
        ])
        if group_index + 1 != len(atlas_groups):
            lines.append("")
    lines.extend(["", f"}}  // namespace {namespace}", "", f"#endif  // {guard}", ""])
    return "\n".join(lines)


def stage_output(path: Path, data: bytes) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=path.parent,
        prefix=f".{path.name}.",
        suffix=".tmp",
    )
    temporary_path = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary_path, 0o644)
    except BaseException:
        temporary_path.unlink(missing_ok=True)
        raise
    return temporary_path


def write_prepared_assets(
    resource_pack_path: Path,
    resource_pack_data: bytes,
    header_path: Path,
    header_text: str,
) -> None:
    if resource_pack_path.resolve() == header_path.resolve():
        raise ValueError("resource pack and generated header must have different paths")
    staged_pack = stage_output(resource_pack_path, resource_pack_data)
    try:
        staged_header = stage_output(header_path, header_text.encode("utf-8"))
    except BaseException:
        staged_pack.unlink(missing_ok=True)
        raise
    try:
        os.replace(staged_header, header_path)
        os.replace(staged_pack, resource_pack_path)
    finally:
        staged_header.unlink(missing_ok=True)
        staged_pack.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(
        description=f"Prepare a resource pack or finalize a MicroPixel App Bundle v{VERSION}."
    )
    parser.add_argument("--aot", type=Path, help="AOT input for final Bundle mode")
    parser.add_argument("--app-id", help="App ID when no app manifest is used")
    parser.add_argument("--app-manifest", type=Path, help="App metadata and launch resource name")
    parser.add_argument("--asset-manifest", type=Path, help="named resources for prepare mode")
    parser.add_argument("--launch-asset", help="semantic name from --asset-manifest")
    parser.add_argument("--prepare-resource-pack", type=Path, help="resource pack output for prepare mode")
    parser.add_argument("--resource-pack", type=Path, help="prepared resources for final Bundle mode")
    parser.add_argument("--emit-cpp-header", type=Path, help="generated AssetId bindings for prepare mode")
    parser.add_argument("--cpp-namespace", help="namespace for generated AssetId bindings")
    parser.add_argument("--output", type=Path, help="final Bundle output")
    parser.add_argument(
        "--legacy-metadata-v1",
        action="store_true",
        help="emit the legacy single UTF-8 display name for compatibility tests",
    )
    args = parser.parse_args()

    if args.app_manifest is not None and args.app_id is not None:
        raise SystemExit("use either --app-id or --app-manifest, not both")
    if args.app_manifest is not None:
        try:
            package_manifest = load_package_manifest(args.app_manifest)
            app_id_text = package_manifest.package_id
            display_names = package_manifest.display_names
            manifest_launch_asset = package_manifest.launch_asset
        except (OSError, ValueError, json.JSONDecodeError) as error:
            raise SystemExit(str(error)) from error
    else:
        app_id_text = args.app_id or ""
        display_names = parse_display_names(app_id_text)
        manifest_launch_asset = ""
        package_manifest = PackageManifest(app_id_text, display_names, "")
    launch_asset = (
        args.launch_asset if args.launch_asset is not None else manifest_launch_asset
    )
    if launch_asset:
        try:
            validate_asset_name(launch_asset, 0)
        except ValueError as error:
            raise SystemExit(str(error)) from error

    if args.prepare_resource_pack is not None:
        if args.legacy_metadata_v1:
            raise SystemExit("--legacy-metadata-v1 is only valid for final Bundle builds")
        if args.aot is not None or args.output is not None or args.resource_pack is not None:
            raise SystemExit("resource preparation cannot also build a final Bundle")
        if (
            args.asset_manifest is None
            or args.emit_cpp_header is None
            or args.cpp_namespace is None
        ):
            raise SystemExit(
                "--prepare-resource-pack requires --asset-manifest, "
                "--emit-cpp-header and --cpp-namespace"
            )
        try:
            manifest_sections = load_asset_manifest(args.asset_manifest)
            resource_pack = build_resource_pack(manifest_sections, launch_asset)
            pack_data = serialize_resource_pack(resource_pack)
            header = render_asset_header(
                manifest_sections,
                args.cpp_namespace,
                resource_pack.digest,
            )
            write_prepared_assets(
                args.prepare_resource_pack,
                pack_data,
                args.emit_cpp_header,
                header,
            )
            verified = load_resource_pack(args.prepare_resource_pack)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            raise SystemExit(str(error)) from error
        print(
            f"Prepared resources v{RESOURCE_PACK_VERSION}: "
            f"assets={len(resource_pack.sections)} launch={launch_asset}"
            f"({resource_pack.launch_asset_id}) digest={verified.digest.hex()} "
            f"-> {args.prepare_resource_pack}, {args.emit_cpp_header}"
        )
        return

    if (
        args.asset_manifest is not None
        or args.emit_cpp_header is not None
        or args.cpp_namespace is not None
    ):
        raise SystemExit("final Bundle builds consume --resource-pack, not an asset manifest")
    if args.output is None:
        raise SystemExit("final Bundle builds require --output")
    if not app_id_text:
        raise SystemExit("one of --app-id or --app-manifest is required")

    app_id = app_id_text.encode("utf-8")
    if not re.fullmatch(rf"[A-Za-z0-9_.-]{{1,{APP_ID_MAX_LENGTH}}}", app_id_text):
        raise SystemExit(
            f"--app-id must be 1..{APP_ID_MAX_LENGTH} ASCII letters, digits, '.', '_' or '-'"
        )
    default_display_name = display_names.values[display_names.default_locale]
    if package_manifest.package_type == "component" and args.legacy_metadata_v1:
        raise SystemExit("Component Packages require typed metadata schema v1")
    if args.legacy_metadata_v1:
        metadata_format = FORMAT_UTF8
        metadata_payload = default_display_name.encode("utf-8")
        metadata_version = 1
    elif package_manifest.package_type == "component":
        metadata_format = FORMAT_PACKAGE_METADATA_JSON
        metadata_payload = serialize_component_metadata(package_manifest)
        metadata_version = PACKAGE_METADATA_VERSION
    else:
        metadata_format = FORMAT_PACKAGE_METADATA_JSON
        metadata_payload = serialize_package_metadata(package_manifest)
        metadata_version = PACKAGE_METADATA_VERSION

    sections = [InputSection(KIND_APP_METADATA, 0, metadata_format, 0, 0, 0, metadata_payload)]
    if package_manifest.package_type == "app":
        if args.aot is None:
            raise SystemExit("App Bundle builds require --aot")
        aot = args.aot.read_bytes()
        if not aot:
            raise SystemExit("AOT input is empty")
        sections.insert(0, InputSection(KIND_AOT, 0, FORMATS["aot"], 0, 0, 0, aot))
    elif args.aot is not None:
        raise SystemExit("Component Packages cannot contain AOT/Wasm")
    launch_asset_id = 0
    resource_digest = bytes(32)
    if args.resource_pack is not None:
        try:
            resource_pack = load_resource_pack(args.resource_pack)
        except (OSError, ValueError) as error:
            raise SystemExit(str(error)) from error
        expected_launch_id = fnv1a32(launch_asset.encode("ascii")) if launch_asset else 0
        if resource_pack.launch_asset_id != expected_launch_id:
            raise SystemExit("resource pack launch asset disagrees with the app manifest")
        sections.extend(resource_pack.sections)
        launch_asset_id = resource_pack.launch_asset_id
        resource_digest = resource_pack.digest
    elif launch_asset:
        raise SystemExit("app manifest names a launch asset but no --resource-pack was supplied")
    if package_manifest.package_type == "component":
        if args.resource_pack is None or package_manifest.font_roles is None:
            raise SystemExit("font Component Packages require a prepared resource pack")
        expected_font_ids = {
            fnv1a32(str(record["asset"]).encode("ascii")) for record in package_manifest.font_roles.values()
        }
        actual_font_ids = {section.section_id for section in resource_pack.sections if section.kind == KIND_FONT}
        if expected_font_ids != actual_font_ids or len(resource_pack.sections) != len(actual_font_ids):
            raise SystemExit("font Component resource pack must contain exactly its four declared font_cbin assets")

    toc_offset = HEADER.size
    cursor = align(toc_offset + len(sections) * SECTION.size, 64)
    entries: list[bytes] = []
    placements: list[tuple[int, bytes]] = []
    for section in sections:
        cursor = align(cursor, 64)
        entries.append(SECTION.pack(
            section.kind, section.section_id, cursor, len(section.data),
            fnv1a32(section.data), section.format, section.width, section.height,
            section.stride, 0, 0, 0,
        ))
        placements.append((cursor, section.data))
        cursor += len(section.data)
    bundle_size = align(cursor, EXTENT_ALIGNMENT)

    app_id_field = app_id + bytes(APP_ID_MAX_LENGTH - len(app_id))
    header_without_hash = HEADER.pack(
        MAGIC, VERSION, HEADER.size, bundle_size, toc_offset, app_id_field, len(app_id),
        len(sections), FRAMEWORK_ABI_VERSION, launch_asset_id, 0, 0, 0, 0, 0, 0,
    )
    header = HEADER.pack(
        MAGIC, VERSION, HEADER.size, bundle_size, toc_offset, app_id_field, len(app_id),
        len(sections), FRAMEWORK_ABI_VERSION, launch_asset_id,
        fnv1a32(header_without_hash), 0, 0, 0, 0, 0,
    )
    image = bytearray(bundle_size)
    image[:HEADER.size] = header
    for index, entry in enumerate(entries):
        begin = toc_offset + index * SECTION.size
        image[begin : begin + SECTION.size] = entry
    for offset, data in placements:
        image[offset : offset + len(data)] = data

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(
        f"Bundle v{VERSION}: type={package_manifest.package_type} id={app_id_text} "
        f"title={default_display_name!r} metadata=v{metadata_version} "
        f"locales={len(display_names.values)} sections={len(sections)} "
        f"content={cursor} extent={bundle_size} launch={launch_asset}({launch_asset_id}) "
        f"resources={len(sections) - (2 if package_manifest.package_type == 'app' else 1)} "
        f"digest={resource_digest.hex()} "
        f"-> {args.output}"
    )


if __name__ == "__main__":
    main()
