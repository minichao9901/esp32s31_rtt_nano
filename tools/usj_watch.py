#!/usr/bin/env python3
"""不触发复位的串口观察（USB-Serial/JTAG 的 DTR/RTS 就是复位/启动模式线）。

为什么需要：`serial.Serial(port, ...)` 默认一开就把 DTR/RTS 拉高 ——
对 S31 来说那等于"按住 BOOT + 拉低 EN"，芯片**立刻复位（并可能进下载模式）**，
于是"设备掉线 / 又复位"里混进了主机自己的动作，看不到芯片真实行为。
这里的做法是先把 dtr/rts 状态设成 False 再 open（pyserial 支持：未打开时只存状态），
于是打开端口**不会**动那两根线。

用法:
    python usj_watch.py COM43 20            # 不复位，观察 20 秒
    python usj_watch.py COM43 20 --reset    # 先复位一次再观察（等价普通脚本）
"""
import argparse
import sys
import time

import serial

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def open_no_touch(port, baud):
    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = 0.2
    s.write_timeout = 0.4
    s.dtr = False               # ← 关键：open 之前就设好，open 时不碰这两根线
    s.rts = False
    s.open()
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("secs", nargs="?", type=float, default=15.0)
    ap.add_argument("--reset", action="store_true", help="打开后主动复位一次")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    st = time.time()
    end = st + args.secs
    total = 0

    def stamp():
        return f"[{time.time() - st:6.2f}s]"

    try:
        ser = open_no_touch(args.port, args.baud)
    except Exception as e:
        print(f"<open fail: {type(e).__name__} {e}>", flush=True)
        return 1

    print(f"{stamp()} <opened WITHOUT touching DTR/RTS>", flush=True)
    if args.reset:
        ser.rts = True
        time.sleep(0.15)
        ser.rts = False
        print(f"{stamp()} <manual reset pulse sent>", flush=True)

    lost = None
    while time.time() < end:
        try:
            data = ser.read(4096)
        except Exception as e:
            lost = time.time() - st
            print(f"{stamp()} <DEVICE LOST: {type(e).__name__} {e}>", flush=True)
            break
        if not data:
            continue
        total += len(data)
        for line in data.decode("utf-8", "replace").splitlines():
            if line.strip():
                print(f"{stamp()} {line}", flush=True)

    print(f"\n[watch] {total} 字节" + (f"，设备在 {lost:.2f}s 掉线" if lost else "，全程设备在线"),
          flush=True)
    try:
        ser.close()
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
