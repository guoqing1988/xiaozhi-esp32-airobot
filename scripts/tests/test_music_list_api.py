"""LocalMusicPlayer 歌单访问契约（防止"整表按值拷贝"回归）。

本板内部 SRAM 空载仅剩 20~25KB（见板级 README「内存」一节），而原来的
`ListSongs()` 按值返回整张歌单：只为"判空"或"显示前 30 首"就要拷贝 N 首歌名
（N 次堆分配）→ 这是碎片与分配失败的直接来源。

这里把约束钉死：谁再把"按值返回整张列表"的接口加回来，测试就会红。
"""

import os
import re
import unittest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BOARD_DIR = os.path.join(REPO_ROOT, "main", "boards", "bread-compact-wifi-s3cam-airobot")


def _read(name):
    with open(os.path.join(BOARD_DIR, name), encoding="utf-8") as f:
        return f.read()


class TestNoByValueSongList(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = _read("local_music_player.h")
        cls.cc = _read("local_music_player.cc")
        cls.board = _read("compact_wifi_board_s3cam_airobot.cc")

    def test_by_value_list_api_is_gone(self):
        """按值返回整张歌单的接口（ListSongs）不得再出现。"""
        for name, text in (("local_music_player.h", self.h),
                           ("local_music_player.cc", self.cc),
                           ("compact_wifi_board_s3cam_airobot.cc", self.board)):
            self.assertNotIn("ListSongs", text, f"{name} 又出现了 ListSongs（整表拷贝）")

    def test_cheap_api_declared(self):
        """判空与遍历接口必须存在（且是 const，不暴露可写引用）。"""
        self.assertIn("bool HasSongs() const;", self.h)
        self.assertIn("void ForEachSong(", self.h)

    def test_foreach_locks_and_supports_early_exit(self):
        """遍历必须在锁内、且回调返回 false 时能提前结束。"""
        m = re.search(r"void LocalMusicPlayer::ForEachSong\([^{]*\{(.*?)\n\}", self.cc, re.S)
        self.assertIsNotNone(m, "ForEachSong 实现缺失")
        body = m.group(1)
        self.assertIn("lock_guard<std::mutex> lock(songs_mutex_)", body, "遍历未持锁")
        self.assertIn("if (!cb(s))", body)
        self.assertIn("return;", body, "回调返回 false 时未提前结束")

    def test_has_songs_locks(self):
        """判空必须持锁（否则与 ScanSongs 的 clear/push_back 竞争）。"""
        m = re.search(r"bool LocalMusicPlayer::HasSongs\(\) const \{(.*?)\n\}", self.cc, re.S)
        self.assertIsNotNone(m, "HasSongs 实现缺失")
        body = m.group(1)
        self.assertIn("lock_guard<std::mutex> lock(songs_mutex_)", body)
        self.assertIn("!songs_.empty()", body)

    def test_board_iterates_instead_of_copying(self):
        """板级三处调用必须走遍历/判空，而不是先取一份拷贝。"""
        self.assertIn("HasSongs()", self.board)
        self.assertIn("ForEachSong(", self.board)
        self.assertNotIn(".ListSongs()", self.board)


if __name__ == "__main__":
    unittest.main()
