"""起一个 OpenOCD 并**保持运行**（等 telnet 4444 就绪），供 rtt.py --attach / gdb :3333 复用。

用法（放到后台跑，用完 kill 掉）：
    python tools\\ocd_keep.py
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("rttmod", os.path.join(HERE, "rtt.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

proc = m.start_openocd()
print("OpenOCD 已就绪 pid=%d（telnet 4444 / gdb 3333），Ctrl+C 退出" % proc.pid, flush=True)
try:
    proc.wait()
except KeyboardInterrupt:
    proc.kill()
