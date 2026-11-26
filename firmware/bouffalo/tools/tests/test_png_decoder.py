from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib


def png(width, height, alpha):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows.extend((x % 256, y % 256, (x + y) % 256, x % 255) if alpha else (255, 0, 0))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6 if alpha else 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


class StreamingPng(unittest.TestCase):
    @unittest.skipUnless(shutil.which('g++') and shutil.which('gcc'), 'requires native C/C++ compilers')
    def test_large_atlas_and_failure_cleanup(self):
        root = Path(__file__).resolve().parents[2]
        upstream = root.parents[1] / 'firmware/espressif/main'
        png_root, zroot = root / 'third_party/libpng', root / 'third_party/zlib'
        pnames = 'png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil pngset pngtrans'.split()
        znames = 'adler32 crc32 inffast inflate inftrees zutil'.split()
        sources = [png_root / (n+'.c') for n in pnames] + [zroot / (n+'.c') for n in znames]
        with tempfile.TemporaryDirectory(prefix='mpx-png-test-') as temp:
            temp = Path(temp)
            (temp/'atlas.png').write_bytes(png(1024, 1632, True))
            (temp/'rgb.png').write_bytes(png(3, 2, False))
            subprocess.run(['gcc', '-O1', '-ffunction-sections', '-fdata-sections', '-c', '-DZ_PREFIX=1', '-DPNG_ARM_NEON_OPT=0',
                            '-DPNG_POWERPC_VSX_OPT=0', '-DPNG_INTEL_SSE_OPT=0', '-DPNG_RISCV_RVV_OPT=0',
                            '-I'+str(png_root), '-I'+str(zroot), *map(str, sources)], cwd=temp, check=True)
            subprocess.run(['g++', '-O1', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                            '-std=c++23', '-I'+str(root/'tools/tests/png_stubs'),
                            '-I'+str(root/'common/include'), '-I'+str(root.parents[1]/'guest'),
                            '-I'+str(upstream), '-I'+str(png_root),
                            '-I'+str(zroot), '-I'+str(Path(__import__('os').environ['BL_SDK_BASE'])/'components/graphics/lvgl_v9/libs/gltf/stb_image'),
                            str(root/'common/png_decoder.cpp'), str(root/'common/bitmap_decoder.cpp'),
                            str(upstream/'runtime/bundle/bundle_section_reader.cpp'),
                            str(root/'tools/tests/png_decoder_test.cpp'), *map(str, temp.glob('*.o')),
                            '-o', str(temp/'png-test')], check=True)
            subprocess.run([str(temp/'png-test'), str(temp/'atlas.png'), str(temp/'rgb.png')], check=True)
