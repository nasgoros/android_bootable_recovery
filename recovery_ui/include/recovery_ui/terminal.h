// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
//
// Minimal terminal for the recovery Terminal menu:
//  - TerminalEngine turns shell output into a character grid with scrollback.
//    It understands what sh/toybox emit in practice: printable text, CR, LF,
//    backspace, tab, and the common ANSI cursor/erase sequences. Colours and
//    other attributes are ignored. Non-ASCII characters are shown as '?',
//    because the recovery font only has ASCII glyphs.
//  - PtyShell runs /system/bin/sh on a pseudo-terminal.
// The engine has no Android dependency and is unit tested on the host.
#pragma once

#include <sys/types.h>

#include <deque>
#include <string>
#include <vector>

namespace recovery::terminal {

class TerminalEngine {
 public:
  TerminalEngine(int cols, int rows, size_t max_scrollback = 1000);

  void Feed(const char* data, size_t size);
  void Feed(const std::string& data) { Feed(data.data(), data.size()); }
  // Clears the screen and scrollback, keeping the size.
  void Reset();

  int cols() const { return cols_; }
  int rows() const { return rows_; }
  int cursor_x() const { return cursor_x_; }
  int cursor_y() const { return cursor_y_; }
  size_t scrollback_size() const { return scrollback_.size(); }

  // `rows()` lines to display. scroll_back = 0 shows the live screen; larger values
  // show older output (clamped to the scrollback size). Lines are padded to cols().
  std::vector<std::string> Visible(size_t scroll_back) const;
  // The live screen line y (0 <= y < rows()).
  const std::string& Line(int y) const { return screen_[y]; }

 private:
  enum class State { kText, kEscape, kEscapeIntermediate, kCsi, kOsc, kOscEscape, kUtf8 };

  void Put(char c);
  void LineFeed();
  void ExecuteCsi(char final_byte);
  int Param(size_t index, int fallback) const;
  void EraseInLine(int mode);
  void EraseInDisplay(int mode);
  void ClampCursor();

  int cols_;
  int rows_;
  size_t max_scrollback_;
  std::vector<std::string> screen_;
  std::deque<std::string> scrollback_;
  int cursor_x_ = 0;
  int cursor_y_ = 0;
  bool wrap_pending_ = false;  // cursor is past the last column
  State state_ = State::kText;
  std::string params_;
  int utf8_remaining_ = 0;
};

class PtyShell {
 public:
  PtyShell() = default;
  ~PtyShell();
  PtyShell(const PtyShell&) = delete;
  PtyShell& operator=(const PtyShell&) = delete;

  // Starts an interactive shell (default /system/bin/sh) on a new pseudo-terminal.
  bool Start(int cols, int rows, std::string* error, const char* shell = "/system/bin/sh");
  // Master side of the pseudo-terminal (readable when the shell prints).
  int fd() const { return fd_; }
  bool running() const { return pid_ > 0; }
  bool Write(const std::string& data);
  // Hangs up the shell and its children and reaps it.
  void Stop();

 private:
  int fd_ = -1;
  pid_t pid_ = -1;
};

}  // namespace recovery::terminal
