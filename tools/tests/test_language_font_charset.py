# SPDX-License-Identifier: Apache-2.0
import importlib.util
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('language_fonts', Path(__file__).parents[1] / 'fonts/build_language_fonts.py')
fonts = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fonts)


def encoded(data):
    values = list(range(33, 127)) + list(range(161, 173)) + list(range(174, 256))
    mapping = {value: chr(value) for value in values}
    remaining = [value for value in range(256) if value not in mapping]
    mapping.update({value: chr(256 + index) for index, value in enumerate(remaining)})
    return ''.join(mapping[value] for value in data)


class CharsetTests(unittest.TestCase):
    def extract(self, tokens):
        return fonts.tokenizer_charset({'model': {'vocab': {token: index for index, token in enumerate(tokens)}}})

    def test_byte_bpe_decodes_before_collecting_characters(self):
        self.assertEqual(self.extract([encoded('観 café 한국'.encode())]), set(map(ord, '観 café 한국')))

    def test_partial_tokens_do_not_imply_unsupported_model_characters(self):
        data = '観'.encode()
        self.assertNotIn(ord('観'), self.extract([encoded(data[:1]), encoded(data[1:])]))
        self.assertIn(ord('観'), self.extract([encoded(data)]))

    def test_special_and_control_tokens_are_excluded(self):
        self.assertEqual(self.extract(['<｜begin▁of▁sentence｜>', encoded(b'A\x00\n\t')]), {ord('A')})

    def test_fragment_does_not_discard_adjacent_complete_characters(self):
        self.assertEqual(self.extract([encoded(b'\xe8hello\xe8\xa6')]), set(map(ord, 'hello')))

    def test_basic_repertoires_cover_common_hangul(self):
        common = fonts.basic_charset('ko-KR')
        self.assertEqual(len(common - set(range(32, 127))), 2350)
        self.assertTrue(set(map(ord, '꺼꼴됨먼앱짐')).issubset(common))
        self.assertEqual(len(fonts.basic_charset('zh-CN')) - 95, 3755)
        self.assertEqual(len(fonts.basic_charset('zh-TW')) - 95, 5401)

    def test_union_preserves_both_vocabularies(self):
        first = self.extract([encoded('中文'.encode())])
        second = self.extract([encoded('観日'.encode())])
        self.assertEqual(first | second, set(map(ord, '中文観日')))


if __name__ == '__main__':
    unittest.main()
