#!/usr/bin/env python3
"""
scripts/fix_vendor_make_defs.py

修正厂商 Make.defs 里 QuickApp 预编译库的 whole-archive 写法，使其**同时适配
两种板级配置**。

────────────────────────────────────────────────────────────────────────
问题
────────────────────────────────────────────────────────────────────────

厂商原文（`vendor/allwinnertech/boards/r528/r528s3-gemini-s1/scripts/Make.defs`）：

    ifeq ($(CONFIG_QUICKAPP),y)
    EXTRA_LIBS += -Wl,--whole-archive
    EXTRA_LIBS += .../libapps_vapp.a
    ...
    EXTRA_LIBS += -Wl,--no-whole-archive
    endif

这段在 `configs/nsh` 下**链接失败**：

    arm-none-eabi-ld: unrecognized option '-Wl,--whole-archive'

但在 `configs/nsh_minidisplay` 下**原本是对的**。

────────────────────────────────────────────────────────────────────────
为什么会这样
────────────────────────────────────────────────────────────────────────

`-Wl,` 前缀是给**编译器驱动器**（gcc）用的；裸链接器（ld）不认这个前缀。
而这个工程里链接器**会随配置变化** —— `nuttx/arch/arm/src/common/Toolchain.defs`：

    else                      # GNU EABI 分支
        CC      = $(CROSSDEV)gcc
        LD      = $(CROSSDEV)ld            ← 默认是裸 ld
        ...
        ifeq ($(CONFIG_LTO_FULL),y)
          ifeq ($(CONFIG_ARM_TOOLCHAIN_GNU_EABI),y)
            LD := $(CROSSDEV)gcc            ← 开了 LTO 就换成 gcc
            ...
          endif
        endif

LTO 必须由编译器驱动链接（才能跑 LTO 插件），所以：

    configs/nsh             CONFIG_LTO_FULL 未开 → LD = arm-none-eabi-ld   → 要裸写法
    configs/nsh_minidisplay CONFIG_LTO_FULL=y    → LD = arm-none-eabi-gcc  → 要 -Wl, 写法

厂商那一段漏了这个判断，于是**只能对上其中一种配置**。

────────────────────────────────────────────────────────────────────────
修法：抄 NuttX 自己的做法
────────────────────────────────────────────────────────────────────────

NuttX 在 `nuttx/arch/arm/src/Makefile` 里就是这么判断的：

    # Override in Make.defs if linker is not 'ld'
    ifeq ($(LD),$(CC))
        LDSTARTGROUP ?= -Wl,--start-group
        LDENDGROUP   ?= -Wl,--end-group
    else
        LDSTARTGROUP ?= --start-group
        LDENDGROUP   ?= --end-group
    endif

于是这里引入同样的判断，并且**用递归变量（=）而不是立即展开**：
`EXTRA_LIBS` 本身就是递归变量（`=`，已用 `make -p` 确认），所以 `$(QAPP_WL)`
会一直保留到真正执行链接配方时才求值 —— 那一刻 `LD` 早已确定。

    QAPP_WL = $(if $(filter $(LD),$(CC)),-Wl,)

    EXTRA_LIBS += $(QAPP_WL)--whole-archive
    ...
    EXTRA_LIBS += $(QAPP_WL)--no-whole-archive

效果：

    LD = arm-none-eabi-gcc 时 → $(QAPP_WL) 展开为 -Wl,  → -Wl,--whole-archive
    LD = arm-none-eabi-ld  时 → $(QAPP_WL) 展开为空    → --whole-archive

两种配置都对，且以后再加配置也不必回来改。

────────────────────────────────────────────────────────────────────────
用法
────────────────────────────────────────────────────────────────────────

    python3 scripts/fix_vendor_make_defs.py            # 应用
    python3 scripts/fix_vendor_make_defs.py --check    # 只检查，不改

脚本是**幂等**的：已经是自适应写法时不会重复插入。
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# 模板与"已生成的副本"都要改：
#   - 模板：重新 configure 时生成的 nuttx/Make.defs 来源于它，必须改，否则被覆盖
#   - 副本：本次构建立即生效，不必等重新 configure
TARGETS = [
    "/root/openvela/vendor/allwinnertech/boards/r528/r528s3-gemini-s1/scripts/Make.defs",
    "/root/openvela/nuttx/Make.defs",
]

MARKER = "QAPP_WL"

DEFINITION = """# whole-archive 的写法必须跟随实际的链接器 —— NuttX 自己就是这么做的
# （见 nuttx/arch/arm/src/Makefile 的 "Override in Make.defs if linker is not 'ld'"）：
#
#   LD == CC（编译器驱动器）→ 需要 -Wl, 前缀
#   LD 是裸链接器           → 不能带前缀，否则 ld 报 unrecognized option
#
# 而 LD 会随配置变化：arch/arm/src/common/Toolchain.defs 在 CONFIG_LTO_FULL=y
# 时把 LD 从 $(CROSSDEV)ld 换成 $(CROSSDEV)gcc（LTO 必须由编译器驱动链接）。
# 于是同一份 Make.defs 在两种配置下需要不同写法：
#   configs/nsh             （无 LTO）→ LD = ld  → 裸写法
#   configs/nsh_minidisplay （有 LTO）→ LD = gcc → -Wl, 写法
#
# 用递归变量（=）而非立即展开：EXTRA_LIBS 本身是递归变量，
# 因此 $(QAPP_WL) 会留到真正链接时才求值，那时 LD 已经确定。
#
# 注意那个单独的 QAPP_WL_IF_GCC：$(if 条件,真值,假值) 是**用逗号分隔参数**的，
# 所以字面量逗号不能直接写在里面 —— 会被当成参数分隔符吃掉，
# 结果 -Wl, 变成 -Wl，链接报 "-Wl--whole-archive"（就是少了个逗号）。
# 拆成一个独立变量即可绕开。
QAPP_WL_IF_GCC = -Wl,
QAPP_WL = $(if $(filter $(LD),$(CC)),$(QAPP_WL_IF_GCC),)
"""

# 四种可能的历史写法都要能收敛到自适应写法：
#   --whole-archive      （我第一版为了 nsh 改成的样子）
#   -Wl,--whole-archive  （厂商原文，适配 LTO）
WA_PRE_RE = re.compile(r"^(\s*)EXTRA_LIBS\s*\+=\s*(?:-Wl,)?--whole-archive\s*$", re.M)
WA_POST_RE = re.compile(r"^(\s*)EXTRA_LIBS\s*\+=\s*(?:-Wl,)?--no-whole-archive\s*$", re.M)

# 注意：判断"定义是否已存在"必须匹配 `QAPP_WL =` 这种**定义**形式，
# 不能用 `"QAPP_WL" in text` —— 替换之后文本里出现的 $(QAPP_WL) 是**引用**，
# 也会命中，结果就是：引用了却从未定义（我第一次就踩了这个坑：
# 脚本报"已修改"，实际只加了引用没加定义，构建仍报 unrecognized option）。
DEF_RE = re.compile(r"^\s*QAPP_WL\s*[:?+]?=", re.M)


def patch_file(path: str, check_only: bool) -> int:
    if not os.path.exists(path):
        print(f"  [!] 不存在，跳过：{path}")
        return 1

    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()

    already = DEF_RE.search(text) is not None

    new = WA_PRE_RE.sub(r"\1EXTRA_LIBS += $(QAPP_WL)--whole-archive", text)
    new = WA_POST_RE.sub(r"\1EXTRA_LIBS += $(QAPP_WL)--no-whole-archive", new)

    # 定义插在 QUICKAPP_PREBUILT_DIR 之后（也就是 whole-archive 那组之前）
    if DEF_RE.search(new) is None:
        anchor = re.search(
            r"^(\s*)QUICKAPP_PREBUILT_DIR\s*=.*$", new, re.M
        )
        if anchor is None:
            print(f"  [!] 找不到 QUICKAPP_PREBUILT_DIR 锚点：{path}")
            return 1
        new = new[: anchor.end() + 1] + "\n" + DEFINITION + new[anchor.end() + 1 :]

    if new == text:
        state = "已是自适应写法" if already else "内容无变化"
        print(f"  [OK] {state}：{path}")
        return 0

    if check_only:
        print(f"  [需修改] {path}")
        return 2

    # 备份一次即可，不要反复覆盖成"上一次的临时态"
    backup = path + ".orig"
    if not os.path.exists(backup):
        with open(backup, "w", encoding="utf-8") as fh:
            fh.write(text)
        print(f"  [备份] {backup}")

    with open(path, "w", encoding="utf-8") as fh:
        fh.write(new)

    print(f"  [已修改] {path}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只检查，不修改")
    ap.add_argument("paths", nargs="*", help="可选：覆盖默认目标路径")
    args = ap.parse_args()

    targets = args.paths or TARGETS

    print("=" * 66)
    print("  修正 QuickApp whole-archive 写法（自适应 LD）")
    print("=" * 66)

    rc = 0
    for path in targets:
        rc |= patch_file(path, args.check)

    print()
    if rc & 2:
        print("有文件需要修改（未改动，因为指定了 --check）。")
    elif rc:
        print("有文件处理失败，请检查上面的提示。")
    else:
        print("完成。两种板级配置（nsh / nsh_minidisplay）现在都能正确链接。")
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
