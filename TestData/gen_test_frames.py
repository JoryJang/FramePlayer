# -*- coding: utf-8 -*-
# 生成 FramePlayer 测试用二进制图像文件
# 内容为移动渐变图案，便于肉眼确认播放是否正常（画面应水平滚动）
#
# 用法：
#   python gen_test_frames.py            # 生成全部三种格式
#   python gen_test_frames.py rgb888     # 只生成某一种
#
# 生成的文件与默认界面参数对应：
#   test_rgb888_320x240_60f.bin   格式选 RGB888，宽 320 高 240
#   test_rgb565_320x240_60f.bin   格式选 RGB565，宽 320 高 240
#   test_gray8_320x240_60f.bin    格式选 灰度8位，宽 320 高 240

import os
import sys

W, H, FRAMES = 320, 240, 60
OUT_DIR = os.path.dirname(os.path.abspath(__file__))


def pixel_rgb(x, y, f):
    """每帧水平滚动的彩色渐变"""
    return ((x + f * 4) % 256, (y) % 256, (x + y + f * 2) % 256)


def gen_rgb888(path):
    with open(path, "wb") as fp:
        for f in range(FRAMES):
            buf = bytearray()
            for y in range(H):
                for x in range(W):
                    buf += bytes(pixel_rgb(x, y, f))
            fp.write(buf)
    print("生成:", path)


def gen_rgb565(path):
    with open(path, "wb") as fp:
        for f in range(FRAMES):
            buf = bytearray()
            for y in range(H):
                for x in range(W):
                    r, g, b = pixel_rgb(x, y, f)
                    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                    buf += bytes((v & 0xFF, v >> 8))   # 低字节在前
            fp.write(buf)
    print("生成:", path)


def gen_gray8(path):
    with open(path, "wb") as fp:
        for f in range(FRAMES):
            buf = bytearray()
            for y in range(H):
                for x in range(W):
                    buf.append((x + y + f * 3) % 256)
            fp.write(buf)
    print("生成:", path)


def main():
    which = sys.argv[1].lower() if len(sys.argv) > 1 else "all"
    if which in ("all", "rgb888"):
        gen_rgb888(os.path.join(OUT_DIR, "test_rgb888_%dx%d_%df.bin" % (W, H, FRAMES)))
    if which in ("all", "rgb565"):
        gen_rgb565(os.path.join(OUT_DIR, "test_rgb565_%dx%d_%df.bin" % (W, H, FRAMES)))
    if which in ("all", "gray8", "gray"):
        gen_gray8(os.path.join(OUT_DIR, "test_gray8_%dx%d_%df.bin" % (W, H, FRAMES)))


if __name__ == "__main__":
    main()
