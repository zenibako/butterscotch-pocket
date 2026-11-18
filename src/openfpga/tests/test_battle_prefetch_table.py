"""Sound selection keeps first use across writers and other objects."""
from pathlib import Path
import importlib.util
import unittest

path = Path(__file__).resolve().parents[1] / 'tools/battle_prefetch_table.py'
spec = importlib.util.spec_from_file_location('battle_table', path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class SoundOrder(unittest.TestCase):
    def test_earlier_non_writer_start_wins(self):
        self.assertEqual(module.select_sounds({'voice': 200}, {'voice': 10}, set(), 150), {'voice': 10})

    def test_selected_voice_keeps_first_use_beyond_generic_window(self):
        self.assertEqual(module.select_sounds({'voice': 200}, {'voice': 170}, set(), 150), {'voice': 170})

    def test_selection_filters_and_minimum_are_preserved(self):
        self.assertEqual(module.select_sounds({'voice': 100, 'common_voice': 200},
                         {'voice': 130, 'early': 10, 'late': 150, 'common': 0, 'common_voice': 0},
                         {'common', 'common_voice'}, 150),
                         {'voice': 100, 'common_voice': 0, 'early': 10})

if __name__ == '__main__':
    unittest.main()
