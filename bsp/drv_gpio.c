/*===========================================================================
 * drv_gpio.c -- GPIO 驱动（挂 RT-Thread 的 PIN 设备框架）
 *
 * 接上框架之后就能用标准 API：
 *      rt_pin_mode(pin, PIN_MODE_OUTPUT);
 *      rt_pin_write(pin, PIN_HIGH);
 *      rt_pin_read(pin);
 * 而且 msh 里直接就有 `pin` 命令（components/drivers/pin/dev_pin.c 自带）。
 *
 * 寄存器来源（全部核对过 IDF 6.1 的 soc/esp32s31）：
 *   GPIO 基址 0x20583000、IO_MUX 基址 0x20582000
 *   gpio_reg.h：OUT_W1TS +0x08 / OUT_W1TC +0x0c / OUT1_W1TS +0x14 / OUT1_W1TC +0x18
 *               ENABLE_W1TS +0x38 / ENABLE_W1TC +0x3c / ENABLE1_W1TS +0x44 / ENABLE1_W1TC +0x48
 *               IN_REG +0x64 / IN1_REG +0x68
 *               PIN<n>_REG = +0xF4 + 4n（PAD_DRIVER=bit2，1=开漏）
 *   io_mux_reg.h：MCU_SEL=[14:12]（PIN_FUNC_GPIO=1）、FUN_IE=bit9、FUN_PU=bit8、
 *                 FUN_PD=bit7、FUN_DRV=[11:10]
 *   🚨 GPIO32~63 走第二组寄存器（OUT1/ENABLE1/IN1），掩码要右移 32。
 *   🚨 IO_MUX 寄存器**不按引脚号连续排列**（GPIO43 在 +0xac、GPIO61 在 +0xf4），
 *      所以用 tools/gen_iomux_table.ps1 从 IDF 头文件生成查表（bsp/s31_iomux_table.h）。
 *===========================================================================*/

#include <rtthread.h>
#include <rthw.h>
#include <rtdevice.h>
#include "s31_regs.h"
#include "s31_iomux_table.h"

#define S31_GPIO_BASE      0x20583000u
#define S31_IO_MUX_BASE    0x20582000u

#define GPIO_OUT_W1TS      (S31_GPIO_BASE + 0x08u)
#define GPIO_OUT_W1TC      (S31_GPIO_BASE + 0x0cu)
#define GPIO_OUT1_W1TS     (S31_GPIO_BASE + 0x14u)
#define GPIO_OUT1_W1TC     (S31_GPIO_BASE + 0x18u)
#define GPIO_ENABLE_W1TS   (S31_GPIO_BASE + 0x38u)
#define GPIO_ENABLE_W1TC   (S31_GPIO_BASE + 0x3cu)
#define GPIO_ENABLE1_W1TS  (S31_GPIO_BASE + 0x44u)
#define GPIO_ENABLE1_W1TC  (S31_GPIO_BASE + 0x48u)
#define GPIO_IN_REG        (S31_GPIO_BASE + 0x64u)
#define GPIO_IN1_REG       (S31_GPIO_BASE + 0x68u)
#define GPIO_PIN_REG(n)    (S31_GPIO_BASE + 0xF4u + 4u * (n))
#define GPIO_PIN_OD        (1u << 2)          /* PAD_DRIVER：1 = 开漏 */

/* IO_MUX 位域（io_mux_reg.h） */
#define IOMUX_MCU_SEL_S    12
#define IOMUX_FUN_DRV_S    10
#define IOMUX_FUN_IE       (1u << 9)
#define IOMUX_FUN_PU       (1u << 8)
#define IOMUX_FUN_PD       (1u << 7)
#define IOMUX_PIN_FUNC_GPIO 1u                /* PIN_FUNC_GPIO */

static inline rt_uint32_t s31_pin_mask(rt_base_t pin) { return 1u << (pin & 31); }

/* 引脚 -> IO_MUX 寄存器地址；该引脚没有 IO_MUX 时返回 0 */
static inline rt_uint32_t s31_iomux_reg(rt_base_t pin)
{
    if (pin < 0 || pin >= S31_GPIO_PIN_COUNT) {
        return 0;
    }
    if (s31_iomux_off[pin] == 0xFFFFFFFFu) {
        return 0;
    }
    return S31_IO_MUX_BASE + s31_iomux_off[pin];
}

static void s31_gpio_set_out_en(rt_base_t pin, int enable)
{
    if (pin < 32) {
        S31_REG32(enable ? GPIO_ENABLE_W1TS : GPIO_ENABLE_W1TC) = s31_pin_mask(pin);
    } else {
        S31_REG32(enable ? GPIO_ENABLE1_W1TS : GPIO_ENABLE1_W1TC) = s31_pin_mask(pin);
    }
}

/* ---- PIN 设备框架的回调 ------------------------------------------------ */

