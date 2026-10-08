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
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <android-base/logging.h>
#include <android-base/properties.h>

#include "minui/minui.h"
#include "recovery_ui/keyboard.h"
#include "recovery_ui/screen_ui.h"
#include "recovery_ui/terminal.h"
#include "recovery_ui/text_editor.h"

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

// Keeps full-screen views below a camera cut-out; ro.recovery.ui.safe_top overrides the default.
int SafeTop(int screen_height, int margin_height) {
  const int inset = android::base::GetIntProperty("ro.recovery.ui.safe_top", screen_height * 4 / 100);
  return std::max(margin_height, inset);
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
      Layout(mode, recovery::keyboard::Variant::kTextEntry, 0, keyboard_top, ScreenWidth(), ScreenHeight() - keyboard_top, gap);

  const int safe_top = SafeTop(ScreenHeight(), margin_height_);
  auto draw = [&]() {
    int y = safe_top;
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
        keys = Layout(mode, recovery::keyboard::Variant::kTextEntry, 0, keyboard_top, ScreenWidth(), ScreenHeight() - keyboard_top,
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
  const int title_y = SafeTop(ScreenHeight(), margin_height_);
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
      Layout(mode, recovery::keyboard::Variant::kTerminal, 0, keyboard_top, ScreenWidth(), ScreenHeight() - keyboard_top, gap);

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
      std::string& line = lines[i];
      line.erase(line.find_last_not_of(' ') + 1);  // blank cells cost glyph blits for nothing
      if (line.empty()) continue;
      gr_text(gr_sys_font(), text_x, text_top + static_cast<int>(i) * char_height_,
              line.c_str(), false);
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
          bool alive = true;
          // Take everything the shell has ready (up to ~12 ms) so a burst of output costs one
          // redraw instead of one per 4 KiB read.
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(12);
          do {
            ssize_t n = read(shell.fd(), buf, sizeof(buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {  // the shell exited (EIO on the master side)
              alive = false;
              break;
            }
            {
              std::lock_guard<std::mutex> lg(engine_mutex);
              engine.Feed(buf, static_cast<size_t>(n));
            }
            pollfd more = {shell.fd(), POLLIN, 0};
            if (poll(&more, 1, 0) <= 0 || !(more.revents & POLLIN)) break;
          } while (std::chrono::steady_clock::now() < deadline);
          Redraw();
          if (!alive) {
            shell_exited = true;
            EnqueueTouch(Point(-1, -1));  // wake the input loop
            break;
          }
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
      const size_t scroll_before = scroll_back;
      const bool ctrl_before = ctrl;
      const Mode mode_before = mode;
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
            keys = Layout(mode, recovery::keyboard::Variant::kTerminal, 0, keyboard_top, ScreenWidth(),
                          ScreenHeight() - keyboard_top, gap);
          }
          if (!input.empty()) scroll_back = 0;
        }
      }
      // Typed characters are redrawn when the shell echoes them; redrawing here as well
      // doubled the work per key press.
      if (scroll_back != scroll_before || ctrl != ctrl_before || mode != mode_before) {
        update_screen_locked();
      }
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

// ---------------------------------------------------------------------------
// Text editor

bool ScreenRecoveryUI::EditDocument(const std::string& title, std::string* content,
                                    std::string* error) {
  using recovery::editor::TextBuffer;
  using recovery::keyboard::Variant;

  error->clear();
  if (!HasOnScreenKeyboard()) {
    *error = "The text editor needs a touch screen.";
    return false;
  }
  TextBuffer buffer;
  if (!buffer.Load(*content, error)) return false;

  const int gap = std::max(4, ScreenWidth() / 180);
  const int keyboard_top = ScreenHeight() * 52 / 100;
  const int text_x = std::max(margin_width_ / 2, 8);
  const int title_y = SafeTop(ScreenHeight(), margin_height_);
  const int text_top = title_y + char_height_ + gap * 2;
  const size_t cols = std::max((ScreenWidth() - text_x * 2) / char_width_, 10);
  const size_t rows = std::max((keyboard_top - gap - text_top) / char_height_, 3);

  Mode mode = Mode::kLower;
  std::vector<PlacedKey> keys =
      Layout(mode, Variant::kEditor, 0, keyboard_top, ScreenWidth(), ScreenHeight() - keyboard_top,
             gap);
  size_t top_row = 0;        // first visible screen row
  bool exit_armed = false;   // Exit tapped once with unsaved changes
  std::string notice;        // replaces the status line until the next action

  auto keep_cursor_visible = [&]() {
    const auto layout = buffer.Layout(cols);
    const size_t row = buffer.CursorRow(layout);
    if (row < top_row) top_row = row;
    if (row >= top_row + rows) top_row = row - rows + 1;
  };

  auto draw = [&]() {
    const auto layout = buffer.Layout(cols);
    // Title and status.
    std::string status = notice;
    if (status.empty()) {
      status = "Ln " + std::to_string(buffer.cursor_line() + 1) + ", Col " +
               std::to_string(buffer.CursorColumn()) + (buffer.modified() ? "  [modified]" : "");
    }
    std::string header = title.size() + status.size() + 3 <= cols
                             ? title + " - " + status
                             : status;
    SetColor(notice.empty() ? UIElement::HEADER : UIElement::BATTERY_LOW);
    gr_text(gr_sys_font(), text_x, title_y, header.substr(0, cols).c_str(), false);
    // Text with the cursor block.
    const size_t cursor_row = buffer.CursorRow(layout);
    if (cursor_row >= top_row && cursor_row < top_row + rows) {
      const int x = text_x + static_cast<int>(buffer.CursorRowColumn(layout)) * char_width_;
      const int y = text_top + static_cast<int>(cursor_row - top_row) * char_height_;
      gr_color(0x7c, 0x4d, 0xff, 255);
      gr_fill(x, y, x + std::max(char_width_ / 4, 2), y + char_height_);
    }
    gr_color(0xe0, 0xe0, 0xe0, 255);
    for (size_t i = 0; i < rows && top_row + i < layout.size(); ++i) {
      const auto& row = layout[top_row + i];
      const std::string text = TextBuffer::Display(buffer.line(row.line), row.start, row.end);
      gr_text(gr_sys_font(), text_x, text_top + static_cast<int>(i) * char_height_, text.c_str(),
              false);
    }
    // Keyboard.
    gr_color(0x10, 0x10, 0x10, 255);
    gr_fill(0, keyboard_top, ScreenWidth(), ScreenHeight());
    for (const auto& key : keys) {
      bool highlighted = key.key.type == KeyType::kSave ||
                         (key.key.type == KeyType::kShift && mode == Mode::kUpper) ||
                         (key.key.type == KeyType::kExit && exit_armed);
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

  bool saved = false;
  bool done = false;
  while (!done) {
    InputEvent evt = WaitInputEvent();
    if (evt.type() == EventType::EXTRA) {
      if (evt.key() == static_cast<int>(KeyError::INTERRUPTED)) break;
      continue;
    }
    std::lock_guard<std::mutex> lg(updateMutex);
    bool exit_tapped = false;
    notice.clear();
    if (evt.type() == EventType::KEY) {
      if (evt.key() == KEY_VOLUMEUP || evt.key() == KEY_SCROLLUP) {
        buffer.MoveRows(-static_cast<int>(rows), cols);
      } else if (evt.key() == KEY_VOLUMEDOWN || evt.key() == KEY_SCROLLDOWN) {
        buffer.MoveRows(static_cast<int>(rows), cols);
      } else if (evt.key() == KEY_BACK) {
        exit_tapped = true;
      }
    } else {
      const Point p = TouchToScreen(evt.pos());
      if (p.y() >= text_top && p.y() < keyboard_top - gap) {
        // Tap in the text: move the cursor there.
        const auto layout = buffer.Layout(cols);
        const size_t row = top_row + static_cast<size_t>((p.y() - text_top) / char_height_);
        const size_t col =
            static_cast<size_t>(std::max(0, (p.x() - text_x + char_width_ / 2) / char_width_));
        if (row < layout.size()) buffer.SetCursor(layout, row, col);
      } else if (const PlacedKey* hit = recovery::keyboard::HitTest(keys, p.x(), p.y())) {
        const Mode before = mode;
        switch (hit->key.type) {
          case KeyType::kChar:
            buffer.Insert(hit->key.text);
            if (mode == Mode::kUpper) mode = Mode::kLower;
            break;
          case KeyType::kSpace: buffer.Insert(" "); break;
          case KeyType::kEnter: buffer.Newline(); break;
          case KeyType::kBackspace: buffer.Backspace(); break;
          case KeyType::kLeft: buffer.MoveLeft(); break;
          case KeyType::kRight: buffer.MoveRight(); break;
          case KeyType::kUp: buffer.MoveRows(-1, cols); break;
          case KeyType::kDown: buffer.MoveRows(1, cols); break;
          case KeyType::kHome: buffer.Home(); break;
          case KeyType::kEnd: buffer.End(); break;
          case KeyType::kPageUp: buffer.MoveRows(-static_cast<int>(rows), cols); break;
          case KeyType::kPageDown: buffer.MoveRows(static_cast<int>(rows), cols); break;
          case KeyType::kSave:
            *content = buffer.Serialize();
            saved = true;
            done = true;
            break;
          case KeyType::kExit: exit_tapped = true; break;
          case KeyType::kShift:
          case KeyType::kSymbols:
          case KeyType::kMoreSymbols:
            mode = recovery::keyboard::NextMode(mode, hit->key.type);
            break;
          default:
            break;
        }
        if (mode != before) {
          keys = Layout(mode, Variant::kEditor, 0, keyboard_top, ScreenWidth(),
                        ScreenHeight() - keyboard_top, gap);
        }
      }
    }
    if (exit_tapped) {
      if (!buffer.modified() || exit_armed) {
        done = true;
      } else {
        exit_armed = true;
        notice = "Unsaved changes: tap Exit again to discard, or Save";
      }
    } else if (!done) {
      exit_armed = false;
    }
    if (!done) {
      keep_cursor_visible();
      update_screen_locked();
    }
  }

  std::lock_guard<std::mutex> lg(updateMutex);
  custom_screen_ = nullptr;
  update_screen_locked();
  return saved;
}

// ---------------------------------------------------------------------------
// Busy animation for slow startup steps

void ScreenRecoveryUI::ShowBusy(const std::string& message) {
  HideBusy();
  {
    std::lock_guard<std::mutex> lg(updateMutex);
    busy_message_ = message;
    busy_frame_ = 0;
    custom_screen_ = [this]() {
      const int width = ScreenWidth();
      const int center_y = ScreenHeight() / 2;
      // Title.
      const std::string title = "NasgorOS Recovery";
      SetColor(UIElement::HEADER);
      gr_text(gr_menu_font(), (width - static_cast<int>(title.size()) * menu_char_width_) / 2,
              center_y - menu_char_height_ * 3, title.c_str(), true);
      // Message with cycling dots; the dots are padded so the text does not jump.
      const std::string dots = std::string(busy_frame_ / 3 % 4, '.') +
                               std::string(3 - busy_frame_ / 3 % 4, ' ');
      const std::string text = busy_message_ + dots;
      gr_color(0xe0, 0xe0, 0xe0, 255);
      gr_text(gr_menu_font(), (width - static_cast<int>(text.size()) * menu_char_width_) / 2,
              center_y - menu_char_height_, text.c_str(), false);
      // Indeterminate bar: a block sweeping across a track.
      const int track_w = width * 6 / 10;
      const int track_x = (width - track_w) / 2;
      const int track_y = center_y + menu_char_height_;
      const int track_h = std::max(6, menu_char_height_ / 4);
      gr_color(0x26, 0x26, 0x26, 255);
      gr_fill(track_x, track_y, track_x + track_w, track_y + track_h);
      const int block_w = track_w / 4;
      const int period = 24;  // frames per sweep
      const int phase = busy_frame_ % (period * 2);
      const int step = phase < period ? phase : period * 2 - phase;  // there and back
      const int block_x = track_x + (track_w - block_w) * step / period;
      gr_color(0x7c, 0x4d, 0xff, 255);
      gr_fill(block_x, track_y, block_x + block_w, track_y + track_h);
    };
    update_screen_locked();
  }
  busy_running_ = true;
  busy_thread_ = std::thread([this]() {
    while (busy_running_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      std::lock_guard<std::mutex> lg(updateMutex);
      if (!busy_running_) break;
      ++busy_frame_;
      update_screen_locked();
    }
  });
}

void ScreenRecoveryUI::HideBusy() {
  if (!busy_thread_.joinable()) return;
  busy_running_ = false;
  busy_thread_.join();
  std::lock_guard<std::mutex> lg(updateMutex);
  custom_screen_ = nullptr;
  update_screen_locked();
}
