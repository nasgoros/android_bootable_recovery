// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
//
// nasgorOS full-screen views for ScreenRecoveryUI: on-screen keyboard text entry
// and an interactive terminal. Kept in its own file so rebasing screen_ui.cpp onto
// LineageOS only touches a small hook. All UI text is English (recovery rule).

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <android-base/logging.h>

#include "minui/minui.h"
#include "recovery_ui/keyboard.h"
#include "recovery_ui/screen_ui.h"
#include "recovery_ui/terminal.h"

using recovery::keyboard::KeyType;
using recovery::keyboard::Layout;
using recovery::keyboard::Mode;
using recovery::keyboard::PlacedKey;

namespace {

// Key colours: letters, special keys, and highlighted keys (Done/Enter, active
// Shift or Ctrl) use the recovery accent.
void FillKey(const PlacedKey& key, bool highlighted) {
  if (highlighted) {
    gr_color(0x7c, 0x4d, 0xff, 255);
  } else if (key.key.type == KeyType::kChar || key.key.type == KeyType::kSpace) {
    gr_color(0x3a, 0x3a, 0x3a, 255);
  } else {
    gr_color(0x26, 0x26, 0x26, 255);
  }
  gr_fill(key.x, key.y, key.x + key.w, key.y + key.h);
}

}  // namespace

Point ScreenRecoveryUI::TouchToScreen(const Point& p) const {
  // Same mapping as SelectMenu(const Point&).
  Point point;
  const auto scale_x = static_cast<double>(p.x()) / ScreenWidth();
  const auto scale_y = static_cast<double>(p.y()) / ScreenHeight();
  switch (gr_touch_rotation()) {
    case GRRotation::NONE:
      point.x(ScreenWidth() * scale_x);
      point.y(ScreenHeight() * scale_y);
      break;
    case GRRotation::RIGHT:
      point.x(ScreenWidth() * scale_y);
      point.y(ScreenHeight() - (ScreenHeight() * scale_x));
      break;
    case GRRotation::DOWN:
      point.x(ScreenWidth() - (ScreenWidth() * scale_x));
      point.y(ScreenHeight() - (ScreenHeight() * scale_y));
      break;
    case GRRotation::LEFT:
      point.x(ScreenWidth() - (ScreenWidth() * scale_y));
      point.y(ScreenHeight() * scale_x);
      break;
  }
  point.x(point.x() - gr_overscan_offset_x());
  point.y(point.y() - gr_overscan_offset_y());
  return point;
}

void ScreenRecoveryUI::DrawKeyLabel(int x, int y, int w, int h, const std::string& label) const {
  // Prefer the larger menu font; fall back to the system font for long labels.
  const GRFont* font = gr_menu_font();
  int char_w = menu_char_width_;
  int char_h = menu_char_height_;
  if (static_cast<int>(label.size()) * char_w > w - 8 || char_h > h - 4) {
    font = gr_sys_font();
    char_w = char_width_;
    char_h = char_height_;
  }
  const int text_x = x + (w - static_cast<int>(label.size()) * char_w) / 2;
  const int text_y = y + (h - char_h) / 2;
  gr_color(0xf0, 0xf0, 0xf0, 255);
  gr_text(font, text_x, text_y, label.c_str(), false);
}

// ---------------------------------------------------------------------------
// Text entry

