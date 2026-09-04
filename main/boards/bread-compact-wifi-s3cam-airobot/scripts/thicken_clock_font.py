#!/usr/bin/env python3
"""时钟字体加粗: 对 clock_dseg7.c 内嵌 1bpp 字形做 1px 膨胀(8 邻域 OR)。

重要: 本文件的位图是 LVGL 9.5 的 packed 格式(stride=0, 逐位紧密打包,
跨行不重置 bit 位置, 字形之间按字节对齐), 不是"每行按字节对齐"的常规格式。
本脚本严格按 packed 格式解析/重编码。

- 每个笔画四周各粗 1px, 视觉加粗;
- box_w/box_h 各 +2, ofs_x/ofs_y 各 -1(保持字形相对位置不变);
- adv_w 保持不变(字距/总宽不变; 数字原本步进=ink 宽, 膨胀后相邻字形最多
  重叠 1px, 视觉可接受);
- line_height 等其余字段不变(字形纵向最多扩 2px, 溢出 1~2px 不影响显示);
- 幂等: 处理完成后在文件头写入 MARKER 注释, 再次运行自动跳过。

用法(任意位置):
    python thicken_clock_font.py [font文件路径]
默认处理本目录 ../clock_dseg7.c
"""
import io
import re
import sys

MARKER = "字体加粗: 字形位图已做 1px 膨胀"


def parse_bytes(block: str) -> list[int]:
    block = re.sub(r"/\*.*?\*/", "", block, flags=re.S)  # 先剥离注释(注释里可能有数字)
    out = []
    for m in re.finditer(r"0[xX][0-9a-fA-F]{1,2}|\b\d+\b", block):
        t = m.group(0)
        out.append(int(t, 16) if t[:2].lower() == "0x" else int(t))
    return out


def find_block_end(text: str, open_brace: int) -> int:
    assert text[open_brace] == "{"
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    raise ValueError("unbalanced braces")


def get_array(text: str, name: str):
    # 类型只允许大写属性宏(如 LV_ATTRIBUTE_LARGE_CONST), 避免误匹配其他类型声明
    m = re.search(r"static(?: [A-Z_]+)* const uint8_t %s\[\]\s*=\s*\{" % re.escape(name), text)
    if not m:
        raise ValueError("array %s not found" % name)
    end = find_block_end(text, m.end() - 1)
    return m.start(), m.end(), end, parse_bytes(text[m.end():end])


def get_dsc(text: str, name: str):
    m = re.search(
        r"static const lv_font_fmt_txt_glyph_dsc_t %s\[\] = \{" % re.escape(name), text)
    if not m:
        raise ValueError("dsc %s not found" % name)
    end = find_block_end(text, m.end() - 1)
    entries = [
        tuple(int(v) for v in t)
        for t in re.findall(
            r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), "
            r"\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\s*\}",
            text[m.end():end])]
    return m.start(), end, entries


def get_code_points(text: str, cmap_suffix: str) -> dict:
    """从 cmaps{suffix} 建立 glyph_id -> 码点(仅用于注释)。"""
    m = re.search(
        r"static const lv_font_fmt_txt_cmap_t cmaps%s\[\]\s*=\s*\{" % cmap_suffix, text)
    if not m:
        return {}
    end = find_block_end(text, m.end() - 1)
    cm = re.search(
        r"\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),\s*"
        r"\.unicode_list = (\w+), \.glyph_id_ofs_list = (\w+)", text[m.end():end])
    if not cm:
        return {}
    r0, rlen, g0 = int(cm.group(1)), int(cm.group(2)), int(cm.group(3))
    ofsl = cm.group(5)
    pts = {}
    if ofsl == "NULL":
        for i in range(rlen):
            pts[g0 + i] = r0 + i
    else:
        _, _, _, offs = get_array(text, ofsl)
        for i in range(rlen):
            pts[g0 + offs[i]] = r0 + i
    return pts


def unpack(bits: list[int], w: int, h: int) -> list[list[int]]:
    return [bits[y * w:(y + 1) * w] for y in range(h)]


def dilate(bits: list[list[int]], w: int, h: int) -> list[list[int]]:
    """8 邻域 1px 膨胀: (w+2) x (h+2)。"""
    def on(x, y):
        if 0 <= x < w and 0 <= y < h:
            return bits[y][x]
        return 0

    nw, nh = w + 2, h + 2
    out = [[0] * nw for _ in range(nh)]
    for y in range(nh):
        for x in range(nw):
            out[y][x] = 1 if any(on(x - 1 + dx, y - 1 + dy)
                                  for dy in (-1, 0, 1) for dx in (-1, 0, 1)) else 0
    return out


