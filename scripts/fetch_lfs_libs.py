#!/usr/bin/env python3
"""
scripts/fetch_lfs_libs.py

补齐 openvela 预编译库中被 Git LFS 托管、但 repo sync 时未拉取的那几个文件。

背景
----
vendor/openvela 的 boards/vela/libs/armv7a_cmake/ 下有一批预编译静态库。
其中 libquickapp.a 与 libgui_wrapper.a 由 Git LFS 托管，仓库里存的只是
134 字节的指针文件。链接阶段会报：

    arm-none-eabi-ld: .../libquickapp.a: file format not recognized;
                      treating as linker script
    arm-none-eabi-ld: .../libquickapp.a:1: syntax error

仓库本身没有 .gitattributes 的 lfs 规则，所以 `git lfs ls-files` 看不到它们、
`git lfs pull` 也不会拉。因此这里直接按 LFS Batch API 协议下载：
先 POST 一份对象清单拿到下载地址，再逐个下载并校验 sha256。

用哪个远端
----------
openvela 的 vendor 仓库托管在 Gitee（gitee.com/open-vela/vendor_openvela），
其 LFS 端点在国内可直连；GitHub 在本构建机上不通，所以走 Gitee。

用法
----
    python3 scripts/fetch_lfs_libs.py [--dry-run]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import urllib.error
import urllib.request

# 按顺序尝试的 LFS 端点。
#
# 为什么需要多个：这些库最初从 vendor_openvela 取，但 Gitee 对每个仓库有 LFS
# 流量配额（报错形如 "507 link object failed, insufficient quota will
# used/total: 5211.44MB/5120.00MB"）。实测同一个对象在 libs_openvela_vela
# 仓库同样存在 —— 源码 Make.defs 的注释就写了这批库来自该仓库 —— 于是可以从
# 另一份配额里取。LFS 的 OID 是内容哈希，跨仓库通用，所以换仓库拿到的字节
# 完全等价（脚本仍会校验 sha256）。
ENDPOINTS = [
    "https://gitee.com/open-vela/vendor_openvela.git/info/lfs",
    "https://gitee.com/open-vela/libs_openvela_vela.git/info/lfs",
    "https://github.com/open-vela/libs_openvela_vela.git/info/lfs",
]

DEST_DIR = "/root/openvela/vendor/openvela/boards/vela/libs/armv7a_cmake"

# (文件名, oid, 字节数) —— oid 与大小直接读自仓库里的指针文件
OBJECTS = [
    (
        "libquickapp.a",
        "34d2aad207edeccfc795b1fb7e08f329ab84c4f59eabdbd10677f0333d804768",
        108089034,
    ),
    (
        "libgui_wrapper.a",
        "5fe6215913ae9d447334b67ebb5c9ae3d41bb0c331949aa206ff443fb8909c6b",
        139526012,
    ),
]

LFS_JSON = "application/vnd.git-lfs+json"
TIMEOUT = 60
CHUNK = 1 << 20


def batch_request(endpoint: str, objects: list[dict]) -> dict:
    """向某个 LFS 端点要下载地址。"""
    payload = json.dumps(
        {"operation": "download", "transfers": ["basic"], "objects": objects}
    ).encode("utf-8")

    req = urllib.request.Request(
        f"{endpoint}/objects/batch",
        data=payload,
        headers={"Accept": LFS_JSON, "Content-Type": LFS_JSON},
        method="POST",
    )

    with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
        return json.loads(resp.read().decode("utf-8"))


def resolve_download(name: str, oid: str, size: int) -> str | None:
    """在多个端点间找一个能给出下载地址的，返回 href；都不行则返回 None。"""
    for endpoint in ENDPOINTS:
        host = endpoint.split("/")[2]
        try:
            reply = batch_request(endpoint, [{"oid": oid, "size": size}])
        except Exception as exc:  # noqa: BLE001 - 逐个端点失败都要继续尝试
            print(f"      [{host}] 请求失败：{exc}")
            continue

        for obj in reply.get("objects", []):
            if obj.get("oid") != oid:
                continue
            if obj.get("error"):
                err = obj["error"]
                print(f"      [{host}] {err.get('code')}: {err.get('message')}")
                break
            href = (obj.get("actions") or {}).get("download", {}).get("href")
            if href:
                print(f"      [{host}] 取得下载地址")
                return href

    return None


def is_pointer(path: str) -> bool:
    """判断文件是否仍是 LFS 指针（而不是真实二进制）。"""
    try:
        with open(path, "rb") as fh:
            head = fh.read(64)
    except OSError:
        return False
    return head.startswith(b"version https://git-lfs.github.com/spec")


def download(url: str, dest: str, expect_oid: str, expect_size: int) -> None:
    """流式下载并校验 sha256 与大小，校验通过才原子替换到目标位置。"""
    tmp = dest + ".part"
    digest = hashlib.sha256()
    got = 0

    req = urllib.request.Request(url, headers={"Accept": "*/*"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as resp, open(tmp, "wb") as out:
        while True:
            chunk = resp.read(CHUNK)
            if not chunk:
                break
            out.write(chunk)
            digest.update(chunk)
            got += len(chunk)
            pct = got * 100 // expect_size
            print(f"\r    {pct:3d}%  {got / 1048576:7.1f} / {expect_size / 1048576:.1f} MB",
                  end="", flush=True)

    print()

    if got != expect_size:
        os.unlink(tmp)
        raise RuntimeError(f"大小不符：得到 {got}，期望 {expect_size}")

    actual = digest.hexdigest()
    if actual != expect_oid:
        os.unlink(tmp)
        raise RuntimeError(f"sha256 不符：\n      得到 {actual}\n      期望 {expect_oid}")

    os.replace(tmp, dest)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true", help="只查下载地址，不实际下载")
    args = ap.parse_args()

    print("=" * 66)
    print("  补齐 Git LFS 托管的预编译库")
    print("=" * 66)
    print(f"  目标目录 : {DEST_DIR}")
    print(f"  端点顺序 : 共 {len(ENDPOINTS)} 个，逐个尝试")
    for e in ENDPOINTS:
        print(f"             {e}")
    print()

    todo = []
    for name, oid, size in OBJECTS:
        path = os.path.join(DEST_DIR, name)
        if not os.path.exists(path):
            print(f"  [!] 不存在：{name}")
            todo.append((name, oid, size))
        elif is_pointer(path):
            print(f"  [指针] 需下载：{name}  ({size / 1048576:.1f} MB)")
            todo.append((name, oid, size))
        else:
            print(f"  [已有] 跳过：{name}")

    if not todo:
        print("\n全部已是真实文件，无需下载。")
        return 0

    total = sum(s for _, _, s in todo)
    print(f"\n共需下载 {len(todo)} 个文件，合计 {total / 1048576:.1f} MB\n")

    failures = []
    for name, oid, size in todo:
        print(f"  {name}：解析下载地址…")
        href = resolve_download(name, oid, size)
        if href is None:
            print(f"  [x] {name}：所有端点都无法提供该对象")
            failures.append(name)
            continue

        if args.dry_run:
            print(f"  [dry-run] {name} -> {href[:80]}...")
            continue

        print(f"  下载 {name} …")
        try:
            download(href, os.path.join(DEST_DIR, name), oid, size)
            print("      ✓ 校验通过")
        except Exception as exc:  # noqa: BLE001
            print(f"      [x] {exc}")
            failures.append(name)

    print()
    if failures:
        print(f"完成，但有 {len(failures)} 个失败：{', '.join(failures)}")
        return 1

    print("全部完成。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