static void s31_pin_mode(struct rt_device *device, rt_base_t pin, rt_uint8_t mode)
{
    rt_uint32_t mux = s31_iomux_reg(pin);
    rt_uint32_t v;

    (void)device;

    if (mux == 0) {
        return;                                  /* 该引脚没有 IO_MUX（如纯模拟脚） */
    }

    /* 先选成 GPIO 功能，再按模式配输入/上下拉/开漏 */
    v  = S31_REG32(mux);
    v &= ~((0x7u << IOMUX_MCU_SEL_S) | IOMUX_FUN_IE | IOMUX_FUN_PU | IOMUX_FUN_PD);
    v |= (IOMUX_PIN_FUNC_GPIO << IOMUX_MCU_SEL_S);

    switch (mode) {
    case PIN_MODE_OUTPUT:
        /* 输出也把输入缓冲打开（FUN_IE=1）：这样 rt_pin_read() 能读回引脚真实电平，
         * 做"写出去再读回来"的自检（IDF 关掉 IE，读回值就不可靠了）。 */
        v |= IOMUX_FUN_IE;
        S31_REG32(GPIO_PIN_REG(pin)) &= ~GPIO_PIN_OD;
        s31_gpio_set_out_en(pin, 1);
        break;

    case PIN_MODE_OUTPUT_OD:
        v |= IOMUX_FUN_IE;
        S31_REG32(GPIO_PIN_REG(pin)) |= GPIO_PIN_OD;   /* 开漏 */
        s31_gpio_set_out_en(pin, 1);
        break;

    case PIN_MODE_INPUT_PULLUP:
        v |= IOMUX_FUN_IE | IOMUX_FUN_PU;
        s31_gpio_set_out_en(pin, 0);
        break;

    case PIN_MODE_INPUT_PULLDOWN:
        v |= IOMUX_FUN_IE | IOMUX_FUN_PD;
        s31_gpio_set_out_en(pin, 0);
        break;

    case PIN_MODE_INPUT:
    default:
        v |= IOMUX_FUN_IE;
        s31_gpio_set_out_en(pin, 0);
        break;
    }

    S31_REG32(mux) = v;
}

static void s31_pin_write(struct rt_device *device, rt_base_t pin, rt_uint8_t value)
{
    (void)device;

    if (pin < 32) {
        S31_REG32(value ? GPIO_OUT_W1TS : GPIO_OUT_W1TC) = s31_pin_mask(pin);
    } else {
        S31_REG32(value ? GPIO_OUT1_W1TS : GPIO_OUT1_W1TC) = s31_pin_mask(pin);
    }
}

static rt_ssize_t s31_pin_read(struct rt_device *device, rt_base_t pin)
{
    rt_uint32_t reg = (pin < 32) ? GPIO_IN_REG : GPIO_IN1_REG;

    (void)device;

    return (S31_REG32(reg) & s31_pin_mask(pin)) ? PIN_HIGH : PIN_LOW;
}

/* 引脚中断暂未实现（S31 的 GPIO 中断分成 GPIO_INTR0..3 四个源，还没核对
 * 引脚->源的映射表）。框架要求这几个回调存在，先如实返回"不支持"。 */
static rt_err_t s31_pin_attach_irq(struct rt_device *device, rt_base_t pin,
                                   rt_uint8_t mode, void (*hdr)(void *args), void *args)
{
    (void)device; (void)pin; (void)mode; (void)hdr; (void)args;
    return -RT_ENOSYS;
}

static rt_err_t s31_pin_detach_irq(struct rt_device *device, rt_base_t pin)
{
    (void)device; (void)pin;
    return -RT_ENOSYS;
}

static rt_err_t s31_pin_irq_enable(struct rt_device *device, rt_base_t pin, rt_uint8_t enabled)
{
    (void)device; (void)pin; (void)enabled;
    return -RT_ENOSYS;
}

static rt_base_t s31_pin_get(const char *name)
{
    /* 支持 "P43" / "p43" 这种简写（RT-Thread 也认 "PA.0" 之类的板级命名，
     * 我们这块板直接按 GPIO 号） */
    if (name == RT_NULL) {
        return -1;
    }
    if ((name[0] == 'P' || name[0] == 'p') && name[1] >= '0' && name[1] <= '9') {
        int n = 0;
        const char *s = &name[1];
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (*s - '0');
            s++;
        }
        if (n < S31_GPIO_PIN_COUNT) {
            return n;
        }
    }
    return -1;
}

static const struct rt_pin_ops s31_pin_ops =
{
    s31_pin_mode,
    s31_pin_write,
    s31_pin_read,
    s31_pin_attach_irq,
    s31_pin_detach_irq,
    s31_pin_irq_enable,
    s31_pin_get,
    RT_NULL,            /* pin_debounce */
};

static int s31_gpio_init(void)
{
    rt_device_pin_register("pin", &s31_pin_ops, RT_NULL);
    return 0;
}

/* 🚨 必须用 INIT_DEVICE_EXPORT（3 级）**不能**用 INIT_BOARD_EXPORT（1 级）：
 *    本 port 没有在 rt_hw_board_init() 里调 rt_components_board_init()，
 *    1 级的初始化函数永远不会被执行 —— 症状是 `pin` 命令一敲就 load access fault
 *    （设备根本没注册，NULL->ops 取成员）。详见 AGENTS.md §4.18。 */
INIT_DEVICE_EXPORT(s31_gpio_init);