bool ScreenRecoveryUI::EditText(const std::vector<std::string>& headers, std::string* text) {
  if (!HasOnScreenKeyboard()) return false;

  const int gap = std::max(4, ScreenWidth() / 180);
  const int keyboard_top = ScreenHeight() * 55 / 100;
  Mode mode = Mode::kLower;
  std::string value = *text;
  std::vector<PlacedKey> keys =
      Layout(mode, false, 0, keyboard_top, ScreenWidth(), ScreenHeight() - keyboard_top, gap);

  auto draw = [&]() {
    int y = margin_height_;
    SetColor(UIElement::HEADER);
    y += DrawTextLines(margin_width_, y, headers);
    y += char_height_ / 2;
    // Text field.
    const int box_bottom = keyboard_top - gap * 2;
    gr_color(0x1e, 0x1e, 0x1e, 255);
    gr_fill(margin_width_, y, ScreenWidth() - margin_width_, box_bottom);
    SetColor(UIElement::MENU);
    DrawWrappedTextLines(margin_width_ + gap * 2, y + gap * 2, {value + "_"});
    // Keyboard.
    gr_color(0x10, 0x10, 0x10, 255);
    gr_fill(0, keyboard_top, ScreenWidth(), ScreenHeight());
    for (const auto& key : keys) {
      bool highlighted = key.key.type == KeyType::kEnter ||
                         (key.key.type == KeyType::kShift && mode == Mode::kUpper);
      FillKey(key, highlighted);
      DrawKeyLabel(key.x, key.y, key.w, key.h, key.key.label);
    }
  };

  {
    std::lock_guard<std::mutex> lg(updateMutex);
    custom_screen_ = draw;
    update_screen_locked();
  }
  FlushKeys();

  bool confirmed = false;
  bool done = false;
  while (!done) {
    InputEvent evt = WaitInputEvent();
    if (evt.type() == EventType::EXTRA) {
      if (evt.key() == static_cast<int>(KeyError::INTERRUPTED)) break;
      continue;  // timeout: keep waiting
    }
    std::lock_guard<std::mutex> lg(updateMutex);
    if (evt.type() == EventType::KEY) {
      if (evt.key() == KEY_BACK) done = true;
    } else {
      const Point p = TouchToScreen(evt.pos());
      const PlacedKey* hit = recovery::keyboard::HitTest(keys, p.x(), p.y());
      if (!hit) continue;
      const Mode before = mode;
      switch (hit->key.type) {
        case KeyType::kChar:
          if (value.size() < 1024) value += hit->key.text;
          if (mode == Mode::kUpper) mode = Mode::kLower;  // one-shot Shift
          break;
        case KeyType::kSpace:
          if (value.size() < 1024) value += ' ';
          break;
        case KeyType::kBackspace:
          recovery::keyboard::EraseLastCharacter(&value);
          break;
        case KeyType::kShift:
        case KeyType::kSymbols:
        case KeyType::kMoreSymbols:
          mode = recovery::keyboard::NextMode(mode, hit->key.type);
          break;
        case KeyType::kEnter:
          confirmed = true;
          done = true;
          break;
        case KeyType::kCancel:
          done = true;
          break;
        default:
          break;
      }
      if (mode != before) {
        keys = Layout(mode, false, 0, keyboard_top, ScreenWidth(), ScreenHeight() - keyboard_top,
                      gap);
      }
    }
    if (!done) update_screen_locked();
  }

  {
    std::lock_guard<std::mutex> lg(updateMutex);
    custom_screen_ = nullptr;
    update_screen_locked();
  }
  if (confirmed) *text = value;
  return confirmed;
}

// ---------------------------------------------------------------------------
// Terminal

