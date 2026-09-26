/*===========================================================================
 * syscalls_stub.c -- newlib 系统调用桩
 *
 * 我们是裸机 + -nostartfiles，但链接时会顺带链上 newlib（内核要用
 * memcpy/memset 之类）。一旦 libc 里有目标文件被拉进来，它就可能要这些
 * syscall 符号；这里全给成"失败/无操作"，避免为它们去实现真的文件系统。
 *
 * 注意：控制台**不走** _write —— RT-Thread 的 rt_kprintf 直接调
 * rt_hw_console_output()（bsp/drv_usj.c），不经过 stdio。
 *===========================================================================*/

#include <stddef.h>

int _close(int fd)                    { (void)fd; return -1; }
int _fstat(int fd, void *st)          { (void)fd; (void)st; return -1; }
int _isatty(int fd)                   { (void)fd; return 1; }
int _lseek(int fd, int off, int dir)  { (void)fd; (void)off; (void)dir; return 0; }
int _read(int fd, void *buf, int len) { (void)fd; (void)buf; (void)len; return 0; }
int _write(int fd, const void *buf, int len) { (void)fd; (void)buf; return len; }
int _getpid(void)                     { return 1; }
int _kill(int pid, int sig)           { (void)pid; (void)sig; return -1; }
void _exit(int code)                  { (void)code; while (1) { } }

/* 没有堆给 libc 用：RT-Thread 的堆由 rt_system_heap_init 自己管 */
void *_sbrk(int incr)                 { (void)incr; return (void *)-1; }

/*===========================================================================
 * timegm()：newlib 只在 _GNU_SOURCE 下才导出它，而本工程的 POSIX 可见性被钉在
 * 1990（tools/build.ps1 的 `-D_POSIX_C_SOURCE=1`，RTT 那套 POSIX 头要求的），
 * 于是链接期 undefined。谁要它：elmfat 的官方移植层
 * （dfs_elm.c:841 `st->st_mtime = timegm(&tm_file)`）。
 *
 * 算法 = days_from_civil（Howard Hinnant 的公认写法），只依赖 tm_year/mon/mday/
 * hour/min/sec，不看 tm_wday/tm_yday，对 1970 前后都正确。
 *===========================================================================*/
/*===========================================================================
 * _gettimeofday()：newlib 的 time()/gettimeofday() 最终都落到这里，而本工具链
 * 自带的那个桩**不是"返回 ENOSYS"，而是往地址 0 写**（链接日志里那句
 * `warning: _gettimeofday is not implemented and will always fail` 就是它）。
 * 🚨 后果是真板上一头栽进异常处理：`mkfs onboard0` → FatFs 的 f_mkfs 要一个随机
 *    种子 → elmfat 的 get_fattime() → time() → _gettimeofday → store access fault
 *    （mtval=0），串口当场没声了（2026-09-26 踩过）。
 * 我们没有 RTC，给"固定基准时间 + 开机以来的 tick"就够用：FatFs 拿它填文件
 * 时间戳，Windows 上看到的就是 2026-01-01 之后的时间。
 *===========================================================================*/
#include <time.h>
#include <sys/time.h>
#include <rtthread.h>

#define S31_TIME_EPOCH_BASE   1767225600L    /* 2026-01-01 00:00:00 UTC */

int _gettimeofday(struct timeval *tv, void *tz)
{
    rt_tick_t tk;

    (void)tz;
    if (tv != NULL)
    {
        tk = rt_tick_get();
        tv->tv_sec  = (time_t)(S31_TIME_EPOCH_BASE + (long)(tk / RT_TICK_PER_SECOND));
        tv->tv_usec = (suseconds_t)((tk % RT_TICK_PER_SECOND) *
                                    (1000000u / RT_TICK_PER_SECOND));
    }
    return 0;
}

static long s31_days_from_civil(long y, unsigned m, unsigned d)
{
    long era;
    unsigned yoe, doy, doe;
    int mp = (int)m + ((m > 2u) ? -3 : 9);      /* 三月起算的"月" */

    y -= (m <= 2u) ? 1 : 0;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);                                  /* [0, 399] */
    doy = (unsigned)((153 * mp + 2) / 5) + d - 1u;                    /* [0, 365] */
    doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;                   /* [0, 146096] */
    return era * 146097L + (long)doe - 719468L;
}

time_t timegm(struct tm *tm)
{
    long days;

    if (tm == NULL)
    {
        return (time_t)-1;
    }
    days = s31_days_from_civil((long)tm->tm_year + 1900L,
                               (unsigned)(tm->tm_mon + 1),
                               (unsigned)tm->tm_mday);
    return (time_t)(days * 86400L + (long)tm->tm_hour * 3600L +
                    (long)tm->tm_min * 60L + (long)tm->tm_sec);
}
