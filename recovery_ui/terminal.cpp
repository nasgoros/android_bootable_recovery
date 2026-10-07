// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0

#include "recovery_ui/terminal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace recovery::terminal {

// ---------------------------------------------------------------------------
// TerminalEngine

TerminalEngine::TerminalEngine(int cols, int rows, size_t max_scrollback)
    : cols_(std::max(cols, 1)), rows_(std::max(rows, 1)), max_scrollback_(max_scrollback) {
  Reset();
}

void TerminalEngine::Reset() {
  screen_.assign(rows_, std::string(cols_, ' '));
  scrollback_.clear();
  cursor_x_ = cursor_y_ = 0;
  wrap_pending_ = false;
  state_ = State::kText;
  params_.clear();
  utf8_remaining_ = 0;
}

void TerminalEngine::ClampCursor() {
  cursor_x_ = std::clamp(cursor_x_, 0, cols_ - 1);
  cursor_y_ = std::clamp(cursor_y_, 0, rows_ - 1);
  wrap_pending_ = false;
}

void TerminalEngine::LineFeed() {
  wrap_pending_ = false;
  if (cursor_y_ + 1 < rows_) {
    ++cursor_y_;
    return;
  }
  scrollback_.push_back(screen_.front());
  while (scrollback_.size() > max_scrollback_) scrollback_.pop_front();
  screen_.erase(screen_.begin());
  screen_.push_back(std::string(cols_, ' '));
}

void TerminalEngine::Put(char c) {
  if (wrap_pending_) {
    cursor_x_ = 0;
    LineFeed();
  }
  screen_[cursor_y_][cursor_x_] = c;
  if (cursor_x_ + 1 < cols_) {
    ++cursor_x_;
  } else {
    wrap_pending_ = true;  // wrap only when the next character arrives
  }
}

int TerminalEngine::Param(size_t index, int fallback) const {
  size_t start = 0;
  for (size_t i = 0; i < index; ++i) {
    start = params_.find(';', start);
    if (start == std::string::npos) return fallback;
    ++start;
  }
  size_t end = params_.find(';', start);
  std::string value = params_.substr(start, end == std::string::npos ? end : end - start);
  if (value.empty() || value.size() > 6 ||
      !std::all_of(value.begin(), value.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
    return fallback;
  }
  return atoi(value.c_str());
}

void TerminalEngine::EraseInLine(int mode) {
  std::string& line = screen_[cursor_y_];
  if (mode == 0) {
    std::fill(line.begin() + cursor_x_, line.end(), ' ');
  } else if (mode == 1) {
    std::fill(line.begin(), line.begin() + cursor_x_ + 1, ' ');
  } else if (mode == 2) {
    std::fill(line.begin(), line.end(), ' ');
  }
}

void TerminalEngine::EraseInDisplay(int mode) {
  if (mode == 0) {
    EraseInLine(0);
    for (int y = cursor_y_ + 1; y < rows_; ++y) screen_[y].assign(cols_, ' ');
  } else if (mode == 1) {
    EraseInLine(1);
    for (int y = 0; y < cursor_y_; ++y) screen_[y].assign(cols_, ' ');
  } else if (mode == 2 || mode == 3) {
    for (auto& line : screen_) line.assign(cols_, ' ');
    if (mode == 3) scrollback_.clear();
  }
}

void TerminalEngine::ExecuteCsi(char final_byte) {
  // Private sequences ("ESC [ ? 25 h" cursor visibility, ...) are ignored.
  if (!params_.empty() && (params_[0] == '?' || params_[0] == '>' || params_[0] == '=')) return;
  const int n = std::max(Param(0, 1), 1);
  switch (final_byte) {
    case 'A': cursor_y_ -= n; ClampCursor(); break;           // cursor up
    case 'B': case 'e': cursor_y_ += n; ClampCursor(); break; // cursor down
    case 'C': case 'a': cursor_x_ += n; ClampCursor(); break; // cursor right
    case 'D': cursor_x_ -= n; ClampCursor(); break;           // cursor left
    case 'E': cursor_y_ += n; cursor_x_ = 0; ClampCursor(); break;
    case 'F': cursor_y_ -= n; cursor_x_ = 0; ClampCursor(); break;
    case 'G': case '`': cursor_x_ = n - 1; ClampCursor(); break;  // column
    case 'd': cursor_y_ = n - 1; ClampCursor(); break;             // row
    case 'H': case 'f':                                            // position
      cursor_y_ = std::max(Param(0, 1), 1) - 1;
      cursor_x_ = std::max(Param(1, 1), 1) - 1;
      ClampCursor();
      break;
    case 'J': EraseInDisplay(Param(0, 0)); break;
    case 'K': EraseInLine(Param(0, 0)); break;
    case 'P': {  // delete characters, shifting the rest of the line left
      std::string& line = screen_[cursor_y_];
      int count = std::min(n, cols_ - cursor_x_);
      line.erase(cursor_x_, count);
      line.append(count, ' ');
      break;
    }
    case '@': {  // insert blank characters
      std::string& line = screen_[cursor_y_];
      int count = std::min(n, cols_ - cursor_x_);
      line.insert(cursor_x_, count, ' ');
      line.resize(cols_);
      break;
    }
    case 'X': {  // erase characters
      std::string& line = screen_[cursor_y_];
      std::fill_n(line.begin() + cursor_x_, std::min(n, cols_ - cursor_x_), ' ');
      break;
    }
    default:  // 'm' (colours) and anything else: ignored
      break;
  }
}

void TerminalEngine::Feed(const char* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    const unsigned char c = static_cast<unsigned char>(data[i]);
    switch (state_) {
      case State::kUtf8:
        if ((c & 0xc0) == 0x80) {
          if (--utf8_remaining_ == 0) state_ = State::kText;
          continue;
        }
        state_ = State::kText;  // malformed sequence: process this byte normally
        [[fallthrough]];
      case State::kText:
        if (c == 0x1b) {
          state_ = State::kEscape;
        } else if (c == '\r') {
          cursor_x_ = 0;
          wrap_pending_ = false;
        } else if (c == '\n' || c == 0x0b || c == 0x0c) {
          LineFeed();
        } else if (c == '\b') {
          if (wrap_pending_) wrap_pending_ = false;
          else if (cursor_x_ > 0) --cursor_x_;
        } else if (c == '\t') {
          int next = std::min((cursor_x_ / 8 + 1) * 8, cols_ - 1);
          while (cursor_x_ < next) Put(' ');
        } else if (c >= 0x20 && c < 0x7f) {
          Put(static_cast<char>(c));
        } else if (c >= 0xc0 && c < 0xf8) {
          // Start of a multi-byte UTF-8 character: shown as one '?'.
          utf8_remaining_ = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
          state_ = State::kUtf8;
          Put('?');
        } else if (c >= 0x80) {
          Put('?');
        }
        // Other control characters (BEL, NUL, ...) are ignored.
        break;
      case State::kEscape:
        if (c == '[') {
          state_ = State::kCsi;
          params_.clear();
        } else if (c == ']') {
          state_ = State::kOsc;
        } else if (c == '(' || c == ')' || c == '*' || c == '+' || c == '#' || c == '%') {
          state_ = State::kEscapeIntermediate;  // charset selection: one more byte
        } else {
          if (c == 'c') Reset();
          else if (c == 'D') LineFeed();
          else if (c == 'E') { cursor_x_ = 0; LineFeed(); }
          else if (c == 'M' && cursor_y_ > 0) --cursor_y_;
          state_ = State::kText;
        }
        break;
      case State::kEscapeIntermediate:
        state_ = State::kText;
        break;
      case State::kCsi:
        if (c >= 0x40 && c <= 0x7e) {
          ExecuteCsi(static_cast<char>(c));
          state_ = State::kText;
        } else if (c == 0x1b) {
          state_ = State::kEscape;  // aborted sequence
        } else if (params_.size() < 32) {
          params_ += static_cast<char>(c);
        }
        break;
      case State::kOsc:  // window title etc.: ends with BEL or ESC backslash
        if (c == 0x07) state_ = State::kText;
        else if (c == 0x1b) state_ = State::kOscEscape;
        break;
      case State::kOscEscape:
        state_ = c == '\\' ? State::kText : State::kOsc;
        break;
    }
  }
}

