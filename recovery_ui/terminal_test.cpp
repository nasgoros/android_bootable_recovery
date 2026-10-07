// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
// Standalone host test (from the repository root):
//   g++ -std=c++17 -Wall -Wextra -Werror -Irecovery_ui/include
//       recovery_ui/terminal.cpp recovery_ui/terminal_test.cpp -o /tmp/terminal-test
#include "recovery_ui/terminal.h"

#include <poll.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <iostream>

using recovery::terminal::PtyShell;
using recovery::terminal::TerminalEngine;

static std::string Trim(const std::string& s) {
  size_t end = s.find_last_not_of(' ');
  return end == std::string::npos ? "" : s.substr(0, end + 1);
}

int main() {
  {  // Text, CR LF, wrapping at the last column.
    TerminalEngine t(10, 3);
    t.Feed("hello\r\nworld");
    assert(Trim(t.Line(0)) == "hello" && Trim(t.Line(1)) == "world");
    assert(t.cursor_x() == 5 && t.cursor_y() == 1);
    t.Feed("\r\n0123456789AB");
    // The 11th character wrapped to a new line, which scrolled the screen up.
    assert(Trim(t.Line(0)) == "world");
    assert(t.Line(1) == "0123456789" && Trim(t.Line(2)) == "AB");
    assert(t.scrollback_size() == 1);
  }
  {  // Deferred wrap: exactly cols characters then CR LF does not add a blank line.
    TerminalEngine t(5, 3);
    t.Feed("abcde\r\nx");
    assert(t.Line(0) == "abcde" && Trim(t.Line(1)) == "x");
  }
  {  // Backspace + space + backspace (shell erase), tab.
    TerminalEngine t(20, 2);
    t.Feed("abc\b \b");
    assert(Trim(t.Line(0)) == "ab" && t.cursor_x() == 2);
    t.Feed("\r\na\tb");
    assert(t.Line(1).substr(0, 9) == "a       b");
  }
  {  // Scrolling keeps history; Visible() pages back through it.
    TerminalEngine t(8, 2, 3);
    t.Feed("1\r\n2\r\n3\r\n4\r\n5\r\n6");
    assert(Trim(t.Line(0)) == "5" && Trim(t.Line(1)) == "6");
    assert(t.scrollback_size() == 3);  // 2,3,4 kept, 1 dropped (max 3)
    auto view = t.Visible(2);
    assert(Trim(view[0]) == "3" && Trim(view[1]) == "4");
    view = t.Visible(100);  // clamped to the oldest line
    assert(Trim(view[0]) == "2" && Trim(view[1]) == "3");
  }
  {  // Cursor movement and erase sequences used by shells and toybox.
    TerminalEngine t(10, 3);
    t.Feed("aaaaaaaaaa\r\nbbbbbbbbbb\r\ncccccccccc");
    t.Feed("\x1b[2;3H");  // row 2, column 3
    assert(t.cursor_y() == 1 && t.cursor_x() == 2);
    t.Feed("\x1b[K");  // erase to end of line
    assert(t.Line(1) == "bb        ");
    t.Feed("\x1b[A\x1b[2D");  // up 1, left 2
    assert(t.cursor_y() == 0 && t.cursor_x() == 0);
    t.Feed("\x1b[2J");  // clear screen
    for (int y = 0; y < 3; ++y) assert(Trim(t.Line(y)).empty());
    t.Feed("\x1b[H\x1b[1;31mred\x1b[0m");  // colours are ignored
    assert(Trim(t.Line(0)) == "red");
    t.Feed("\x1b[1G\x1b[2P");  // delete 2 characters at column 1
    assert(Trim(t.Line(0)) == "d");
    t.Feed("\x1b[?25l\x1b[?1049h");  // private modes are ignored
    assert(Trim(t.Line(0)) == "d");
  }
  {  // OSC title, charset selection and UTF-8 do not leak into the screen.
    TerminalEngine t(20, 1);
    t.Feed("\x1b]0;title\x07" "a\x1b(Bb\xc3\xa9" "c\x1b]2;x\x1b\\d");
    assert(Trim(t.Line(0)) == "ab?cd");
  }
  {  // Out-of-range positions are clamped, huge parameters are ignored.
    TerminalEngine t(4, 2);
    t.Feed("\x1b[99;99Hx\x1b[999999999A");
    assert(t.Line(1)[3] == 'x');
  }
  {  // A real shell on a pseudo-terminal: run a command and see its output.
    PtyShell shell;
    std::string error;
    assert(shell.Start(40, 10, &error, "/bin/sh"));
    TerminalEngine t(40, 10);
    assert(shell.Write("/bin/stty size; echo nasgor-$((6*7))\n"));
    std::string seen;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (seen.find("nasgor-42") == std::string::npos &&
           std::chrono::steady_clock::now() < deadline) {
      pollfd pfd{shell.fd(), POLLIN, 0};
      if (poll(&pfd, 1, 200) > 0) {
        char buf[512];
        ssize_t n = read(shell.fd(), buf, sizeof(buf));
        if (n <= 0) break;
        seen.append(buf, n);
        t.Feed(buf, n);
      }
    }
    assert(seen.find("nasgor-42") != std::string::npos);
    assert(seen.find("10 40") != std::string::npos);  // window size reached the shell
    shell.Stop();
    assert(!shell.running());
  }
  std::cout << "PASS: text, wrap, CR/LF, backspace, tab, scrollback, CSI cursor/erase, SGR, OSC, UTF-8, PTY shell\n";
  return 0;
}
