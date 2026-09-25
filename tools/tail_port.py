#!/usr/bin/env python3
"""容错版串口观察：设备掉了就重开，打印时间戳 —— 用来看"板子是不是在反复复位"。

用法:
    python tail_port.py COM43 20          # 观察 20 秒
    python tail_port.py COM43 20 --quiet  # 只打 ROM 复位行/错误行

为什么需要它：普通 read_port 一遇 ClearCommError(PermissionError 13) 就退出，
而"设备消失 → 重新枚举"正是复位循环的特征 —— 一退出就什么都看不见了。
"""
import argparse
import sys
import time

import serial

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

KEYS = ("rst:", "ESP-ROM", "boot:", "Saved PC", "clk]", "msh >")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("secs", nargs="?", type=float, default=15.0)
    ap.add_argument("--quiet", action="store_true", help="只打关键行")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    end = time.time() + args.secs
    st = time.time()
    boots = 0
    total = 0
    ser = None

    def stamp():
        return f"[{time.time() - st:6.2f}s]"

    while time.time() < end:
        if ser is None:
            try:
                ser = serial.Serial(args.port, args.baud, timeout=0.2,
                                    write_timeout=0.4)
                ser.dtr = False
                ser.rts = False
                ser.reset_input_buffer()
                print(f"{stamp()} <port open>", flush=True)
            except Exception as e:
                print(f"{stamp()} <open fail: {type(e).__name__}>", flush=True)
                time.sleep(0.5)
                continue
        try:
            data = ser.read(4096)
        except Exception as e:
            print(f"{stamp()} <read fail: {type(e).__name__} {e}>", flush=True)
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            time.sleep(0.3)
            continue
        if not data:
            continue
        total += len(data)
        text = data.decode("utf-8", "replace")
        for line in text.splitlines():
            if not line.strip():
                continue
            if "rst:" in line or "ESP-ROM" in line:
                boots += 1
            if args.quiet and not any(k in line for k in KEYS):
                continue
            print(f"{stamp()} {line}", flush=True)

    if ser is not None:
        ser.close()
    print(f"\n[tail] 共 {total} 字节，见到 {boots} 个复位标志行", flush=True)


if __name__ == "__main__":
    main()
