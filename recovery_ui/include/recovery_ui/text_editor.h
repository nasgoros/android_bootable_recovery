// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
//
// Text buffer for the recovery text editor. No graphics dependency, unit tested on
// the host. Bytes are kept as they are: only characters the user deletes or types
// change, so UTF-8 text survives even though the recovery font shows it as '?'.
#pragma once

#include <stddef.h>

#include <string>
#include <vector>

namespace recovery::editor {

// One screen row of a soft-wrapped line: bytes [start, end) of lines[line].
struct VisualRow {
  size_t line;
  size_t start;
  size_t end;
};

class TextBuffer {
 public:
  TextBuffer() : lines_{""} {}

  // Loads text. Fails (and keeps the previous content) for binary data (NUL bytes).
  bool Load(const std::string& data, std::string* error);
  // Text with the original line endings (LF or CRLF) and final newline.
  std::string Serialize() const;

  bool modified() const { return modified_; }
  void set_modified(bool modified) { modified_ = modified; }
  bool crlf() const { return crlf_; }
  size_t line_count() const { return lines_.size(); }
  const std::string& line(size_t index) const { return lines_[index]; }
  size_t cursor_line() const { return cursor_line_; }
  size_t cursor_byte() const { return cursor_byte_; }
  // 1-based column in characters, for the status line.
  size_t CursorColumn() const;

  // Inserts typed text at the cursor; '\n' starts a new line.
  void Insert(const std::string& text);
  void Newline();
  // Deletes the character before the cursor (joins lines at the start of a line).
  void Backspace();
  void MoveLeft();
  void MoveRight();
  void Home();
  void End();
  // Moves by `rows` screen rows (negative: up) in a layout of `cols` columns.
  void MoveRows(int rows, size_t cols);

  // Soft-wrapped screen rows for `cols` columns (at least one row per line).
  std::vector<VisualRow> Layout(size_t cols) const;
  // Row index of the cursor in `rows` and its column within that row.
  size_t CursorRow(const std::vector<VisualRow>& rows) const;
  size_t CursorRowColumn(const std::vector<VisualRow>& rows) const;
  // Places the cursor at screen position (row, col) of `rows` (e.g. a tap).
  void SetCursor(const std::vector<VisualRow>& rows, size_t row, size_t col);

  // Printable text for bytes [start, end) of a line: one cell per character,
  // non-ASCII and control characters as '?', tab as a space.
  static std::string Display(const std::string& line, size_t start, size_t end);
  // Byte length of the UTF-8 character starting at pos (1 for invalid bytes).
  static size_t CharLength(const std::string& text, size_t pos);

 private:
  size_t PrevCharStart(size_t pos) const;

  std::vector<std::string> lines_;
  size_t cursor_line_ = 0;
  size_t cursor_byte_ = 0;
  bool crlf_ = false;
  bool final_newline_ = false;
  bool modified_ = false;
};

}  // namespace recovery::editor
