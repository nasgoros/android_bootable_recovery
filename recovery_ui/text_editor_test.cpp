// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
// Standalone host test (from the repository root):
//   g++ -std=c++17 -Wall -Wextra -Werror -Irecovery_ui/include
//       recovery_ui/text_editor.cpp recovery_ui/text_editor_test.cpp -o /tmp/text-editor-test
#include "recovery_ui/text_editor.h"

#include <cassert>
#include <iostream>

using recovery::editor::TextBuffer;

static TextBuffer Loaded(const std::string& data) {
  TextBuffer buffer;
  std::string error;
  assert(buffer.Load(data, &error));
  return buffer;
}

int main() {
  std::string error;
  {  // Round trips keep line endings and the presence of a final newline.
    for (const char* data : {"", "a", "a\n", "a\nb", "a\nb\n", "a\r\nb\r\n", "a\r\nb", "\n\n",
                             "x\r\ny\nz\n", "mid\rline\n", "\xc3\xa9t\xc3\xa9\n"}) {
      assert(Loaded(data).Serialize() == data);
    }
    assert(Loaded("a\r\nb\r\n").crlf() && !Loaded("x\r\ny\nz\n").crlf());
    TextBuffer binary;
    assert(!binary.Load(std::string("ab\0cd", 5), &error) && !error.empty());
  }
  {  // Typing, Enter, Backspace across lines.
    TextBuffer t = Loaded("hello\nworld\n");
    t.End();
    t.Insert("!");
    assert(t.Serialize() == "hello!\nworld\n" && t.modified());
    t.Newline();
    t.Insert("new");
    assert(t.Serialize() == "hello!\nnew\nworld\n");
    t.Home();
    t.Backspace();  // joins "new" onto "hello!"
    assert(t.Serialize() == "hello!new\nworld\n" && t.cursor_byte() == 6);
    t.Insert("a\nb");  // multi-line insert
    assert(t.Serialize() == "hello!a\nbnew\nworld\n");
  }
  {  // CRLF files stay CRLF after edits.
    TextBuffer t = Loaded("one\r\ntwo\r\n");
    t.MoveRight();
    t.Newline();
    assert(t.Serialize() == "o\r\nne\r\ntwo\r\n");
  }
  {  // UTF-8: one cursor step per character, backspace removes whole characters.
    TextBuffer t = Loaded("a\xc3\xa9" "b");  // "aéb"
    t.End();
    t.MoveLeft();
    assert(t.cursor_byte() == 3 && t.CursorColumn() == 3);
    t.Backspace();
    assert(t.Serialize() == "ab");
    assert(TextBuffer::Display("a\xc3\xa9\tb\x01", 0, 6) == "a? b?");
  }
  {  // Soft wrap and moving through wrapped rows.
    TextBuffer t = Loaded("abcdefgh\nxy\n");
    auto rows = t.Layout(3);
    assert(rows.size() == 4);  // abc def gh | xy
    assert(rows[0].start == 0 && rows[0].end == 3 && rows[2].end == 8 && rows[3].line == 1);
    t.SetCursor(rows, 1, 1);  // "e"
    assert(t.cursor_line() == 0 && t.cursor_byte() == 4);
    assert(t.CursorRow(rows) == 1 && t.CursorRowColumn(rows) == 1);
    t.MoveRows(1, 3);  // down to "h"
    assert(t.cursor_byte() == 7);
    t.MoveRows(1, 3);  // down to "y" on the next line
    assert(t.cursor_line() == 1 && t.cursor_byte() == 1);
    t.MoveRows(-10, 3);  // clamped to the first row
    assert(t.cursor_line() == 0 && t.cursor_byte() == 1);
    // Tapping past the end of a wrapped row stays on that row.
    t.SetCursor(rows, 0, 99);
    assert(t.cursor_byte() == 2 && t.CursorRow(rows) == 0);
    // Tapping past the end of a line's last row goes to the end of the line.
    t.SetCursor(rows, 2, 99);
    assert(t.cursor_byte() == 8 && t.CursorRow(rows) == 2);
    // A line of exactly `cols` characters keeps the cursor at its end on its row.
    TextBuffer exact = Loaded("abc");
    auto exact_rows = exact.Layout(3);
    exact.End();
    assert(exact_rows.size() == 1 && exact.CursorRow(exact_rows) == 0);
  }
  {  // Loading resets state; cursor movement never leaves the buffer.
    TextBuffer t = Loaded("x");
    t.MoveLeft();
    t.Backspace();
    t.MoveRight();
    t.MoveRight();
    assert(t.Serialize() == "x" && t.cursor_byte() == 1 && !t.modified());
  }
  std::cout << "PASS: load/save (LF, CRLF, mixed, empty, binary), edit, UTF-8, wrap, cursor, taps\n";
  return 0;
}
