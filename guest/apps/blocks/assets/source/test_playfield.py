"""Artwork contract checks for the static Blocks scene atlas."""
from pathlib import Path
import unittest
from PIL import Image

ROOT = Path(__file__).resolve().parent


class PlayfieldArtworkTest(unittest.TestCase):
    def test_all_pieces_keep_round_corners_highlight_and_hollow_ghost(self):
        with Image.open(ROOT / 'playfield_atlas.png') as atlas:
            self.assertEqual(atlas.size, (240, 270))
            for piece in range(7):
                x = piece * 30
                self.assertEqual(atlas.getpixel((x + 1, 1))[3], 0)
                fill = atlas.getpixel((x + 15, 15))
                highlight = atlas.getpixel((x + 15, 3))
                self.assertEqual(fill[3], 255)
                self.assertGreater(sum(highlight[:3]), sum(fill[:3]))
                # The entire original 22x22 Ghost interior must show the board.
                self.assertEqual(atlas.crop((x + 4, 34, x + 26, 56)).getchannel('A').getbbox(), None)
                self.assertEqual(atlas.getpixel((x + 2, 45))[3], 255)
                self.assertEqual(atlas.getpixel((x + 1, 31))[3], 0)
                self.assertGreater(sum(atlas.getpixel((x + 15, 75))[:3]),
                                   sum(atlas.getpixel((x + 15, 105))[:3]))

    def test_clear_flash_has_eight_distinct_steps_for_every_theme(self):
        with Image.open(ROOT / 'playfield_atlas.png') as atlas:
            for theme in range(5):
                samples = [atlas.getpixel((phase * 30 + 15, 120 + theme * 30 + 15)) for phase in range(8)]
                self.assertEqual(len(set(samples)), 8)
                self.assertTrue(all(color[3] == 255 for color in samples))
                self.assertTrue(all(sum(a[:3]) < sum(b[:3]) for a, b in zip(samples, samples[1:])))

    def test_background_profiles_preserve_border_and_grid(self):
        for stroke in (1, 2):
            with Image.open(ROOT / f'playfield_background_{stroke}.png') as board:
                self.assertEqual(board.size, (300, 600))
                self.assertEqual(board.getpixel((0, 0)), (5, 5, 5))
                self.assertEqual(board.getpixel((150, 0)), (38, 38, 38))
                self.assertEqual(board.getpixel((15, 15)), (16, 16, 16))
                for offset in range(stroke):
                    self.assertEqual(board.getpixel((30 + offset, 15)), (32, 32, 32))
                self.assertEqual(board.getpixel((30 + stroke, 15)), (16, 16, 16))


if __name__ == '__main__':
    unittest.main()
