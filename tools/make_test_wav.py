#!/usr/bin/env python3
"""make_test_wav.py —— 生成测试用 WAV（给板子的音频播放器用）。

    python tools\\make_test_wav.py out.wav                  # 默认：16kHz 单声道 8bit 小星星
    python tools\\make_test_wav.py out.wav --rate 22050 --bits 16 --tune sweep

为什么自己做而不是下载：板子那块 flash 只有 8MB、还挂着 FAT，
测试音频要**小**（几万字节）、**可控**（频率/时长/位深都能指定），
而且这样不引入任何外部依赖（只用标准库的 wave + math）。

支持 --bits 8/16（8=无符号、16=小端有符号，都是 WAV 标准格式），
--tune star|scale|sweep|sine（默认 star = 小星星，听得出来对不对）。
"""
import argparse
import math
import struct
import sys
import wave

# 简谱：小星星（C 大调），(半音相对 C4, 拍数)
STAR = [(0, 1), (0, 1), (7, 1), (7, 1), (9, 1), (9, 1), (7, 2),
        (5, 1), (5, 1), (4, 1), (4, 1), (2, 1), (2, 1), (0, 2)]
SCALE = [(i, 0.5) for i in range(0, 13)]


def note_hz(semitone_from_c4):
    return 261.6255653 * (2.0 ** (semitone_from_c4 / 12.0))


def gen(tune, rate, seconds):
    if tune == "sweep":
        n = int(rate * seconds)
        f0, f1 = 200.0, 3000.0
        for i in range(n):
            t = i / rate
            f = f0 * (f1 / f0) ** (t / seconds)
            yield 0.6 * math.sin(2 * math.pi * f * t)
        return
    if tune == "sine":
        n = int(rate * seconds)
        for i in range(n):
            yield 0.6 * math.sin(2 * math.pi * 440.0 * i / rate)
        return

    seq = STAR if tune == "star" else SCALE
    beat = 0.42                       # 每拍秒数
    for semi, beats in seq:
        n = int(rate * beat * beats)
        f = note_hz(semi)
        for i in range(n):
            t = i / rate
            env = min(1.0, t * 60.0) * min(1.0, (n - i) / (rate * 0.03))   # 去咔哒
            yield 0.7 * env * math.sin(2 * math.pi * f * t)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default="test.wav")
    ap.add_argument("--rate", type=int, default=16000)
    ap.add_argument("--bits", type=int, default=8, choices=[8, 16])
    ap.add_argument("--tune", default="star", choices=["star", "scale", "sweep", "sine"])
    ap.add_argument("--seconds", type=float, default=4.0, help="sweep/sine 用")
    a = ap.parse_args()

    with wave.open(a.out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(a.bits // 8)
        w.setframerate(a.rate)
        buf = bytearray()
        for v in gen(a.tune, a.rate, a.seconds):
            v = max(-1.0, min(1.0, v))
            if a.bits == 8:
                buf.append(int(round((v * 0.5 + 0.5) * 255)))          # 8bit = 无符号
            else:
                buf += struct.pack("<h", int(round(v * 32767)))
        w.writeframes(bytes(buf))
    import os
    print("%s : %d bytes, %d Hz, %d bit, mono, tune=%s"
          % (a.out, os.path.getsize(a.out), a.rate, a.bits, a.tune))


if __name__ == "__main__":
    sys.exit(main())
