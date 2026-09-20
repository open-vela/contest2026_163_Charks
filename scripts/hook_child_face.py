#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 child_face_init() 挂到 AI Agent 的 LVGL UI 通道初始化里。

为什么要做这一步
----------------
child_face.c 编译没问题，但**没有任何地方调用它**。
NuttX 的 libapps.a 里，未被任何符号引用的目标不会链进最终固件 ——
所以上一版镜像里根本就没有表情界面。

本脚本做的事：
  1. 解析 packages/ai_agent/src/ui/lvgl_ui_channel.c
  2. 找出"看起来是初始化"的函数（名字含 init / create / start）
  3. 在其函数体**末尾**（用大括号配对定位，不是简单插在开头）
     插入 child_face_init()
  4. 在 include 区补上 #include "ui/child_face.h"

幂等：已插入过就跳过。

用法（在云主机上）：
  python3 /root/openvela-build/scripts/hook_child_face.py
"""

import re
import sys

PATH = "/root/openvela/packages/ai_agent/src/ui/lvgl_ui_channel.c"
INCLUDE_LINE = '#include "ui/child_face.h"'
CALL_LINE = "    child_face_init();"

try:
    src = open(PATH, encoding="utf-8").read()
except OSError as e:
    print("[错误] 读不到 %s: %s" % (PATH, e))
    sys.exit(1)

# ---------------------------------------------------------------- 幂等
if "child_face_init" in src:
    print("[跳过] 已经挂过了")
    sys.exit(0)

# ---------------------------------------------------------------- 1. 找初始化函数
# 只看行首的函数定义（排除 static 内部小函数时用名字匹配）
cands = []
for m in re.finditer(r"^(?:static\s+)?[A-Za-z_][\w \*]*?\s+(\w+)\s*\([^;]*\)\s*\{",
                     src, re.M):
    name = m.group(1)
    if re.search(r"init|create|start", name, re.I):
        cands.append((m.start(), name, m.end()))

print("候选初始化函数: %s" % ([c[1] for c in cands] or "无"))
if not cands:
    print("[错误] 没找到初始化函数，需要人工指定")
    sys.exit(1)

# 取名字最像入口的那个：优先含 ui+init 的，否则取第一个
def score(item):
    n = item[1].lower()
    s = 0
    if "ui" in n:
        s += 2
    if "init" in n:
        s += 3
    if "channel" in n:
        s += 1
    return -s

cands.sort(key=score)
start, fname, body_open = cands[0]
print("选定函数: %s" % fname)

# ---------------------------------------------------------------- 2. 用大括号配对找函数结尾
depth = 0
i = body_open - 1          # 指向 '{'
end = None
j = i
while j < len(src):
    ch = src[j]
    if ch == "{":
        depth += 1
    elif ch == "}":
        depth -= 1
        if depth == 0:
            end = j
            break
    j += 1

if end is None:
    print("[错误] 大括号不配对，无法定位函数结尾")
    sys.exit(1)

# ---------------------------------------------------------------- 3. 补 include
if INCLUDE_LINE in src:
    print("[跳过] include 已存在")
else:
    incs = list(re.finditer(r"^#include.*$", src, re.M))
    if incs:
        at = incs[-1].end()
        src = src[:at] + "\n" + INCLUDE_LINE + src[at:]
        print("[OK] 补入 %s" % INCLUDE_LINE)
        # 插入点整体后移
        end += len(INCLUDE_LINE) + 1
    else:
        print("[警告] 没找到 #include 区，需手工补")

# ---------------------------------------------------------------- 4. 插调用（函数体末尾）
indent = "    "
insert_at = end            # 结束大括号的位置（该行行首可能有缩进）
line_start = src.rfind("\n", 0, insert_at) + 1
prefix = src[line_start:insert_at]
m_ind = re.match(r"^(\s*)", prefix)
if m_ind:
    indent = m_ind.group(1)

src = src[:insert_at] + indent + "child_face_init();\n" + src[insert_at:]

open(PATH, "w", encoding="utf-8").write(src)
print("[OK] 已在 %s() 末尾插入 child_face_init()" % fname)

# ---------------------------------------------------------------- 校验
print("")
print("--- 校验 ---")
for n, line in enumerate(open(PATH, encoding="utf-8"), 1):
    if "child_face" in line:
        print("%5d: %s" % (n, line.rstrip()))
