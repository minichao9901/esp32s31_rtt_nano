#!/usr/bin/env python3
"""读串口 N 秒并原样打印（UTF-8 + errors=replace，和本工作区 capture_boot.py 同一套策略）。

用法: python read_port.py COM43 [秒数]

注意：pyserial 打开端口默认 dtr=rts=True，会把板子按进下载模式 →
这里显式置 False（USB-Serial/JTAG 上这两个信号由外设转成复位/strapping 行为）。
"""
import sys
import time

import serial

# Windows 控制台默认 GBK：中文日志会乱码，明确改成 UTF-8 + 替换（工作区惯例）
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

port = sys.argv[1]
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0

s = serial.Serial(port, 115200, timeout=0.2)
try:
    s.setDTR(False)
    s.setRTS(False)
except Exception:
    pass
# 注意：这里**不要** reset_input_buffer()。
# 打开端口会让芯片复位（USB-JTAG 的 DTR/RTS 就是复位线），开机日志紧接着就来了，
# 清输入缓冲会把复位后最早那批字节直接丢掉（实测横幅开头缺字）。

t0 = time.time()
data = b""
while time.time() - t0 < secs:
    try:
        d = s.read(4096)
    except Exception as e:  # 端口被拔/被别的进程抢
        sys.stdout.write("\n[read error] %s\n" % e)
        break
    if d:
        data += d
        sys.stdout.write(d.decode("utf-8", errors="replace"))
        sys.stdout.flush()
s.close()

sys.stdout.write("\n[read_port] %d bytes in %.1fs\n" % (len(data), secs))
