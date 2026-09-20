#!/usr/bin/env python3
"""
scripts/find_symbol.py

在源码树/预编译库里查找某个符号的定义位置。

用途：链接期报 "undefined reference to `sws_getContext'" 这类错误时，
需要知道哪个已有库定义了它（或者说：到底有没有）。逐个手写 find/nm/grep
容易在 ssh 里被引号吃掉，所以固定成一个脚本。

用法：
    python3 scripts/find_symbol.py sws_getContext [搜索根目录...]
"""

from __future__ import annotations

import os
import subprocess
import sys

DEFAULT_ROOTS = [
    "/root/openvela/vendor",
    "/root/openvela/apps",
    "/root/openvela/prebuilts",
    "/root/openvela/nuttx",
]

NM_CANDIDATES = [
    "/root/openvela/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin/arm-none-eabi-nm",
    "arm-none-eabi-nm",
    "nm",
]


def find_nm() -> str | None:
    for cand in NM_CANDIDATES:
        if os.path.isabs(cand):
            if os.path.exists(cand):
                return cand
        else:
            from shutil import which

            found = which(cand)
            if found:
                return found
    return None


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    symbol = sys.argv[1]
    roots = sys.argv[2:] or DEFAULT_ROOTS

    nm = find_nm()
    if nm is None:
        print("找不到 nm，无法扫描。")
        return 1
    print(f"符号   : {symbol}")
    print(f"nm     : {nm}")
    print(f"根目录 : {', '.join(roots)}")
    print()

    archives: list[str] = []
    for root in roots:
        for dirpath, _dirnames, filenames in os.walk(root):
            for fn in filenames:
                if fn.endswith(".a"):
                    archives.append(os.path.join(dirpath, fn))

    print(f"共发现 {len(archives)} 个静态库，开始扫描…\n")

    hits: list[tuple[str, str]] = []
    for i, path in enumerate(archives, 1):
        if i % 50 == 0:
            print(f"  …已扫 {i}/{len(archives)}")
        try:
            out = subprocess.run(
                [nm, "--defined-only", path],
                capture_output=True,
                text=True,
                timeout=60,
            ).stdout
        except Exception:  # noqa: BLE001 - 单个库读不了就跳过
            continue

        for line in out.splitlines():
            parts = line.split()
            # nm 输出形如：00000000 T sws_getContext
            if len(parts) >= 3 and parts[-1] == symbol:
                hits.append((path, parts[-2]))
                break

    print()
    if hits:
        print(f"找到 {len(hits)} 处定义：")
        for path, kind in hits:
            print(f"  [{kind}] {path}")
    else:
        print("没有任何库定义该符号。")

    return 0


if __name__ == "__main__":
    sys.exit(main())
