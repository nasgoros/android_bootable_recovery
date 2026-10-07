// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
//
// On-screen keyboard model for recovery: layout, geometry and hit testing.
// It has no graphics dependency, so it can be unit tested on the host;
// ScreenRecoveryUI draws it with minui.
#pragma once

#include <string>
#include <vector>

namespace recovery::keyboard {

enum class Mode { kLower, kUpper, kSymbols, kSymbols2 };

// Where the keyboard is used; decides the extra control row and the bottom row.
enum class Variant { kTextEntry, kTerminal, kEditor };

enum class KeyType {
  kChar,       // inserts text()
  kShift,      // lower <-> upper
  kSymbols,    // letters <-> symbols
  kMoreSymbols,// symbols <-> symbols 2
  kBackspace,
  kSpace,
  kEnter,      // newline in the terminal, "Done" in text entry
  kCancel,     // text entry only
  // Terminal row
  kExit,
  kEscape,
  kTab,
  kCtrl,
  kUp,
  kDown,
  kLeft,
  kRight,
  // Editor row (also uses kExit and the arrows)
  kSave,
  kHome,
  kEnd,
  kPageUp,
  kPageDown,
};

struct Key {
  KeyType type;
  std::string label;  // ASCII only: the recovery font has no other glyphs
  std::string text;   // inserted text for kChar
  float weight;       // relative width within its row
};

struct PlacedKey {
  Key key;
  int x, y, w, h;
};

// Rows for the given mode. The terminal and editor variants add a control row on
// top and use Enter (newline) instead of Cancel/Done.
std::vector<std::vector<Key>> Rows(Mode mode, Variant variant);

// Places the rows inside the rectangle (x, y, w, h) with `gap` pixels between keys.
std::vector<PlacedKey> Layout(Mode mode, Variant variant, int x, int y, int w, int h, int gap);

// Returns the key at (px, py), or nullptr.
const PlacedKey* HitTest(const std::vector<PlacedKey>& keys, int px, int py);

// Mode after pressing a mode key in `mode`.
Mode NextMode(Mode mode, KeyType pressed);

// Removes the last UTF-8 character from text (no-op when empty).
void EraseLastCharacter(std::string* text);

}  // namespace recovery::keyboard
