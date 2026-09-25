#!/usr/bin/env python3
"""板子"不理人"时的手动复位探测（USB-Serial/JTAG 的 DTR/RTS 就是复位线）。

用法:
    python reset_probe.py COM43            # 拉 EN 复位一次，然后听 4 秒
    python reset_probe.py COM43 8          # 听 8 秒
    python reset_probe.py COM43 4 --boot0  # 顺便把 GPIO0 按下去（进下载模式）

USB-Serial/JTAG（VID 303A:1001）里：
    RTS = EN(reset)，DTR = GPIO0(boot)
    正常启动：EN 拉低 → 放开 → 芯片从 flash 跑
    下载模式：先按住 GPIO0，再放开 EN（esptool 的 default-reset 就是这个顺序）
"""
import argparse
import sys
import time

import serial

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("secs", nargs="?", type=float, default=4.0)
    ap.add_argument("--boot0", action="store_true", help="按住 GPIO0 = 进下载模式")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.2,
                        write_timeout=0.5, exclusive=True)
    try:
        # 先都放开（别把芯片按在复位里）
        ser.dtr = False
        ser.rts = False
        time.sleep(0.2)
        ser.reset_input_buffer()

        if args.boot0:
            ser.dtr = True                 # GPIO0 低 = boot
            time.sleep(0.1)
        ser.rts = True                     # EN 低 = 复位
        time.sleep(0.15)
        ser.rts = False                    # 放开复位，芯片开始跑
        if args.boot0:
            time.sleep(0.1)
            ser.dtr = False
        print(f"[reset] 已复位 {args.port}，听 {args.secs}s ...", flush=True)

        end = time.time() + args.secs
        total = 0
        while time.time() < end:
            data = ser.read(4096)
            if data:
                total += len(data)
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
        print(f"\n[reset] 共收到 {total} 字节", flush=True)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
