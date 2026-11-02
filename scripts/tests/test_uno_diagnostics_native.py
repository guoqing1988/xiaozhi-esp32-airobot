"""对 UNO 固定容量发送队列和串口行解析器执行真实 C++ 主机测试。"""

import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/bread-compact-wifi-s3cam-airobot"


class UnoDiagnosticsNativeTest(unittest.TestCase):
    def test_queue_and_stream_parser(self):
        source = r'''
#include "uno_tx_queue.h"
#include "uno_line_parser.h"
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
int main() {
    UnoTxQueue q;
    char out[UnoTxQueue::kCommandSize];
    assert(q.Push("drive-forward-100", 1));
    assert(q.Push("drive-back-100", 2));
    assert(q.Pop(out, 3) && strcmp(out, "drive-back-100") == 0);
    assert(q.Push("go-forward-10", 4));
    assert(q.Push("line-start", 4));
    assert(q.Push("servo-90", 4));
    assert(q.Push("drive-forward-100", 4));
    assert(q.Push("drive-stop", 5));
    assert(q.Cancelled() == 2);
    assert(q.Pop(out, 5) && strcmp(out, "drive-stop") == 0);
    assert(q.Pop(out, 5) && strcmp(out, "servo-90") == 0);
    assert(!q.Pop(out, 5));
    assert(q.Push("drive-forward-100", 1));
    assert(!q.Pop(out, 500002));
    for (size_t i = 0; i < UnoTxQueue::kCapacity; ++i) assert(q.Push("servo-10", 1));
    assert(!q.Push("servo-20", 1));
    assert(q.Push("drive-stop", 1));
    assert(q.Pop(out, 1) && strcmp(out, "drive-stop") == 0);
    assert(q.Push("line-stop", 1));
    assert(q.Pop(out, 1) && strcmp(out, "line-stop") == 0);
    UnoLineParser parser;
    std::vector<std::string> lines;
    auto collect = [&](char* s) { lines.emplace_back(s); };
    const std::string input = "@busy go-forward-10\r\n@done go-forward-10\n";
    for (char c : input) parser.Feed(&c, 1, collect);
    assert(lines.size() == 2 && lines[0] == "@busy go-forward-10" && lines[1] == "@done go-forward-10");
    std::string long_line(200, 'a'); long_line += "\n@stat s200 v82\n";
    parser.Feed(long_line.data(), long_line.size(), collect);
    assert(lines.size() == 3 && lines[2] == "@stat s200 v82");
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / "test.cc"
            binary = pathlib.Path(temp) / "test"
            cpp.write_text(source, encoding="utf-8")
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(BOARD), str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
