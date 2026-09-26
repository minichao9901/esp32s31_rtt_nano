#!/usr/bin/env python3
"""s31_serial.py —— 打开板子的 USB-Serial/JTAG 口，并把芯片"复位到 app"。

为什么需要这个模块（2026-09-26 定论，本工程被它坑过好几轮）
--------------------------------------------------------
pyserial 打开 COM 口时，Windows 的 usbser.sys 会把 **DTR 和 RTS 都拉起来**。
在这颗芯片上这两个信号不是装饰：
    DTR 断言（低）= BOOT 引脚被按下   → 复位时进 **ROM 下载模式**
    RTS 断言（低）= EN  引脚拉低      → 触发一次 `rst:0x17 (CHIP_USB_UART_RESET)`
所以"开串口 → 紧接着 setDTR(False)/setRTS(False)"这种写法是**错的**：
复位已经发生过了，芯片此刻正坐在 ROM 下载模式里（JTAG 采 PC 会是 0x2F8xxxxx），
现象就是**串口 0 字节、板子看起来哑了** —— 其实 app 根本没跑。

esptool 用的是另一套序列（esp_pylib.serial_reset）：
    * 进下载模式：DTR 低 + RTS 脉冲（bootloader 复位）
    * **重启 app  ：DTR 保持高（BOOT 不按下）+ RTS 脉冲（EN）**
这里照抄后者，于是"开串口"变成"干净地重启一次 app"。

`PIN_LOW = True` 这个命名跟着 esp_pylib：断言 = 物理拉低。
"""
import time

import serial

PIN_LOW = True      # 断言（物理低）
PIN_HIGH = False    # 释放（物理高）


def set_dtr(s, state):
    s.setDTR(state)


def set_rts(s, state):
    """RTS 写法照抄 esp_pylib：Windows 的 usbser.sys 只在 RTS 变化时下发
    SET_CONTROL_LINE_STATE，所以每次改 RTS 后补写一次当前 DTR 值。"""
    s.setRTS(state)
    s.setDTR(s.dtr)


def hard_reset(s, hold=0.2, after=0.2):
    """脉冲 EN（RTS），期间把 BOOT（DTR）保持在"不按下" —— 芯片会从 flash 启 app。"""
    set_dtr(s, PIN_HIGH)
    set_rts(s, PIN_LOW)         # EN 低 = 复位
    time.sleep(hold)
    set_rts(s, PIN_HIGH)        # EN 高 = 放开
    set_dtr(s, PIN_HIGH)
    time.sleep(after)


def enter_bootloader(s, settle=0.1):
    """esptool 的 USB-JTAG 进下载模式序列（要用 esptool 烧录时才需要）。"""
    set_rts(s, PIN_HIGH)
    set_dtr(s, PIN_HIGH)
    time.sleep(settle)
    set_dtr(s, PIN_LOW)
    set_rts(s, PIN_HIGH)
    time.sleep(settle)
    set_rts(s, PIN_LOW)
    set_dtr(s, PIN_HIGH)
    set_rts(s, PIN_LOW)
    time.sleep(settle)
    set_dtr(s, PIN_HIGH)
    set_rts(s, PIN_HIGH)


def open_port(port, baud=115200, timeout=0.2):
    return serial.Serial(port, baud, timeout=timeout, write_timeout=1.0)


def boot_and_open(port, baud=115200, timeout=0.2, boot_wait=2.0, reset=True,
                  wait_prompt=True, prompt=b"msh />", quiet=False):
    """开端口 →（默认）硬复位到 app → 等它把启动日志吐出来。

    reset=False 时**不动复位线**：适合"板子已经在跑、只想看后续输出"的场合
    （比如它刚从 JTAG 烧录复位过）。注意开端口本身仍会断言 DTR/RTS，
    所以这一步也把两条线显式摆回"不复位"状态。
    """
    s = open_port(port, baud, timeout)
    if reset:
        hard_reset(s)
    else:
        set_dtr(s, PIN_HIGH)
        set_rts(s, PIN_HIGH)

    buf = bytearray()
    deadline = time.time() + (boot_wait if reset else min(boot_wait, 0.5))
    while time.time() < deadline:
        try:
            d = s.read(4096)
        except Exception as e:
            if not quiet:
                print("[s31_serial] read error: %s" % e, flush=True)
            break
        if d:
            buf.extend(d)
            if wait_prompt and prompt in buf:
                break
    if not quiet:
        print("[s31_serial] 开端口 %s（reset=%s）：收到 %d 字节启动输出%s"
              % (port, reset, len(buf), "，已到 msh 提示符" if prompt in buf else ""),
              flush=True)
    return s, bytes(buf)


if __name__ == "__main__":
    import sys
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    p = sys.argv[1] if len(sys.argv) > 1 else "COM43"
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0
    sp, boot = boot_and_open(p, boot_wait=secs)
    sys.stdout.write(boot.decode("utf-8", errors="replace"))
    t0 = time.time()
    while time.time() - t0 < secs:
        d = sp.read(4096)
        if d:
            sys.stdout.write(d.decode("utf-8", errors="replace"))
            sys.stdout.flush()
    sp.close()
