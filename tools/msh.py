#!/usr/bin/env python3
"""msh 自动验证：开串口 → 等固件启动 → 敲命令 → 收响应 → 存证据。

用法:
    python msh.py COM43                          # 只听，看启动日志
    python msh.py COM43 "help"                   # 敲一条
    python msh.py COM43 "help|s31_info|ps" 3     # 用 | 分隔多条；最后一条后听 3 秒
    python msh.py COM43 "list_thread" 2 -o captures/list_thread.txt

⚠️ 打开这个串口会复位芯片：USB-Serial/JTAG 的 DTR/RTS 就是芯片的复位/启动模式线
   （ROM 日志会打 rst:0x17 CHIP_USB_UART_RESET）。所以开完端口必须**先等固件
   起来**再敲命令，否则命令会掉进重启的空窗里。
"""
import argparse
import sys
import time

import serial

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("cmds", nargs="?", default="")
    ap.add_argument("secs", nargs="?", type=float, default=3.0)
    ap.add_argument("--boot-wait", type=float, default=1.8,
                    help="开端口后等固件启动的秒数（复位后的空窗）")
    ap.add_argument("-o", "--out", default="", help="把原始响应存文件")
    ap.add_argument("--deadline", type=float, default=30.0, help="整体硬超时（秒）")
    a = ap.parse_args()

    t_end = time.time() + a.deadline
    raw = bytearray()

    def note(msg):
        sys.stderr.write("[msh] %s\n" % msg)
        sys.stderr.flush()

    def pump(seconds):
        t0 = time.time()
        while time.time() - t0 < seconds and time.time() < t_end:
            try:
                d = s.read(4096)
            except Exception as e:                       # 拔线/被抢
                note("read error: %s" % e)
                return
            if d:
                raw.extend(d)
                sys.stdout.write(d.decode("utf-8", errors="replace"))
                sys.stdout.flush()

    s = serial.Serial(a.port, 115200, timeout=0.1, write_timeout=0.5)
    try:
        s.setDTR(False)
        s.setRTS(False)
    except Exception:
        pass
    # 不清输入缓冲：开端口会让芯片复位，开机日志紧接着就来，清了就丢开头（实测掉过横幅）

    note("waiting %.1fs for firmware boot after port-open reset" % a.boot_wait)
    pump(a.boot_wait)

    cmds = [c for c in a.cmds.split("|") if c.strip()]
    for i, c in enumerate(cmds):
        sys.stdout.write("\n>>> %s\n" % c)
        sys.stdout.flush()
        try:
            s.write(c.encode() + b"\r")
        except Exception as e:
            note("write error: %s" % e)
            break
        pump(a.secs if i == len(cmds) - 1 else 1.0)

    if not cmds:
        pump(a.secs)

    s.close()
    if a.out:
        with open(a.out, "wb") as f:
            f.write(bytes(raw))
        note("saved %d bytes to %s" % (len(raw), a.out))
    sys.stdout.write("\n[msh] received %d bytes\n" % len(raw))
    return 0


if __name__ == "__main__":
    sys.exit(main())
