#!/usr/bin/env python3
"""rtt.py —— SEGGER RTT 主机端（自己实现，走 OpenOCD 的 telnet 口读写内存）。

    python tools\\rtt.py                          # 只听 6 秒
    python tools\\rtt.py "help|ps|free" 3          # 敲命令（语法同 msh.py）
    python tools\\rtt.py --keep                    # 当监视器：一直听，Ctrl+C 停
    python tools\\rtt.py --attach "ls /disk" 3     # 复用已经在跑的 OpenOCD(4444)
    python tools\\rtt.py --addr 0x2f04c2a0 3       # 手工指定控制块地址（默认从 ELF 里读符号）

为什么不用 OpenOCD 自带的 RTT
------------------------------
OpenOCD 0.12 的命令组里**只有 `rtt server`**（`rtt setup` / `rtt start` 在这份
ESP-IDF fork 里没有注册，`help rtt` 只列出 server），所以走不了"官方主机端"。

于是这里直接把 RTT 协议自己实现一遍 —— 它本来就只有三件事：
  1. 找到控制块（RAM 里以 "SEGGER RTT" 开头的那个结构）；
  2. 上行走 ring buffer：读 WrOff/RdOff，把 [RdOff, WrOff) 读出来，再把 RdOff 写回 WrOff；
  3. 下行同理反过来（主机写数据、推 WrOff）。
内存访问全部借 OpenOCD 的 telnet 命令：批量用 `dump_image` / `load_image`（一条命令搬一大块），
指针用 `mdw` / `mww`。控制块地址优先从 ELF 的 `_SEGGER_RTT` 符号取（最稳），
取不到就在 RAM 窗口里扫 "SEGGER RTT" —— 和真正的 RTT 主机一个做法。

⚠️ OpenOCD 独占 JTAG：本脚本跑着时不能烧录（先关掉它，或 --attach 复用）。
"""
import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

RAM_BASE = 0x2F000000
RAM_SIZE = 0x00080000          # linker.ld 的 RAM 窗口 0x2F000000..0x2F07AFC0
CB_ID = b"SEGGER RTT"
UP_ENTRY = 24                  # SEGGER_RTT_BUFFER_UP / DOWN 都是 6 个 32 位字段
RTT_TMP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "rtt_tmp.bin")


# --------------------------------------------------------------------------
# OpenOCD telnet 客户端
# --------------------------------------------------------------------------
class Ocd:
    def __init__(self, host="127.0.0.1", port=4444):
        self.s = socket.create_connection((host, port), timeout=3.0)
        self.s.settimeout(3.0)
        self.buf = b""
        time.sleep(0.3)
        self._drain()

    def _drain(self):
        self.s.settimeout(0.2)
        try:
            while True:
                d = self.s.recv(65536)
                if not d:
                    break
        except socket.timeout:
            pass
        except OSError:
            pass
        self.s.settimeout(3.0)

    def cmd(self, line):
        self.s.sendall(line.encode() + b"\n")
        out = b""
        deadline = time.time() + 5.0
        while time.time() < deadline:
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                break
            if not d:
                break
            out += d
            if out.endswith(b"> "):          # OpenOCD telnet 提示符
                break
        return out.decode("utf-8", errors="replace")

    def mdw(self, addr, count=1):
        out = self.cmd("mdw 0x%08x %d" % (addr, count))
        # OpenOCD 的输出形如：  0x2f04d3ec: 2f01af3c 2f04c2a4 00000100 ...
        # ⚠️ 地址带 0x、**值不带** —— 用 `0x(...)` 去抓值只会抓到地址那一个
        #    （第一版就这么错的：mdw 明明成功，却被判成"失败"）。
        m = re.search(r"0x[0-9a-fA-F]+:\s*((?:[0-9a-fA-F]{8}[ \t]*)+)", out)
        if m:
            vals = re.findall(r"[0-9a-fA-F]{8}", m.group(1))
        else:                                   # 兜底：某些版本会带 0x 前缀
            vals = re.findall(r"0x([0-9a-fA-F]{8})", out)[1:]
        if len(vals) < count:
            raise RuntimeError("mdw 失败 @0x%08x: %r" % (addr, out))
        return [int(v, 16) for v in vals[:count]]

    def mww(self, addr, val):
        self.cmd("mww 0x%08x 0x%08x" % (addr, val & 0xFFFFFFFF))

    def dump(self, addr, length):
        if length <= 0:
            return b""
        self.cmd("dump_image {%s} 0x%08x %d" % (RTT_TMP, addr, length))
        with open(RTT_TMP, "rb") as f:
            data = f.read()
        if len(data) < length:
            # dump 长度不足时尾部补 0（RTT 侧会按 WrOff/RdOff 截断，不会用到）
            data += b"\x00" * (length - len(data))
        return data[:length]

    def load(self, addr, data):
        if not data:
            return
        with open(RTT_TMP, "wb") as f:
            f.write(data)
        self.cmd("load_image {%s} 0x%08x bin" % (RTT_TMP, addr))

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


