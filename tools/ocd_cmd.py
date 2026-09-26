"""往正在运行的 OpenOCD telnet(4444) 发一条命令并打印回复。

用法：
    python tools\\ocd_cmd.py "reg"                 # 列出所有寄存器
    python tools\\ocd_cmd.py "reg mintthresh"      # 读某个寄存器
    python tools\\ocd_cmd.py "mdw 0x2f05a760 8"    # 读内存
    python tools\\ocd_cmd.py "mww 0x20399054 1"    # 写内存（慎用）
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("rttmod", os.path.join(HERE, "rtt.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

if len(sys.argv) < 2:
    sys.exit(__doc__)
o = m.Ocd()
out = o.cmd(sys.argv[1])
print(out if isinstance(out, str) else repr(out))
o.close()
