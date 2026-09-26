/*===========================================================================
 * wav_player.c -- msh 命令 `wav_play`：把 FAT 里的 WAV 文件丢给 sound0 播
 *
 * 数据流：DFS 读文件 → 解析 RIFF/WAVE 头 → 转成 sound0 要的格式 → rt_device_write()
 *   · 采样率 / 声道 / 位深直接告诉 audio 设备（`AUDIO_CTL_CONFIGURE`），
 *     **不做重采样**（PWM 驱动的采样时钟按你给的速率走，8k~48k 都支持）；
 *   · 只支持未压缩 PCM（fmt=1）。碰到别的编码直接报错 —— 不猜。
 *
 * 用法：
 *   wav_play /STAR8.WAV        播
 *   wav_play -i /STAR8.WAV     只看文件头（不播）—— 排查"没声音"时先看这个
 *   wav_play -v 2 /STAR8.WAV   音量减半（右移 2 位；0 = 满音量）
 *   wav_play -l 8 /STAR8.WAV   只播前 8 秒
 *
 * 🚨 两个真板踩过的点：
 *   ① **播之前必须 `mount onboard0 / elm`**（DFS v1 没有虚拟根目录，
 *      没挂文件系统时 `/x.wav` 一律 "No such file"）。
 *   ② 文件是被 PC 通过 MSC 写进来的，写完后**先在 PC 上"安全弹出"、再 `msc stop`**，
 *      否则你读到的可能是半截数据（FAT 目录项已建、簇链还没落盘）。
 *===========================================================================*/

#include <rtthread.h>
#include <rtdevice.h>
#include <dfs_file.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

#define WAV_BUF_BYTES   2048        /* 每块喂给音频设备的字节数 */

struct wav_hdr
{
    /* RIFF */
    char     riff[4];
    rt_uint32_t riff_size;
    char     wave[4];
    /* fmt */
    char     fmt[4];
    rt_uint32_t fmt_size;
    rt_uint16_t audio_format;
    rt_uint16_t channels;
    rt_uint32_t samplerate;
    rt_uint32_t byterate;
    rt_uint16_t block_align;
    rt_uint16_t bits;
};

static void wav_dump(const struct wav_hdr *h, rt_uint32_t data_bytes)
{
    rt_kprintf("  RIFF/WAVE  : %c%c%c%c / %c%c%c%c\n",
               h->riff[0], h->riff[1], h->riff[2], h->riff[3],
               h->wave[0], h->wave[1], h->wave[2], h->wave[3]);
    rt_kprintf("  编码       : %u（1 = PCM 未压缩）\n", (unsigned)h->audio_format);
    rt_kprintf("  声道/采样率: %u ch / %u Hz\n", (unsigned)h->channels, (unsigned)h->samplerate);
    rt_kprintf("  位深/块对齐: %u bit / %u 字节\n", (unsigned)h->bits, (unsigned)h->block_align);
    rt_kprintf("  PCM 数据   : %u 字节 = %u 毫秒\n", (unsigned)data_bytes,
               (unsigned)((rt_uint64_t)data_bytes * 1000u /
                          (h->byterate ? h->byterate : 1u)));
}

