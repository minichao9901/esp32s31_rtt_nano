/*===========================================================================
 * sdkconfig.h -- 极简版（**不是** IDF 生成的那份 84KB 大文件）
 *
 * 为什么需要它：IDF 的头文件（`hal/assert.h` 等）会 `#include "sdkconfig.h"`，
 * 而本工程不链 IDF、也没有自己的 sdkconfig。这里只定义
 * **编译那几个 IDF LL 头所必需**的宏，别的一个都不要。
 *
 * 目前只用到两个：
 *   CONFIG_HAL_DEFAULT_ASSERTION_LEVEL
 *       IDF 的 HAL_ASSERT 级别：0=不做断言(未定义行为) 1=失败调 abort() 2=带文件名/行号
 *       这里取 1 —— 万一 LL 函数被传了非法参数，会**响亮地停住**而不是默默跑飞。
 *       abort() 由 bsp/mini_libc.c 提供（打印一行后停机）。
 *   CONFIG_LIBC_PICOLIBC
 *       不定义（=0）：IDF 的 hal/assert.h 只拿它决定 __noreturn 怎么写，两种都能编。
 *===========================================================================*/
#pragma once

#define CONFIG_HAL_DEFAULT_ASSERTION_LEVEL 1