# --------------------------------------------------------------------------
# RTT 协议
# --------------------------------------------------------------------------
class Rtt:
    def __init__(self, ocd, cb_addr=None, elf=None):
        self.o = ocd
        self.cb = cb_addr or self._sym_addr(elf) or self._scan()
        if not self.cb:
            raise RuntimeError("RAM 里没找到 SEGGER RTT 控制块（固件编进去了吗？）")
        hdr = self.o.dump(self.cb, 24)
        if hdr[:10] != CB_ID[:10]:
            raise RuntimeError("0x%08x 处不是 RTT 控制块: %r" % (self.cb, hdr[:16]))
        self.max_up, self.max_down = struct.unpack("<II", hdr[16:24])
        self.up_base = self.cb + 24
        self.down_base = self.up_base + UP_ENTRY * self.max_up

    def _sym_addr(self, elf):
        if not elf or not os.path.exists(elf):
            return None
        import glob
        pat = os.path.join(os.path.expanduser("~"), ".espressif", "tools", "riscv32-esp-elf",
                           "*", "riscv32-esp-elf", "bin", "riscv32-esp-elf-nm.exe")
        hits = sorted(glob.glob(pat))
        if not hits:
            return None
        out = subprocess.run([hits[-1], elf], capture_output=True, text=True, errors="replace").stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[2] == "_SEGGER_RTT":
                return int(parts[0], 16)
        return None

    def _scan(self):
        step = 0x1000
        off = 0
        while off < RAM_SIZE:
            n = min(step, RAM_SIZE - off)
            chunk = self.o.dump(RAM_BASE + off, n)
            i = chunk.find(CB_ID)
            if i >= 0:
                return RAM_BASE + off + i
            off += n
        return None

    def _buf(self, base, ch):
        w = self.o.mdw(base + UP_ENTRY * ch, 6)
        # 0:sName 1:pBuffer 2:SizeOfBuffer 3:WrOff 4:RdOff 5:Flags
        return {"name": w[0], "buf": w[1], "size": w[2], "wr": w[3], "rd": w[4], "flags": w[5]}

    def up_read(self, ch=0):
        """把上行缓冲里主机还没取走的字节读出来，并推进 RdOff。"""
        b = self._buf(self.up_base, ch)
        if b["size"] == 0 or b["wr"] == b["rd"]:
            return b""
        if b["wr"] > b["rd"]:
            data = self.o.dump(b["buf"] + b["rd"], b["wr"] - b["rd"])
        else:                                   # 绕回
            data = self.o.dump(b["buf"] + b["rd"], b["size"] - b["rd"])
            data += self.o.dump(b["buf"], b["wr"])
        self.o.mww(self.up_base + UP_ENTRY * ch + 16, b["wr"])   # RdOff = WrOff
        return data

    def down_write(self, data, ch=0):
        """往下行缓冲写（= 给固件送输入），推进 WrOff。返回写入字节数。"""
        b = self._buf(self.down_base, ch)
        if b["size"] == 0:
            return 0
        free = (b["size"] - 1 + b["rd"] - b["wr"]) % b["size"]
        n = min(len(data), free)
        if n <= 0:
            return 0
        part1 = min(n, b["size"] - b["wr"])
        self.o.load(b["buf"] + b["wr"], data[:part1])
        if n > part1:
            self.o.load(b["buf"], data[part1:n])
        self.o.mww(self.down_base + UP_ENTRY * ch + 12, (b["wr"] + n) % b["size"])
        return n