static void wav_play(int argc, char **argv)
{
    const char *path = RT_NULL;
    int info_only = 0, vol_shift = 0, max_sec = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (rt_strcmp(argv[i], "-i") == 0) {
            info_only = 1;
        } else if (rt_strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
            vol_shift = atoi(argv[++i]);
        } else if (rt_strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            max_sec = atoi(argv[++i]);
        } else {
            path = argv[i];
        }
    }
    if (path == RT_NULL) {
        rt_kprintf("用法: wav_play [-i] [-v 右移位数] [-l 秒数] <文件>\n");
        rt_kprintf("  例: wav_play /STAR8.WAV       （板载 flash 挂在 / 上）\n");
        rt_kprintf("      wav_play /ext/B.WAV       （外接 flash 挂在 /ext 上）\n");
        return;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        rt_kprintf("[wav] 打不开 %s（先 mount？看 `ls /`）\n", path);
        return;
    }

    /* ---- 解析头部：先读 12 字节 RIFF 头，再按 chunk 往下找 fmt / data ---- */
    struct wav_hdr h;
    rt_uint8_t head[12];

    if (read(fd, head, sizeof(head)) != (int)sizeof(head) ||
        memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "WAVE", 4) != 0) {
        rt_kprintf("[wav] %s 不是 RIFF/WAVE 文件\n", path);
        close(fd);
        return;
    }
    memcpy(h.riff, head, 4);
    memcpy(&h.riff_size, head + 4, 4);
    memcpy(h.wave, head + 8, 4);

    rt_uint32_t data_bytes = 0;
    int got_fmt = 0;
    for (;;) {
        char ck[8];
        if (read(fd, ck, 8) != 8) {
            break;
        }
        rt_uint32_t sz = (rt_uint8_t)ck[4] | ((rt_uint8_t)ck[5] << 8) |
                         ((rt_uint8_t)ck[6] << 16) | ((rt_uint8_t)ck[7] << 24);
        if (memcmp(ck, "fmt ", 4) == 0 && sz >= 16) {
            rt_uint8_t fb[16];
            if (read(fd, fb, 16) != 16) {
                break;
            }
            memcpy(h.fmt, ck, 4);
            memcpy(&h.fmt_size, ck + 4, 4);
            memcpy(&h.audio_format, fb + 0, 2);
            memcpy(&h.channels,     fb + 2, 2);
            memcpy(&h.samplerate,   fb + 4, 4);
            memcpy(&h.byterate,     fb + 8, 4);
            memcpy(&h.block_align,  fb + 12, 2);
            memcpy(&h.bits,         fb + 14, 2);
            got_fmt = 1;
            /* 跳过 fmt 里多出来的扩展字节 */
            for (rt_uint32_t k = 16; k < sz; k++) {
                char dummy;
                if (read(fd, &dummy, 1) != 1) {
                    break;
                }
            }
        } else if (memcmp(ck, "data", 4) == 0) {
            data_bytes = sz;
            break;                          /* 数据起始位置就在这儿 */
        } else {
            /* 其它 chunk（LIST/fact…）整块跳过；奇数长度要补一个填充字节 */
            rt_uint32_t skip = sz + (sz & 1u);
            char dummy;
            while (skip--) {
                if (read(fd, &dummy, 1) != 1) {
                    break;
                }
            }
        }
    }

    if (!got_fmt || data_bytes == 0) {
        rt_kprintf("[wav] %s 里没找到 fmt/data 块（got_fmt=%d data=%u）\n",
                   path, got_fmt, (unsigned)data_bytes);
        close(fd);
        return;
    }

    rt_kprintf("[wav] %s\n", path);
    wav_dump(&h, data_bytes);

    if (h.audio_format != 1) {
        rt_kprintf("[wav] 只支持未压缩 PCM（fmt=1），这个是 %u —— 先转成 PCM 再拷进来\n",
                   (unsigned)h.audio_format);
        close(fd);
        return;
    }
    if (info_only) {
        close(fd);
        return;
    }

    /* ---- 打开音频设备并配置成文件里的格式 ---- */
    rt_device_t snd = rt_device_find("sound0");
    if (snd == RT_NULL) {
        rt_kprintf("[wav] 没有 sound0（bsp/drv_audio_pwm.c 没编进来？）\n");
        close(fd);
        return;
    }
    if (rt_device_open(snd, RT_DEVICE_OFLAG_WRONLY) != RT_EOK) {
        rt_kprintf("[wav] 打开 sound0 失败\n");
        close(fd);
        return;
    }

    struct rt_audio_caps caps;
    rt_memset(&caps, 0, sizeof(caps));
    caps.main_type = AUDIO_TYPE_OUTPUT;
    caps.sub_type  = AUDIO_DSP_PARAM;
    caps.udata.config.samplerate = h.samplerate;
    caps.udata.config.channels   = h.channels;
    caps.udata.config.samplebits = h.bits;
    if (rt_device_control(snd, AUDIO_CTL_CONFIGURE, &caps) != RT_EOK) {
        rt_kprintf("[wav] sound0 不接受这个格式（%u Hz / %u ch / %u bit）\n",
                   (unsigned)h.samplerate, (unsigned)h.channels, (unsigned)h.bits);
        rt_device_close(snd);
        close(fd);
        return;
    }

    /* ---- 播放循环 ---- */
    static rt_uint8_t buf[WAV_BUF_BYTES];
    rt_uint32_t left = data_bytes;
    rt_uint32_t limit = (max_sec > 0) ? ((rt_uint32_t)max_sec * h.byterate) : 0xFFFFFFFFu;
    rt_uint32_t played = 0;
    rt_tick_t  t0 = rt_tick_get();

    if (left > limit) {
        left = limit;
    }
    while (left > 0u) {
        rt_size_t want = (left > sizeof(buf)) ? sizeof(buf) : left;
        int n = read(fd, buf, want);
        if (n <= 0) {
            break;
        }
        if (vol_shift > 0 && h.bits == 8 && vol_shift < 8) {
            for (int k = 0; k < n; k++) {
                buf[k] = (rt_uint8_t)(128 + ((int)buf[k] - 128 >> vol_shift));
            }
        }
        rt_device_write(snd, 0, buf, (rt_size_t)n);
        left -= (rt_uint32_t)n;
        played += (rt_uint32_t)n;
    }

    /* 🚨 别急着 close —— 框架的 replay 池能吞下十几 KB（这里 4x4KB），
     *    而 `rt_device_close()` 会把**还没播的**整块丢掉（_audio_flush_replay_frame）。
     *    所以写入返回 ≠ 放完：要按"音频时长"等够再关（16kHz/8bit 就是 16kB/s）。*/
    {
        rt_uint32_t expect_ms = (rt_uint32_t)((rt_uint64_t)played * 1000u /
                                              (h.byterate ? h.byterate : 1u));
        while ((rt_uint32_t)(rt_tick_get() - t0) < expect_ms) {
            rt_thread_mdelay(10);
        }
    }

    rt_device_close(snd);               /* close 会等最后一个块放完 */
    rt_tick_t ms = rt_tick_get() - t0;
    rt_kprintf("[wav] 放完 %u 字节 / %u ms（%u kB/s 有效，含起停）\n",
               (unsigned)played, (unsigned)ms,
               (unsigned)(ms ? ((rt_uint64_t)played * 1000u / ms / 1024u) : 0u));
    close(fd);
}
MSH_CMD_EXPORT(wav_play, play a PCM wav file via sound0: wav_play [-i] [-v n] [-l sec] <file>);
