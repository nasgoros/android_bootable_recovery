// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0

#include "recovery_ui/text_editor.h"

#include <algorithm>

namespace recovery::editor {

size_t TextBuffer::CharLength(const std::string& text, size_t pos) {
  const unsigned char c = static_cast<unsigned char>(text[pos]);
  size_t length = c >= 0xf0 && c < 0xf8 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
  if (length == 1) return 1;
  // Only accept the sequence when all continuation bytes are present.
  if (pos + length > text.size()) return 1;
  for (size_t i = 1; i < length; ++i) {
    if ((static_cast<unsigned char>(text[pos + i]) & 0xc0) != 0x80) return 1;
  }
  return length;
}

bool TextBuffer::Load(const std::string& data, std::string* error) {
  if (data.find('\0') != std::string::npos) {
    *error = "This looks like a binary file; it cannot be edited.";
    return false;
  }
  std::vector<std::string> lines;
  size_t crlf_count = 0, lf_count = 0;
  size_t start = 0;
  while (start <= data.size()) {
    size_t end = data.find('\n', start);
    if (end == std::string::npos) {
      lines.push_back(data.substr(start));
      break;
    }
    ++lf_count;
    if (end > start && data[end - 1] == '\r') ++crlf_count;
    lines.push_back(data.substr(start, end - start));
    start = end + 1;
  }
  const bool final_newline = !data.empty() && data.back() == '\n';
  if (final_newline) lines.pop_back();  // the empty piece after the last '\n'
  if (lines.empty()) lines.push_back("");
  // CRLF only when every line break is CRLF, so mixed files are not rewritten.
  const bool crlf = lf_count > 0 && crlf_count == lf_count;
  if (crlf) {
    const size_t last = final_newline ? lines.size() : lines.size() - 1;
    for (size_t i = 0; i < last; ++i) lines[i].pop_back();  // drop the '\r'
  }
  lines_ = std::move(lines);
  crlf_ = crlf;
  final_newline_ = final_newline;
  cursor_line_ = cursor_byte_ = 0;
  modified_ = false;
  return true;
}

std::string TextBuffer::Serialize() const {
  const std::string newline = crlf_ ? "\r\n" : "\n";
  std::string out;
  for (size_t i = 0; i < lines_.size(); ++i) {
    out += lines_[i];
    if (i + 1 < lines_.size() || final_newline_) out += newline;
  }
  return out;
}

size_t TextBuffer::CursorColumn() const {
  const std::string& text = lines_[cursor_line_];
  size_t column = 1;
  for (size_t pos = 0; pos < cursor_byte_; pos += CharLength(text, pos)) ++column;
  return column;
}

size_t TextBuffer::PrevCharStart(size_t pos) const {
  const std::string& text = lines_[cursor_line_];
  // Walk forward from the line start so invalid bytes are handled consistently.
  size_t previous = 0;
  for (size_t p = 0; p < pos; p += CharLength(text, p)) previous = p;
  return previous;
}

void TextBuffer::Insert(const std::string& text) {
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    std::string piece = text.substr(start, end == std::string::npos ? end : end - start);
    lines_[cursor_line_].insert(cursor_byte_, piece);
    cursor_byte_ += piece.size();
    if (!piece.empty()) modified_ = true;
    if (end == std::string::npos) break;
    Newline();
    start = end + 1;
  }
}

void TextBuffer::Newline() {
  std::string rest = lines_[cursor_line_].substr(cursor_byte_);
  lines_[cursor_line_].erase(cursor_byte_);
  lines_.insert(lines_.begin() + cursor_line_ + 1, rest);
  ++cursor_line_;
  cursor_byte_ = 0;
  modified_ = true;
}

void TextBuffer::Backspace() {
  if (cursor_byte_ > 0) {
    size_t start = PrevCharStart(cursor_byte_);
    lines_[cursor_line_].erase(start, cursor_byte_ - start);
    cursor_byte_ = start;
    modified_ = true;
  } else if (cursor_line_ > 0) {
    cursor_byte_ = lines_[cursor_line_ - 1].size();
    lines_[cursor_line_ - 1] += lines_[cursor_line_];
    lines_.erase(lines_.begin() + cursor_line_);
    --cursor_line_;
    modified_ = true;
  }
}

