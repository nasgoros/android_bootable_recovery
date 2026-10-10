// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
// Standalone host test (from the repository root):
//   g++ -std=c++17 -Wall -Wextra -Werror -Irecovery_ui/include recovery_ui/text_editor.cpp
//       recovery_ui/text_viewer.cpp recovery_ui/text_viewer_test.cpp -o /tmp/text-viewer-test
#include "recovery_ui/text_viewer.h"

#include <cassert>
#include <iostream>

using recovery::viewer::Row;
using recovery::viewer::TextView;

static TextView Loaded(const std::string& data) {
  TextView view;
  std::string error;
  assert(view.Load(data, &error));
  return view;
}

int main() {
  std::string error;
  {  // Lines: LF and CRLF, no extra row after a final newline, empty file has one row.
    assert(Loaded("").line_count() == 1 && Loaded("").line(0).empty());
    assert(Loaded("a").line_count() == 1);
    assert(Loaded("a\n").line_count() == 1);
    assert(Loaded("a\n\n").line_count() == 2);
    TextView v = Loaded("one\r\ntwo\nthree");
    assert(v.line_count() == 3 && v.line(0) == "one" && v.line(1) == "two" &&
           v.line(2) == "three" && v.max_width() == 5);
  }
  {  // Display cells: tab as space, controls and UTF-8 characters as one '?'.
    TextView v = Loaded("a\tb\x01" "c\n\xc3\xa9t\xc3\xa9\n");
    assert(v.line(0) == "a b?c");
    assert(v.line(1) == "?t?" && v.max_width() == 5);
  }
  {  // Binary data is refused and the previous text is kept.
    TextView v = Loaded("keep\n");
    assert(!v.Load(std::string("ab\0cd", 5), &error) && !error.empty());
    assert(v.line_count() == 1 && v.line(0) == "keep");
  }
  {  // Line number width.
    std::string many;
    for (int i = 0; i < 120; ++i) many += "x\n";
    assert(Loaded(many).NumberWidth() == 3 && Loaded("a").NumberWidth() == 1);
  }
  {  // Layout with and without wrap.
    TextView v = Loaded("abcdefghij\nxy\n\n");
    auto rows = v.Layout(4, true);
    assert(rows.size() == 5);  // abcd efgh ij | xy | (empty)
    assert(rows[0].line == 0 && rows[0].start == 0 && rows[0].end == 4 && rows[0].first);
    assert(rows[1].start == 4 && rows[1].end == 8 && !rows[1].first);
    assert(rows[2].start == 8 && rows[2].end == 10 && !rows[2].first);
    assert(rows[3].line == 1 && rows[3].first && rows[3].end == 2);
    assert(rows[4].line == 2 && rows[4].first && rows[4].start == 0 && rows[4].end == 0);
    rows = v.Layout(4, false);
    assert(rows.size() == 3 && rows[0].end == 10 && rows[0].first);
    rows = v.Layout(0, true);  // clamped to one column
    assert(rows.size() == 10 + 2 + 1);
  }
  std::cout << "text viewer tests passed" << std::endl;
  return 0;
}
