// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
//
// Read-only text model for the recovery text/code viewer (File manager > Open).
// No graphics dependency, unit tested on the host. Characters are counted per UTF-8
// sequence; the recovery font shows non-ASCII characters as '?'.
#pragma once

#include <stddef.h>

#include <string>
#include <vector>

namespace recovery::viewer {

// One screen row: characters [start, end) (character indexes, not bytes) of lines[line].
// first is true for the row that starts the line, which carries the line number.
struct Row {
  size_t line;
  size_t start;
  size_t end;
  bool first;
};

class TextView {
 public:
  // Fails for binary data (NUL bytes). Line endings LF and CRLF are accepted.
  bool Load(const std::string& data, std::string* error);

  size_t line_count() const { return lines_.size(); }
  // Line as display cells: one character per cell, non-ASCII and controls as '?',
  // tab as a space.
  const std::string& line(size_t index) const { return lines_[index]; }
  // Widest line in cells (for horizontal scrolling without wrap).
  size_t max_width() const { return max_width_; }
  // Digits of the largest line number.
  size_t NumberWidth() const;

  // Rows for `cols` text columns. With wrap, long lines continue on the next rows;
  // without, every line is one row covering the whole line.
  std::vector<Row> Layout(size_t cols, bool wrap) const;

 private:
  std::vector<std::string> lines_{""};
  size_t max_width_ = 0;
};

}  // namespace recovery::viewer
