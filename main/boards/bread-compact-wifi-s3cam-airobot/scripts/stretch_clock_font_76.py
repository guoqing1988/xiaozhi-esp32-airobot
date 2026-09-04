# -*- coding: utf-8 -*-
"""对 clock_dseg7_76 做 1.2 倍纵向拉伸(最近邻) + 冒号 advance 收窄, 原地改写字段。

- 位图是 LVGL 9.5 的 "packed" 1bpp 格式: stride=0, 逐位连续、无行字节对齐,
  每个字形从字节边界开始, 占 ceil(box_w*box_h/8) 字节, 数组内每个字形前有
  /* U+xxxx "c" */ 注释行。
- dsc 数组按 cmap 顺序: [0]=保留, [1..10]='0'..'9', [11]=':'。
- 只改 _76 字体的: 各字形位图(纵向拉伸)、box_h/ofs_y、clock_dseg7_76 的
  line_height 76→91; 另把冒号 adv_w 240→192(收窄冒号两侧留白)。
- 幂等: 文件含 MARKER 时直接退出。
- 依赖: 字体已加粗(文件头含加粗 MARKER, 见 scripts/thicken_clock_font.py)。
"""
import re
import sys
from pathlib import Path

FONT_C = Path(__file__).resolve().parent.parent / "clock_dseg7.c"
MARKER = "字体加高: 76px 档已做 1.2 倍纵向拉伸(见 scripts/stretch_clock_font_76.py)"
SY = 1.20          # 纵向拉伸倍数
COLON_ADV = 192    # 冒号 advance(1/16 px), 原 240


def decode_packed(data, w, h):
    bits = []
    for i in range(w * h):
        bits.append((data[i >> 3] >> (7 - (i & 7))) & 1)
    return [[bits[y * w + x] for x in range(w)] for y in range(h)]


