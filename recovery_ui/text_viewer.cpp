// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0

#include "recovery_ui/text_viewer.h"

#include <algorithm>

#include "recovery_ui/text_editor.h"

namespace recovery::viewer {

bool TextView::Load(const std::string& data, std::string* error) {
  if (data.find('\0') != std::string::npos) {
    *error = "This looks like a binary file; it cannot be shown as text.";
    return false;
  }
  std::vector<std::string> lines;
  size_t max_width = 0;
  size_t start = 0;
  while (start <= data.size()) {
    size_t end = data.find('\n', start);
    const bool last = end == std::string::npos;
    if (last) end = data.size();
    size_t len = end - start;
    if (len > 0 && data[start + len - 1] == '\r') --len;
    if (!(last && start == data.size() && !lines.empty())) {  // no row after a final '\n'
      std::string cells = recovery::editor::TextBuffer::Display(data.substr(start, len), 0, len);
      max_width = std::max(max_width, cells.size());
      lines.push_back(std::move(cells));
    }
    if (last) break;
    start = end + 1;
  }
  if (lines.empty()) lines.push_back("");
  lines_ = std::move(lines);
  max_width_ = max_width;
  return true;
}

size_t TextView::NumberWidth() const {
  return std::to_string(lines_.size()).size();
}

std::vector<Row> TextView::Layout(size_t cols, bool wrap) const {
  std::vector<Row> rows;
  cols = std::max<size_t>(cols, 1);
  rows.reserve(lines_.size());
  for (size_t i = 0; i < lines_.size(); ++i) {
    const size_t width = lines_[i].size();
    if (!wrap || width <= cols) {
      rows.push_back({i, 0, width, true});
      continue;
    }
    for (size_t start = 0; start < width; start += cols) {
      rows.push_back({i, start, std::min(width, start + cols), start == 0});
    }
  }
  return rows;
}

}  // namespace recovery::viewer