std::vector<std::string> TerminalEngine::Visible(size_t scroll_back) const {
  scroll_back = std::min(scroll_back, scrollback_.size());
  std::vector<std::string> lines;
  lines.reserve(rows_);
  // The view is the last rows_ lines of (scrollback + screen), moved up by scroll_back.
  const size_t total = scrollback_.size() + screen_.size();
  const size_t first = total - rows_ - scroll_back;
  for (size_t i = first; i < first + rows_; ++i) {
    lines.push_back(i < scrollback_.size() ? scrollback_[i] : screen_[i - scrollback_.size()]);
  }
  return lines;
}

// ---------------------------------------------------------------------------
// PtyShell

PtyShell::~PtyShell() { Stop(); }

bool PtyShell::Start(int cols, int rows, std::string* error, const char* shell) {
  int fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (fd < 0 || grantpt(fd) != 0 || unlockpt(fd) != 0) {
    *error = std::string("Cannot open a pseudo-terminal: ") + strerror(errno);
    if (fd >= 0) close(fd);
    return false;
  }
  char name[64];
  if (ptsname_r(fd, name, sizeof(name)) != 0) {
    *error = std::string("Cannot open a pseudo-terminal: ") + strerror(errno);
    close(fd);
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    *error = std::string("Cannot start the shell: ") + strerror(errno);
    close(fd);
    return false;
  }
  if (pid == 0) {
    // Child: new session with the pseudo-terminal as controlling terminal.
    setsid();
    int slave = open(name, O_RDWR);
    if (slave < 0) _exit(126);
    ioctl(slave, TIOCSCTTY, 0);
    struct winsize size {};
    size.ws_col = static_cast<unsigned short>(cols);
    size.ws_row = static_cast<unsigned short>(rows);
    ioctl(slave, TIOCSWINSZ, &size);
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    dup2(slave, STDERR_FILENO);
    if (slave > STDERR_FILENO) close(slave);
    // Signals ignored by recovery must not stay ignored in the shell.
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    setenv("TERM", "vt100", 1);
    setenv("HOME", "/", 1);
    setenv("PATH", "/system/bin:/system/xbin:/sbin", 1);
    if (chdir("/") != 0) _exit(126);
    execl(shell, "sh", "-i", nullptr);
    _exit(127);
  }
  fd_ = fd;
  pid_ = pid;
  return true;
}

bool PtyShell::Write(const std::string& data) {
  size_t done = 0;
  while (fd_ >= 0 && done < data.size()) {
    ssize_t n = write(fd_, data.data() + done, data.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    done += n;
  }
  return done == data.size();
}

void PtyShell::Stop() {
  if (pid_ > 0) {
    // The shell is a session leader: signal its whole process group.
    kill(-pid_, SIGHUP);
    kill(pid_, SIGHUP);
    for (int i = 0; i < 20; ++i) {
      if (waitpid(pid_, nullptr, WNOHANG) == pid_) {
        pid_ = -1;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (pid_ > 0) {
      kill(-pid_, SIGKILL);
      kill(pid_, SIGKILL);
      waitpid(pid_, nullptr, 0);
      pid_ = -1;
    }
  }
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

}  // namespace recovery::terminal
