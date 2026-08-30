#!/usr/bin/env python3
"""将当前 build 产物合并为单个 merged-binary.bin，供 Flash 下载工具直接烧录。

用法:
    python scripts/merge_bin.py [--build-dir build] [--output-dir packages]

输出:
    <项目根>/packages/merged-binary.bin (默认)

之后:
    - ESP32 Flash Download Tool: 芯片选 ESP32-S3, 加载 merged-binary.bin,
      起始地址填 0x0, 直接 Download
    - 或命令行: esptool.py write_flash 0x0 packages/merged-binary.bin
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys


def find_esptool(idf_path: str | None) -> list[str] | None:
    """定位 esptool: 依次尝试 PATH、IDF python_env、IDF 组件 wrapper。"""
    # 1) PATH(已 source IDF 环境时可用)
    for name in ("esptool.py", "esptool"):
        p = shutil.which(name)
        if p:
            return [p]

    # 2) IDF python_env: $IDF_TOOLS_PATH 或默认安装位置, 选最新版本 env
    candidates = []
    tools_path = os.environ.get("IDF_TOOLS_PATH")
    if tools_path:
        candidates.append(tools_path)
    if sys.platform == "win32":
        candidates.append("D:/Espressif")
    else:
        candidates.append(os.path.expanduser("~/.espressif"))
    for base in candidates:
        env_root = os.path.join(base, "python_env")
        if not os.path.isdir(env_root):
            continue
        for env in sorted(os.listdir(env_root), reverse=True):
            if sys.platform == "win32":
                py = os.path.join(env_root, env, "Scripts", "python.exe")
            else:
                py = os.path.join(env_root, env, "bin", "python")
            if os.path.exists(py):
                return [py, "-m", "esptool"]

    # 3) IDF 组件 wrapper(最后手段)
    if idf_path:
        p = os.path.join(idf_path, "components", "esptool_py", "esptool", "esptool.py")
        if os.path.exists(p):
            return [sys.executable, p]
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="Merge ESP-IDF build outputs into a single binary")
    parser.add_argument("--build-dir", default="build", help="build directory (default: build)")
    parser.add_argument("--output-dir", default=None,
                        help="output directory for merged-binary.bin (default: <project root>/packages)")
    args = parser.parse_args()

    build_dir = os.path.abspath(args.build_dir)
    if not os.path.isdir(build_dir):
        print(f"ERROR: build directory not found: {build_dir}")
        print("Please run 'idf.py build' first.")
        return 1

    # 项目根 = build 目录的上一级; 输出到 <项目根>/packages/
    project_root = os.path.dirname(build_dir)
    output_dir = os.path.abspath(args.output_dir) if args.output_dir else os.path.join(project_root, "packages")
    os.makedirs(output_dir, exist_ok=True)

    # 1) 读取烧录参数(flash 模式/频率/大小 + 各 bin 偏移), 由 ESP-IDF 生成, 比硬编码可靠
    flash_args_path = os.path.join(build_dir, "flash_args")
    if not os.path.exists(flash_args_path):
        print(f"ERROR: {flash_args_path} not found. Please run 'idf.py build' first.")
        return 1
    with open(flash_args_path, encoding="utf-8") as f:
        lines = [line.strip() for line in f if line.strip()]

    flash_line = lines[0]
    entries = []  # (offset_hex, relative_path)
    for line in lines[1:]:
        parts = line.split()
        if len(parts) >= 2:
            entries.append((parts[0], parts[1]))

    def get_flash_arg(name: str, default: str) -> str:
        m = re.search(rf"--{name}\s+(\S+)", flash_line)
        return m.group(1) if m else default

    flash_mode = get_flash_arg("flash-mode", "dio")
    flash_freq = get_flash_arg("flash-freq", "80m")
    flash_size = get_flash_arg("flash-size", "4MB")

    # 2) 读取 target(芯片型号) 与 IDF 路径
    target, idf_path = "esp32", None
    proj_desc = os.path.join(build_dir, "project_description.json")
    if os.path.exists(proj_desc):
        with open(proj_desc, encoding="utf-8") as f:
            info = json.load(f)
        target = info.get("target") or target
        idf_path = info.get("idf_path")

    # 3) 校验 bin 文件存在(缺失则跳过并警告)
    merged_entries = []
    for offset, rel in entries:
        path = os.path.join(build_dir, rel)
        if not os.path.exists(path):
            print(f"WARNING: missing {rel}, skipped")
            continue
        merged_entries.append((offset, path))

    if not merged_entries:
        print("ERROR: no binary files to merge.")
        return 1

    # 4) 合并
    output = os.path.join(output_dir, "merged-binary.bin")
    esptool_cmd = find_esptool(idf_path)
    if not esptool_cmd:
        print("ERROR: esptool not found. Please run this script inside the ESP-IDF environment.")
        return 1
    cmd = esptool_cmd + [
        "--chip", target,
        "merge-bin",
        "-o", output,
        "--flash-mode", flash_mode,
        "--flash-freq", flash_freq,
        "--flash-size", flash_size,
    ]
    for offset, path in merged_entries:
        cmd += [offset, path]

    print("Merging:", ", ".join(f"{off} {os.path.basename(p)}" for off, p in merged_entries))
    print(f"Flash: {flash_mode} / {flash_freq} / {flash_size}, chip: {target}")
    print(f"esptool: {' '.join(esptool_cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode != 0:
        print(f"ERROR: merge failed (rc={result.returncode})")
        return result.returncode

    size_mb = os.path.getsize(output) / (1024 * 1024)
    print(f"OK: {output} ({size_mb:.1f} MB)")
    print()
    print("Flash Download Tool: chip=ESP32-S3, load this file, start address 0x0, Download")
    print(f"CLI flash: esptool.py --chip {target} write_flash 0x0 {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
