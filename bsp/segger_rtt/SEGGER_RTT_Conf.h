/*********************************************************************
*                   (c) SEGGER Microcontroller GmbH                  *
*                        The Embedded Experts                        *
*                           www.segger.com                           *
**********************************************************************
*                                                                    *
*        SEGGER RTT * Real Time Transfer for embedded targets        *
*                  https://github.com/SEGGERMicro/RTT                *
*                                                                    *
**********************************************************************

---------------------------END-OF-HEADER------------------------------
Purpose : User configuration file for RTT (ESP32-S31 / RT-Thread 那份配置).
          For available configuration,
          refer to SEGGER_RTT_ConfDefaults.h.

⚠️ 这个文件是 SEGGER 留给我们改的（官方源文件 SEGGER_RTT.c/h 一个字没动）。
   本工程的取值与理由：

   BUFFER_SIZE_UP 4096 —— 开机那一串日志（psram/spi/sfud/onboard ≈ 2KB）一次
     全塞得下，主机来不及取也不会丢。RTT 的上行缓冲是"主机轮询"模式，
     缓冲区越大、越不容易在开机瞬间丢字。
   BUFFER_SIZE_DOWN 256 —— 命令行输入用，够一条 msh 命令。
   SEGGER_RTT_MODE_NO_BLOCK_SKIP —— **绝不能让日志阻塞目标**（控制台卡死比丢日志严重）。
   SEGGER_RTT_LOCK/UNLOCK —— 用 RT-Thread 的关中断原语：rt_kprintf 可能同时从
     线程和中断里进来，没有锁会把环形缓冲的指针写乱。
     （宏的书写方式跟 SEGGER 在 ConfDefaults 里的示例一致：LOCK 开一个 `{`、
       UNLOCK 关掉它，中间那个 _SEGGER_RTT__LockState 是同作用域的局部变量。）
----------------------------------------------------------------------
*/

#ifndef SEGGER_RTT_CONF_H
#define SEGGER_RTT_CONF_H

#include <rtthread.h>

/*********************************************************************
*
*       Defines, configurable
*
**********************************************************************
*/

#define BUFFER_SIZE_UP                            (4096)
#define BUFFER_SIZE_DOWN                          (256)

#define SEGGER_RTT_MAX_NUM_UP_BUFFERS             (2)
#define SEGGER_RTT_MAX_NUM_DOWN_BUFFERS           (2)

#define SEGGER_RTT_MODE_DEFAULT                   SEGGER_RTT_MODE_NO_BLOCK_SKIP

#define SEGGER_RTT_LOCK()   {                                     \
                              rt_base_t _SEGGER_RTT__LockState;    \
                              _SEGGER_RTT__LockState = rt_hw_interrupt_disable();

#define SEGGER_RTT_UNLOCK()   rt_hw_interrupt_enable(_SEGGER_RTT__LockState); \
                            }

#endif
/*************************** End of file ****************************/