def repack(rows, w, h):
    bits = [b for row in rows for b in row]
    out = bytearray((h * w + 7) // 8)
    for i, b in enumerate(bits):
        if b:
            out[i >> 3] |= 0x80 >> (i & 7)
    return bytes(out)


def stretch_y(rows, w, h, sy):
    """最近邻纵向拉伸: 目标行 j 采样原行 floor(j/sy)"""
    nh = max(1, round(h * sy))
    return [[rows[min(h - 1, int(j / sy))][i] for i in range(w)] for j in range(nh)]


def hex_lines(b, indent, per=16):
    toks = ["0x%02x" % v for v in b]
    out = []
    for k in range(0, len(toks), per):
        out.append(indent + ", ".join(toks[k:k + per]))
    return ",\n".join(out)


def main():
    text = FONT_C.read_text(encoding="utf-8")
    if MARKER in text:
        print("已处理过, 跳过")
        return
    if "字体加粗: 字形位图已做 1px 膨胀" not in text:
        sys.exit("未找到加粗 MARKER, 请先运行 thicken_clock_font.py")

    # ---------- 位图数组: 按 /* U+xxxx "c" */ 注释切分各字形 ----------
    bm = re.search(
        r"(static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap_76\[\] = \{\n)(\s*)([\s\S]*?)\n\};",
        text)
    if not bm:
        sys.exit("找不到 glyph_bitmap_76 数组")
    indent = bm.group(2)
    body = bm.group(3).lstrip("\n")  # 只去掉开头空行, 保留注释行缩进
    parts = re.split(r"(?m)^(?=\s*/\* U\+)", body)
    glyphs = []  # (code, comment行, bytes)
    for part in parts:
        cm = re.match(r"(\s*/\* U\+([0-9A-F]{4}) \"(.*)\" \*/\n)", part)
        data = bytes(int(t, 16) for t in re.findall(r"0x([0-9a-f]{2})", part))
        if cm:
            glyphs.append((int(cm.group(2), 16), cm.group(1), data))
        elif data:
            sys.exit("位图数组存在未归类数据")
    glyphs.sort(key=lambda g: g[0])
    if [g[0] for g in glyphs] != list(range(0x30, 0x3B)):
        sys.exit("字形集合异常(应为 0x30-0x3A 共 11 个): %r" % [hex(g[0]) for g in glyphs])
    data = b"".join(g[2] for g in glyphs)

    # ---------- dsc 数组: [0] 保留, [1..10] 数字, [11] 冒号 ----------
    da = re.search(
        r"(static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc_76\[\] = \{\n)(\s*)([\s\S]*?)\n\};", text)
    if not da:
        sys.exit("找不到 glyph_dsc_76 数组")
    dscs = re.findall(
        r"\{\s*\.bitmap_index = (\d+),\s*\.adv_w = (\d+),\s*\.box_w = (\d+),\s*\.box_h = (\d+),\s*\.ofs_x = (-?\d+),\s*\.ofs_y = (-?\d+)[^}]*\}",
        da.group(3))
    if len(dscs) != 12:
        sys.exit("dsc 项数异常: %d (应为 12)" % len(dscs))
    dscs = [tuple(map(int, t)) for t in dscs]

    # 校验偏移与位图字节数一致
    old_cur = 0
    for i, (bi, adv, bw, bh, ox, oy) in enumerate(dscs):
        if i == 0:
            continue
        if bi != old_cur:
            sys.exit("dsc 偏移不连续: idx=%d bi=%d cur=%d" % (i, bi, old_cur))
        old_cur += (bw * bh + 7) // 8
    if old_cur != len(data):
        sys.exit("位图长度对不上: dsc 累计 %d != 实际 %d" % (old_cur, len(data)))

    # ---------- 逐字形拉伸, 重建两个数组 ----------
    dsc_rows = ["    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,"]
    bmp_chunks = []
    nbi = 0
    for i, g in enumerate(glyphs):
        code, comment, blob_old = g
        bi, adv, bw, bh, ox, oy = dscs[i + 1]
        bits = stretch_y(decode_packed(blob_old, bw, bh), bw, bh, SY)
        nbh = len(bits)
        blob = repack(bits, bw, nbh)
        nadv = COLON_ADV if code == 0x3A else adv
        dsc_rows.append(
            "    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d}%s"
            % (nbi, nadv, bw, nbh, ox, round(oy * SY), "," if i < len(glyphs) - 1 else ""))
        # 除最后一个字形外, 块末尾必须保留逗号(后面跟下一个字形的注释行)
        tail = "," if i < len(glyphs) - 1 else ""
        bmp_chunks.append(comment + hex_lines(blob, indent) + tail)
        nbi += len(blob)

    # 写盘前自检: 去掉注释后, 除最后一个字节外每个 0xNN 后必须紧跟逗号
    stripped = re.sub(r"/\*.*?\*/", "", "\n".join(bmp_chunks))
    it = list(re.finditer(r"0x[0-9a-f]{2}", stripped))
    bad = [m for m in it[:-1] if stripped[m.end():m.end() + 1] != ","]
    if len(it) != nbi or bad:
        sys.exit("位图数组逗号自检失败: 字节数 %d(应 %d), 缺逗号 %d 处" % (len(it), nbi, len(bad)))

    new_bitmap = bm.group(1) + indent + "\n".join(bmp_chunks) + "\n};"
    text = text.replace(bm.group(0), new_bitmap)

    new_dsc = da.group(1) + "\n".join(dsc_rows) + "\n};"
    text = text.replace(da.group(0), new_dsc)

    # ---------- line_height 76 → round(76*SY) ----------
    new_lh = round(76 * SY)
    pf = re.search(r"(const lv_font_t clock_dseg7_76 = \{[\s\S]*?)\.line_height = \d+([\s\S]*?\};)", text)
    if not pf:
        sys.exit("找不到 clock_dseg7_76 公共字体结构")
    text = text.replace(pf.group(0), pf.group(1) + ".line_height = %d" % new_lh + pf.group(2))

    # ---------- MARKER ----------
    text = text.replace(
        "// 字体加粗: 字形位图已做 1px 膨胀(见 scripts/thicken_clock_font.py)。\n",
        "// 字体加粗: 字形位图已做 1px 膨胀(见 scripts/thicken_clock_font.py)。\n// " + MARKER + "\n")
    FONT_C.write_text(text, encoding="utf-8")
    print("完成: _76 纵向 %.2fx, line_height 76→%d, 冒号 adv 240→%d, 位图 %d→%d 字节"
          % (SY, new_lh, COLON_ADV, len(data), nbi))


if __name__ == "__main__":
    main()
