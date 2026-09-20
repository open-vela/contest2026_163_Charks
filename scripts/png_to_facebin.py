#!/usr/bin/env python3
"""
把任意图片转成板子能直接显示的 face.bin。

为什么要转成"裸 RGB565"而不是直接放 PNG
======================================
板上没有开 PNG 解码（LVGL 的 PNG 支持要额外链解码器，体积和风险都不划算），
而**裸数据是零成本的** —— 驱动直接把它当位图喂给屏，不需要任何解码。

文件格式
========
320 x 240，RGB565，**小端**，共 320*240*2 = 153600 字节，无文件头。

关于缩放的取舍
==============
原图按 **短边铺满 + 居中裁剪**（cover），不是拉伸。
理由是屏幕是固定 4:3，而人脸图往往不是；
直接拉伸会把脸拉变形，裁剪只会切掉边缘，观感好得多。

用法
====
    python png_to_facebin.py 输入图.png face.bin

然后推到板子：
    adb push face.bin /data/etc/superchild/face.bin
"""

import sys
import os


def main():
    if len(sys.argv) != 3:
        print("用法: python png_to_facebin.py <输入图> <输出.bin>", file=sys.stderr)
        return 2

    src, dst = sys.argv[1], sys.argv[2]

    try:
        from PIL import Image
    except ImportError:
        print("需要 Pillow：pip install pillow", file=sys.stderr)
        return 3

    if not os.path.exists(src):
        print(f"找不到输入文件：{src}", file=sys.stderr)
        return 4

    W, H = 320, 240

    im = Image.open(src)

    # 统一处理成不带透明度的 RGB（PNG 可能是 RGBA / P / L）
    if im.mode != "RGB":
        if im.mode in ("RGBA", "LA", "P"):
            # 透明区域合成到暖白底上，避免出现黑边
            bg = Image.new("RGB", im.size, (255, 240, 225))
            im = im.convert("RGBA")
            bg.paste(im, mask=im.split()[-1])
            im = bg
        else:
            im = im.convert("RGB")

    # cover 缩放：短边铺满，再居中裁剪
    sw, sh = im.size
    scale = max(W / sw, H / sh)
    nw, nh = max(1, round(sw * scale)), max(1, round(sh * scale))
    im = im.resize((nw, nh), Image.LANCZOS)

    left = (nw - W) // 2
    top = (nh - H) // 2
    im = im.crop((left, top, left + W, top + H))

    px = im.load()
    out = bytearray(W * H * 2)

    i = 0
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y]
            # RGB565：R 高 5 位、G 中 6 位、B 低 5 位
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            # **小端**：低字节在前。LCD 驱动按 16 位小端读，写反了会颜色错乱。
            out[i] = v & 0xFF
            out[i + 1] = (v >> 8) & 0xFF
            i += 2

    with open(dst, "wb") as f:
        f.write(out)

    print(f"已生成 {dst}：{W}x{H} RGB565 小端，{len(out)} 字节")
    return 0


if __name__ == "__main__":
    sys.exit(main())
