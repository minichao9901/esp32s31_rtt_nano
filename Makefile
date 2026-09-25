# ============================================================
#  rtt_nano_s31 —— RT-Thread Nano + msh on ESP32-S31（**不依赖 idf.py**）
#
#    make help        目标一览（默认目标）
#    make build       编译 -> build\app.bin
#    make flash       编译 + 烧到 flash 0x2000 + 读 4 秒日志
#    make run         flash 之后一直看串口
#    make msh MSH="help|psram_info|free"    敲 msh 命令并收响应
#
#  说明：
#    - 逻辑全在 tools\*.ps1 / tools\*.py，这里只做薄封装（跟工作区另外两个
#      工程 boot_msc_s31 / s31_app 一个套路）
#    - 本机没有 sh.exe，显式用 cmd.exe 当 SHELL，必须配 /c
#    - 端口默认取 local.env.ps1 里的 ESP32_S31_PORT（本机 COM43 = USB-DBG）
#      临时换： make monitor PORT=COM7
#    - 读串口秒数 SECONDS：0 = 一直读（Ctrl+C 退出）
#    - 🚨 镜像烧在 flash 0x2000（ROM 的二级镜像位置）：会和 IDF 的二级 bootloader
#      打架；本工作区 boot_msc_s31（MSC 拖拽 bootloader）也烧在那儿，来回切要重烧
# ============================================================

SHELL       := cmd.exe
.SHELLFLAGS := /c

PORT ?=
# 读串口读多少秒；0 = 一直读（Ctrl+C 退出）。想只看一段：make monitor SECONDS=8
SECONDS ?= 0
# make msh 要敲的命令，多条用 | 分隔
MSH ?= help

PWSH := pwsh -NoProfile -ExecutionPolicy Bypass -File
MK   := $(PWSH) tools/make.ps1

.DEFAULT_GOAL := help

.PHONY: help build rebuild clean size flash run monitor msh watch tail reset safe headers fetch doc

help:
	@$(MK) help

build:
	@$(MK) build -Port "$(PORT)"

rebuild:
	@$(MK) rebuild -Port "$(PORT)"

clean:
	@$(MK) clean

size:
	@$(MK) size

flash:
	@$(MK) flash -Port "$(PORT)" -Seconds $(SECONDS)

# flash + 一直看串口（SECONDS=8 就只看 8 秒）
run:
	@$(MK) run -Port "$(PORT)" -Seconds $(SECONDS)

monitor:
	@$(MK) monitor -Port "$(PORT)" -Seconds $(SECONDS)

# 敲 msh 命令： make msh MSH="psram_speed 256"
msh:
	@$(MK) msh -Port "$(PORT)" -Msh "$(MSH)" -Seconds $(SECONDS)

# 不复位地观察串口（板子在跑、不想打断它）
watch:
	@$(MK) watch -Port "$(PORT)" -Seconds $(SECONDS)

# 容错观察：设备掉了自动重开 —— 看"是不是在反复复位"
tail:
	@$(MK) tail -Port "$(PORT)" -Seconds $(SECONDS)

# 手动拉 EN 复位一次再听（板子"不理人"时用）
reset:
	@$(MK) reset -Port "$(PORT)" -Seconds $(SECONDS)

# 40MHz 安全档（救砖）：完全不碰时钟树
safe:
	@$(MK) safe -Port "$(PORT)" -Seconds $(SECONDS)

# 维护类：重新冻结 IDF 头 / 重拉 RT-Thread / 渲染文档 PDF
headers:
	@$(MK) headers

fetch:
	@$(MK) fetch

doc:
	@$(MK) doc
