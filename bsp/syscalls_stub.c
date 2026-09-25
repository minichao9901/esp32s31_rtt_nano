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
