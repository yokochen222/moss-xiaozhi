import unittest


def is_valid_wake_word_pinyin(command: str) -> bool:
    max_len = 64
    if not command or len(command) > max_len:
        return False
    if command[0] == " " or command[-1] == " ":
        return False
    for i, ch in enumerate(command):
        if "A" <= ch <= "Z":
            return False
        if ch == " ":
            if i + 1 >= len(command) or command[i + 1] == " ":
                return False
            continue
        if ch < "a" or ch > "z":
            return False
    return True


class WakeWordPinyinValidationTest(unittest.TestCase):
    def test_accepts_valid_pinyin(self):
        self.assertTrue(is_valid_wake_word_pinyin("ni hao"))
        self.assertTrue(is_valid_wake_word_pinyin("xiao zhi"))
        self.assertTrue(is_valid_wake_word_pinyin("mo si"))

    def test_rejects_symbols_and_digits(self):
        for value in (
            "ni-hao",
            "ni.hao",
            "ni_hao",
            "ni!hao",
            "ni3 hao",
            "ni@hao",
            "ni/hao",
            "  ni hao",
            "ni hao ",
            "ni  hao",
            "NI hao",
            "你好",
            "",
        ):
            with self.subTest(value=value):
                self.assertFalse(is_valid_wake_word_pinyin(value))


if __name__ == "__main__":
    unittest.main()