void TextBuffer::MoveLeft() {
  if (cursor_byte_ > 0) {
    cursor_byte_ = PrevCharStart(cursor_byte_);
  } else if (cursor_line_ > 0) {
    --cursor_line_;
    cursor_byte_ = lines_[cursor_line_].size();
  }
}

void TextBuffer::MoveRight() {
  const std::string& text = lines_[cursor_line_];
  if (cursor_byte_ < text.size()) {
    cursor_byte_ += CharLength(text, cursor_byte_);
  } else if (cursor_line_ + 1 < lines_.size()) {
    ++cursor_line_;
    cursor_byte_ = 0;
  }
}

void TextBuffer::Home() { cursor_byte_ = 0; }

void TextBuffer::End() { cursor_byte_ = lines_[cursor_line_].size(); }

std::vector<VisualRow> TextBuffer::Layout(size_t cols) const {
  cols = std::max<size_t>(cols, 1);
  std::vector<VisualRow> rows;
  for (size_t index = 0; index < lines_.size(); ++index) {
    const std::string& text = lines_[index];
    size_t start = 0, pos = 0, count = 0;
    while (pos < text.size()) {
      if (count == cols) {
        rows.push_back({index, start, pos});
        start = pos;
        count = 0;
      }
      pos += CharLength(text, pos);
      ++count;
    }
    rows.push_back({index, start, text.size()});
  }
  return rows;
}

size_t TextBuffer::CursorRow(const std::vector<VisualRow>& rows) const {
  size_t found = 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].line != cursor_line_) continue;
    found = i;
    // The cursor belongs to the first row of its line whose end lies beyond it;
    // at the very end of a line it stays on the line's last row.
    if (cursor_byte_ < rows[i].end) break;
  }
  return found;
}

size_t TextBuffer::CursorRowColumn(const std::vector<VisualRow>& rows) const {
  const VisualRow& row = rows[CursorRow(rows)];
  const std::string& text = lines_[row.line];
  size_t column = 0;
  for (size_t pos = row.start; pos < cursor_byte_; pos += CharLength(text, pos)) ++column;
  return column;
}

void TextBuffer::SetCursor(const std::vector<VisualRow>& rows, size_t row, size_t col) {
  if (rows.empty()) return;
  row = std::min(row, rows.size() - 1);
  const VisualRow& target = rows[row];
  const std::string& text = lines_[target.line];
  size_t pos = target.start;
  for (size_t i = 0; i < col && pos < target.end; ++i) pos += CharLength(text, pos);
  // A wrapped (non-final) row ends where the next row starts; stay on this row.
  const bool last_row_of_line = row + 1 >= rows.size() || rows[row + 1].line != target.line;
  if (!last_row_of_line && pos >= target.end && pos > target.start) {
    pos = target.start;
    for (size_t p = target.start; p < target.end; p += CharLength(text, p)) pos = p;
  }
  cursor_line_ = target.line;
  cursor_byte_ = pos;
}

void TextBuffer::MoveRows(int rows, size_t cols) {
  const auto layout = Layout(cols);
  const size_t current = CursorRow(layout);
  const size_t column = CursorRowColumn(layout);
  long target = static_cast<long>(current) + rows;
  target = std::clamp<long>(target, 0, static_cast<long>(layout.size()) - 1);
  SetCursor(layout, static_cast<size_t>(target), column);
}

std::string TextBuffer::Display(const std::string& line, size_t start, size_t end) {
  std::string out;
  for (size_t pos = start; pos < end; pos += CharLength(line, pos)) {
    const unsigned char c = static_cast<unsigned char>(line[pos]);
    if (c == '\t') out += ' ';
    else if (c >= 0x20 && c < 0x7f) out += static_cast<char>(c);
    else out += '?';
  }
  return out;
}

}  // namespace recovery::editor
