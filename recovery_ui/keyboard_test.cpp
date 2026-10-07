// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
// Standalone host test (from the repository root):
//   g++ -std=c++17 -Wall -Wextra -Werror -Irecovery_ui/include
//       recovery_ui/keyboard.cpp recovery_ui/keyboard_test.cpp -o /tmp/keyboard-test
#include "recovery_ui/keyboard.h"

#include <cassert>
#include <iostream>
#include <map>

using namespace recovery::keyboard;

int main() {
  const Mode modes[] = {Mode::kLower, Mode::kUpper, Mode::kSymbols, Mode::kSymbols2};
  for (bool terminal : {false, true}) {
    for (Mode mode : modes) {
      const int x = 10, y = 1000, w = 1080, h = 900, gap = 6;
      auto keys = Layout(mode, terminal, x, y, w, h, gap);
      auto rows = Rows(mode, terminal);
      size_t count = 0;
      for (const auto& row : rows) count += row.size();
      assert(keys.size() == count);
      std::map<int, std::vector<const PlacedKey*>> by_row;
      for (const auto& key : keys) {
        assert(key.w > 0 && key.h > 0);
        assert(key.x >= x && key.x + key.w <= x + w);
        assert(key.y >= y && key.y + key.h <= y + h);
        assert(!key.key.label.empty());
        for (char c : key.key.label) assert(c >= 32 && c < 127);  // font is ASCII only
        by_row[key.y].push_back(&key);
      }
      assert(by_row.size() == rows.size());
      for (const auto& [row_y, row] : by_row) {
        (void)row_y;
        // Keys in a row do not overlap and the row ends at the right edge.
        for (size_t i = 1; i < row.size(); ++i) assert(row[i]->x >= row[i - 1]->x + row[i - 1]->w);
        assert(row.back()->x + row.back()->w == x + w - gap);
        // Every key is hit at its centre.
        for (const auto* key : row) {
          const PlacedKey* hit = HitTest(keys, key->x + key->w / 2, key->y + key->h / 2);
          assert(hit == key);
        }
        // A tap in the gap between two keys still selects one of them.
        if (row.size() > 1) {
          const PlacedKey* hit = HitTest(keys, row[0]->x + row[0]->w + gap / 2, row[0]->y + 2);
          assert(hit == row[0] || hit == row[1]);
        }
      }
      // Outside the keyboard nothing is hit.
      assert(HitTest(keys, x + w / 2, y - 50) == nullptr);
      // Text entry has Cancel and Done, the terminal has Exit and Enter.
      bool has_cancel = false, has_exit = false, has_enter = false;
      for (const auto& key : keys) {
        has_cancel |= key.key.type == KeyType::kCancel;
        has_exit |= key.key.type == KeyType::kExit;
        has_enter |= key.key.type == KeyType::kEnter;
      }
      assert(has_cancel == !terminal && has_exit == terminal && has_enter);
    }
  }
  assert(NextMode(Mode::kLower, KeyType::kShift) == Mode::kUpper);
  assert(NextMode(Mode::kUpper, KeyType::kShift) == Mode::kLower);
  assert(NextMode(Mode::kUpper, KeyType::kSymbols) == Mode::kSymbols);
  assert(NextMode(Mode::kSymbols, KeyType::kSymbols) == Mode::kLower);
  assert(NextMode(Mode::kSymbols, KeyType::kMoreSymbols) == Mode::kSymbols2);
  assert(NextMode(Mode::kSymbols2, KeyType::kMoreSymbols) == Mode::kSymbols);
  std::string text = "ab\xc3\xa9";  // "abé"
  EraseLastCharacter(&text);
  assert(text == "ab");
  EraseLastCharacter(&text);
  EraseLastCharacter(&text);
  EraseLastCharacter(&text);
  assert(text.empty());
  std::cout << "PASS: keyboard layout, hit testing, modes, UTF-8 backspace\n";
  return 0;
}