def pack(rows: list[list[int]]) -> bytes:
    """packed 位流: 逐位连续, MSB 先行, 末尾补零到字节。"""
    flat = [b for row in rows for b in row]
    out = bytearray()
    for i in range(0, len(flat), 8):  # 末尾不足 8 bit 的组补零(条件在下方)
        v = 0
        for j in range(8):
            v = (v << 1) | (flat[i + j] if i + j < len(flat) else 0)
        out.append(v)
    return bytes(out)


def process_font(text: str, suffix: str) -> str:
    bm_name = "glyph_bitmap" + suffix
    dsc_start, dsc_end, entries = get_dsc(text, "glyph_dsc" + suffix)
    bm_start, bm_body, bm_end, data = get_array(text, bm_name)
    code_pts = get_code_points(text, suffix)

    new_glyphs = []  # (comment, bytes)
    new_entries = [(0, 0, 0, 0, 0, 0)]  # id=0 保留位
    bitmap_index = 0
    for i, (bi, adv, bw, bh, ox, oy) in enumerate(entries[1:], start=1):
        need = (bw * bh + 7) // 8
        if bi + need > len(data):
            raise ValueError("glyph %d 数据不完整: %d+%d > %d" % (i, bi, need, len(data)))
        flat = []
        for b in data[bi:bi + need]:
            for k in range(7, -1, -1):
                flat.append((b >> k) & 1)
        flat = flat[:bw * bh]
        rows = dilate(unpack(flat, bw, bh), bw, bh)
        blob = pack(rows)
        cp = code_pts.get(i)
        comment = '/* U+%04X "%s" */' % (cp, chr(cp)) if cp is not None else ""
        new_glyphs.append((comment, blob))
        new_entries.append((bitmap_index, adv, bw + 2, bh + 2, ox - 1, oy - 1))
        bitmap_index += len(blob)

    # 校验原数组被完整切片(最后一个字形到数组末尾)
    last = entries[-1]
    if last[0] + (last[2] * last[3] + 7) // 8 != len(data):
        raise ValueError("原数组末尾有 %d 字节未匹配" % (len(data) - last[0]
                         - (last[2] * last[3] + 7) // 8))

    lines = []
    for i, (comment, blob) in enumerate(new_glyphs):
        if comment:
            lines.append("    " + comment)
        for j in range(0, len(blob), 16):
            lines.append("    " + ", ".join("0x%02x" % b for b in blob[j:j + 16]) + (
                "," if j + 16 < len(blob) or i + 1 < len(new_glyphs) else ""))
    new_bitmap = ("static LV_ATTRIBUTE_LARGE_CONST const uint8_t %s[] = {\n%s\n}"
                  % (bm_name, "\n".join(lines)))

    dsc_lines = ["    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, "
                 ".ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,"]
    for i, e in enumerate(new_entries[1:], start=1):
        tail = "," if i + 1 < len(new_entries) else ""
        dsc_lines.append("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, "
                         ".ofs_x = %d, .ofs_y = %d}%s" % (e[0], e[1], e[2], e[3], e[4], e[5], tail))
    new_dsc = ("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc%s[] = {\n%s\n}"
               % (suffix, "\n".join(dsc_lines)))

    # 先替换 dsc 再替换 bitmap(dsc 在 bitmap 之后, 先换靠后的不影响前者下标)
    text = text[:dsc_start] + new_dsc + text[dsc_end + 1:]
    _, bm_body2, bm_end2, _ = get_array(text, bm_name)
    text = text[:bm_start] + new_bitmap + text[bm_end2 + 1:]
    return text


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "../clock_dseg7.c"
    src = io.open(path, encoding="utf-8").read()
    if MARKER in src:
        print("已加粗过(%s 已存在), 跳过。" % MARKER)
        return
    for suffix in ("", "_56", "_76", "_18"):
        src = process_font(src, suffix)
        print("font clock_dseg7%s: 1px 加粗完成(packed 格式)" % (suffix or "_40"))
    lines = src.split("\n")
    for i, ln in enumerate(lines):
        if ln.startswith("//"):
            lines.insert(i + 1, "// %s(见 scripts/thicken_clock_font.py)。" % MARKER)
            break
    io.open(path, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    print("完成: %s" % path)


if __name__ == "__main__":
    main()
