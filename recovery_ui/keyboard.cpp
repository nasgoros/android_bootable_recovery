// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0

#include "recovery_ui/keyboard.h"

namespace recovery::keyboard {
namespace {

std::vector<Key> CharRow(const std::string& chars) {
  std::vector<Key> row;
  for (char c : chars) row.push_back({KeyType::kChar, std::string(1, c), std::string(1, c), 1.0f});
  return row;
}

Key Special(KeyType type, const std::string& label, float weight) {
  return {type, label, "", weight};
}

}  // namespace

std::vector<std::vector<Key>> Rows(Mode mode, bool terminal) {
  std::vector<std::vector<Key>> rows;
  if (terminal) {
    rows.push_back({Special(KeyType::kExit, "Exit", 1.2f), Special(KeyType::kEscape, "Esc", 1.0f),
                    Special(KeyType::kTab, "Tab", 1.0f), Special(KeyType::kCtrl, "Ctrl", 1.0f),
                    Special(KeyType::kLeft, "Lt", 1.0f), Special(KeyType::kUp, "Up", 1.0f),
                    Special(KeyType::kDown, "Dn", 1.0f), Special(KeyType::kRight, "Rt", 1.0f)});
  }

  const bool upper = mode == Mode::kUpper;
  if (mode == Mode::kLower || mode == Mode::kUpper) {
    rows.push_back(CharRow(upper ? "QWERTYUIOP" : "qwertyuiop"));
    rows.push_back(CharRow(upper ? "ASDFGHJKL" : "asdfghjkl"));
    std::vector<Key> row{Special(KeyType::kShift, upper ? "SHIFT" : "Shift", 1.5f)};
    for (const auto& key : CharRow(upper ? "ZXCVBNM" : "zxcvbnm")) row.push_back(key);
    row.push_back(Special(KeyType::kBackspace, "Del", 1.5f));
    rows.push_back(row);
  } else if (mode == Mode::kSymbols) {
    rows.push_back(CharRow("1234567890"));
    rows.push_back(CharRow("-/:;()$&@\""));
    std::vector<Key> row{Special(KeyType::kMoreSymbols, "#+=", 1.5f)};
    for (const auto& key : CharRow(".,?!'*_")) row.push_back(key);
    row.push_back(Special(KeyType::kBackspace, "Del", 1.5f));
    rows.push_back(row);
  } else {
    rows.push_back(CharRow("[]{}#%^*+="));
    rows.push_back(CharRow("_\\|~<>`'?!"));
    std::vector<Key> row{Special(KeyType::kMoreSymbols, "123", 1.5f)};
    for (const auto& key : CharRow(".,:;\"$&")) row.push_back(key);
    row.push_back(Special(KeyType::kBackspace, "Del", 1.5f));
    rows.push_back(row);
  }

  const bool letters = mode == Mode::kLower || mode == Mode::kUpper;
  std::vector<Key> bottom{Special(KeyType::kSymbols, letters ? "?123" : "ABC", 1.5f)};
  if (!terminal) bottom.push_back(Special(KeyType::kCancel, "Cancel", 1.6f));
  bottom.push_back({KeyType::kChar, "/", "/", 1.0f});
  bottom.push_back(Special(KeyType::kSpace, "space", terminal ? 4.5f : 3.0f));
  bottom.push_back({KeyType::kChar, ".", ".", 1.0f});
  bottom.push_back(Special(KeyType::kEnter, terminal ? "Enter" : "Done", 1.6f));
  rows.push_back(bottom);
  return rows;
}

std::vector<PlacedKey> Layout(Mode mode, bool terminal, int x, int y, int w, int h, int gap) {
  const auto rows = Rows(mode, terminal);
  std::vector<PlacedKey> placed;
  if (rows.empty() || w <= 0 || h <= 0) return placed;
  const int row_count = static_cast<int>(rows.size());
  const int row_height = (h - gap * (row_count + 1)) / row_count;
  for (int r = 0; r < row_count; ++r) {
    const auto& row = rows[r];
    float total = 0;
    for (const auto& key : row) total += key.weight;
    const int usable = w - gap * (static_cast<int>(row.size()) + 1);
    const int key_y = y + gap + r * (row_height + gap);
    float cursor = static_cast<float>(x + gap);
    for (size_t k = 0; k < row.size(); ++k) {
      const int key_x = static_cast<int>(cursor);
      int key_w = static_cast<int>(usable * row[k].weight / total);
      // The last key absorbs rounding so every row spans the full width.
      if (k + 1 == row.size()) key_w = x + w - gap - key_x;
      placed.push_back({row[k], key_x, key_y, key_w, row_height});
      cursor += usable * row[k].weight / total + gap;
    }
  }
  return placed;
}

const PlacedKey* HitTest(const std::vector<PlacedKey>& keys, int px, int py) {
  // Gaps belong to no key; pick the nearest key in the touched row instead so a
  // slightly imprecise tap still registers.
  const PlacedKey* best = nullptr;
  int best_distance = 0;
  for (const auto& key : keys) {
    if (py < key.y || py >= key.y + key.h) continue;
    int distance = px < key.x ? key.x - px : px >= key.x + key.w ? px - (key.x + key.w - 1) : 0;
    if (!best || distance < best_distance) {
      best = &key;
      best_distance = distance;
    }
  }
  return best;
}

Mode NextMode(Mode mode, KeyType pressed) {
  switch (pressed) {
    case KeyType::kShift:
      return mode == Mode::kLower ? Mode::kUpper : mode == Mode::kUpper ? Mode::kLower : mode;
    case KeyType::kSymbols:
      return (mode == Mode::kLower || mode == Mode::kUpper) ? Mode::kSymbols : Mode::kLower;
    case KeyType::kMoreSymbols:
      return mode == Mode::kSymbols ? Mode::kSymbols2 : Mode::kSymbols;
    default:
      return mode;
  }
}

void EraseLastCharacter(std::string* text) {
  if (text->empty()) return;
  size_t last = text->size() - 1;
  while (last > 0 && (static_cast<unsigned char>((*text)[last]) & 0xc0) == 0x80) --last;
  text->resize(last);
}

}  // namespace recovery::keyboard