void ScreenRecoveryUI::RunTerminal() {
  using recovery::terminal::PtyShell;
  using recovery::terminal::TerminalEngine;

  const int gap = std::max(4, ScreenWidth() / 180);
  const int keyboard_top = ScreenHeight() * 56 / 100;
  const int text_x = std::max(margin_width_ / 2, 8);
  const int title_y = margin_height_;
  const int text_top = title_y + char_height_ + gap * 2;
  const int cols = std::max((ScreenWidth() - text_x * 2) / char_width_, 20);
  const int rows = std::max((keyboard_top - gap - text_top) / char_height_, 4);

  TerminalEngine engine(cols, rows);
  std::mutex engine_mutex;  // lock order: updateMutex, then engine_mutex
  PtyShell shell;
  std::string error;
  const bool started = shell.Start(cols, rows, &error);
  if (!started) engine.Feed(error + "\r\nPress Exit to return.\r\n");

  Mode mode = Mode::kLower;
  bool ctrl = false;
  size_t scroll_back = 0;
  std::vector<PlacedKey> keys =
      Layout(mode, true, 0, keyboard_top, ScreenWidth(), ScreenHeight() - keyboard_top, gap);

  auto draw = [&]() {
    SetColor(UIElement::HEADER);
    gr_text(gr_sys_font(), text_x, title_y,
            scroll_back ? "Terminal - scrolled back (type to return)" : "Terminal - root shell",
            false);
    std::vector<std::string> lines;
    int cursor_x, cursor_y;
    {
      std::lock_guard<std::mutex> lg(engine_mutex);
      lines = engine.Visible(scroll_back);
      cursor_x = engine.cursor_x();
      cursor_y = engine.cursor_y();
    }
    if (scroll_back == 0) {
      gr_color(0x7c, 0x4d, 0xff, 255);
      const int x = text_x + cursor_x * char_width_;
      const int y = text_top + cursor_y * char_height_;
      gr_fill(x, y, x + char_width_, y + char_height_);
    }
    gr_color(0xe0, 0xe0, 0xe0, 255);
    for (size_t i = 0; i < lines.size(); ++i) {
      gr_text(gr_sys_font(), text_x, text_top + static_cast<int>(i) * char_height_,
              lines[i].c_str(), false);
    }
    gr_color(0x10, 0x10, 0x10, 255);
    gr_fill(0, keyboard_top, ScreenWidth(), ScreenHeight());
    for (const auto& key : keys) {
      bool highlighted = key.key.type == KeyType::kEnter ||
                         (key.key.type == KeyType::kShift && mode == Mode::kUpper) ||
                         (key.key.type == KeyType::kCtrl && ctrl);
      FillKey(key, highlighted);
      DrawKeyLabel(key.x, key.y, key.w, key.h, key.key.label);
    }
  };

  {
    std::lock_guard<std::mutex> lg(updateMutex);
    custom_screen_ = draw;
    update_screen_locked();
  }
  FlushKeys();

  // Reader thread: shell output -> engine -> redraw. A pipe wakes it for shutdown.
  std::atomic<bool> shell_exited{false};
  int wake[2] = {-1, -1};
  std::thread reader;
  if (started && pipe2(wake, O_CLOEXEC) == 0) {
    reader = std::thread([&]() {
      char buf[4096];
      while (true) {
        pollfd fds[2] = {{shell.fd(), POLLIN, 0}, {wake[0], POLLIN, 0}};
        if (poll(fds, 2, -1) < 0) {
          if (errno == EINTR) continue;
          break;
        }
        if (fds[1].revents) break;
        if (fds[0].revents) {
          ssize_t n = read(shell.fd(), buf, sizeof(buf));
          if (n < 0 && errno == EINTR) continue;
          if (n <= 0) {  // the shell exited (EIO on the master side)
            shell_exited = true;
            EnqueueTouch(Point(-1, -1));  // wake the input loop
            break;
          }
          {
            std::lock_guard<std::mutex> lg(engine_mutex);
            engine.Feed(buf, static_cast<size_t>(n));
          }
          Redraw();
        }
      }
    });
  }

  while (!shell_exited) {
    InputEvent evt = WaitInputEvent();
    if (evt.type() == EventType::EXTRA) {
      if (evt.key() == static_cast<int>(KeyError::INTERRUPTED)) break;
      continue;
    }
    if (shell_exited) break;

    std::string input;
    bool exit_requested = false;
    {
      std::lock_guard<std::mutex> lg(updateMutex);
      if (evt.type() == EventType::KEY) {
        const int page = std::max(rows / 2, 1);
        size_t history;
        {
          std::lock_guard<std::mutex> elg(engine_mutex);
          history = engine.scrollback_size();
        }
        if (evt.key() == KEY_VOLUMEUP || evt.key() == KEY_SCROLLUP) {
          scroll_back = std::min(scroll_back + page, history);
        } else if (evt.key() == KEY_VOLUMEDOWN || evt.key() == KEY_SCROLLDOWN) {
          scroll_back = scroll_back > static_cast<size_t>(page) ? scroll_back - page : 0;
        } else if (evt.key() == KEY_BACK) {
          exit_requested = true;
        }
      } else {
        const Point p = TouchToScreen(evt.pos());
        const PlacedKey* hit = recovery::keyboard::HitTest(keys, p.x(), p.y());
        if (hit) {
          const Mode before = mode;
          switch (hit->key.type) {
            case KeyType::kChar: {
              std::string ch = hit->key.text;
              if (ctrl && ch.size() == 1 && ((ch[0] >= 'a' && ch[0] <= 'z') ||
                                             (ch[0] >= 'A' && ch[0] <= 'Z') || ch[0] == '\\' ||
                                             ch[0] == '[' || ch[0] == ']')) {
                ch = std::string(1, static_cast<char>(ch[0] & 0x1f));  // Ctrl+C = 0x03, ...
              }
              ctrl = false;
              input = ch;
              if (mode == Mode::kUpper) mode = Mode::kLower;
              break;
            }
            case KeyType::kSpace: input = " "; break;
            case KeyType::kEnter: input = "\r"; break;
            case KeyType::kBackspace: input = "\x7f"; break;
            case KeyType::kTab: input = "\t"; break;
            case KeyType::kEscape: input = "\x1b"; break;
            case KeyType::kUp: input = "\x1b[A"; break;
            case KeyType::kDown: input = "\x1b[B"; break;
            case KeyType::kRight: input = "\x1b[C"; break;
            case KeyType::kLeft: input = "\x1b[D"; break;
            case KeyType::kCtrl: ctrl = !ctrl; break;
            case KeyType::kExit: exit_requested = true; break;
            case KeyType::kShift:
            case KeyType::kSymbols:
            case KeyType::kMoreSymbols:
              mode = recovery::keyboard::NextMode(mode, hit->key.type);
              break;
            default:
              break;
          }
          if (mode != before) {
            keys = Layout(mode, true, 0, keyboard_top, ScreenWidth(),
                          ScreenHeight() - keyboard_top, gap);
          }
          if (!input.empty()) scroll_back = 0;
        }
      }
      update_screen_locked();
    }
    if (exit_requested) break;
    if (!input.empty() && started) shell.Write(input);
  }

  // Shut down: wake and join the reader before closing the pseudo-terminal.
  if (reader.joinable()) {
    if (wake[1] >= 0) {
      ssize_t written = TEMP_FAILURE_RETRY(write(wake[1], "x", 1));
      (void)written;
    }
    reader.join();
  }
  if (wake[0] >= 0) close(wake[0]);
  if (wake[1] >= 0) close(wake[1]);
  shell.Stop();
  FlushKeys();

  std::lock_guard<std::mutex> lg(updateMutex);
  custom_screen_ = nullptr;
  update_screen_locked();
}