# --------------------------------------------------------------------------
def find_openocd():
    import glob
    hits = sorted(glob.glob(os.path.join(os.path.expanduser("~"), ".espressif", "tools",
                                         "openocd-esp32", "*", "openocd-esp32", "bin", "openocd.exe")))
    if not hits:
        sys.exit("找不到 openocd.exe")
    return hits[-1]


def start_openocd():
    ocd = find_openocd()
    scripts = os.path.join(os.path.dirname(os.path.dirname(ocd)), "share", "openocd", "scripts")
    cfg = os.path.join(scripts, "board", "esp32s31-builtin.cfg")
    log = open(os.path.join(os.path.dirname(RTT_TMP), "rtt_openocd.log"), "wb")
    p = subprocess.Popen([ocd, "-s", scripts, "-f", cfg, "-c", "init"],
                         stdout=log, stderr=subprocess.STDOUT)
    t0 = time.time()
    while time.time() - t0 < 10.0:
        if p.poll() is not None:
            sys.exit("OpenOCD 自己退了，看 build\\rtt_openocd.log")
        try:
            socket.create_connection(("127.0.0.1", 4444), timeout=0.3).close()
            return p
        except OSError:
            time.sleep(0.2)
    p.kill()
    sys.exit("等 OpenOCD telnet(4444) 超时")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmds", nargs="?", default="", help="| 分隔的命令；留空只听")
    ap.add_argument("secs", nargs="?", type=float, default=6.0, help="最后一条命令后听多久")
    ap.add_argument("--attach", action="store_true", help="复用已跑的 OpenOCD")
    ap.add_argument("--keep", action="store_true", help="不退出（监视器模式）")
    ap.add_argument("--addr", default=None, help="控制块地址（默认取自 ELF 符号 _SEGGER_RTT）")
    ap.add_argument("--elf", default=os.path.join(os.path.dirname(RTT_TMP), "app.elf"))
    ap.add_argument("--poll", type=float, default=0.02, help="轮询间隔（秒）")
    ap.add_argument("--reset", action="store_true",
                    help="敲命令前先硬复位一次。⚠️ 开 OpenOCD **不保证**复位芯片："
                         "上一轮的挂载/播放状态会留着（同一个启动周期里重复 mount 会失败），"
                         "要可复现就从复位开始")
    a = ap.parse_args()

    proc = start_openocd() if not a.attach else None
    total = 0
    try:
        o = Ocd()
        if a.reset:
            print("[rtt] 硬复位芯片 ...")
            o.cmd("reset run")
            time.sleep(2.0)          # 等它把启动日志打完（这些日志也会从 RTT 出来）
        rtt = Rtt(o, cb_addr=int(a.addr, 16) if a.addr else None, elf=a.elf)
        print("[rtt] 控制块 @0x%08x（up x%d / down x%d）" % (rtt.cb, rtt.max_up, rtt.max_down))

        def pump(seconds):
            nonlocal total
            t0 = time.time()
            while time.time() - t0 < seconds:
                d = rtt.up_read(0)
                if d:
                    total += len(d)
                    sys.stdout.write(d.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
                else:
                    time.sleep(a.poll)

        cmds = [c for c in a.cmds.split("|") if c.strip()]
        if not cmds:
            if a.keep:
                while True:
                    pump(0.5)
            else:
                pump(a.secs)
        else:
            for i, c in enumerate(cmds):
                sys.stdout.write("\n>>> %s\n" % c)
                sys.stdout.flush()
                n = rtt.down_write(c.encode() + b"\r")
                if n < len(c) + 1:
                    sys.stdout.write("[rtt] ⚠️ 下行只写进 %d/%d 字节（固件没在取？）\n"
                                     % (n, len(c) + 1))
                pump(1.5 if i < len(cmds) - 1 else a.secs)
    except KeyboardInterrupt:
        print("\n[rtt] Ctrl+C")
    finally:
        if proc is not None:
            proc.kill()
            proc.wait(timeout=3)
    print("\n[rtt] 共收到 %d 字节" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
