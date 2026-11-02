#ifndef H779_SWD_GPIO_H
#define H779_SWD_GPIO_H

#include <linux/types.h>
#include <asm/mpc5xxx.h>

#ifndef MPC5xxx_MBAR
#define MPC5xxx_MBAR 0xf0000000UL
#endif

/* MPC5200 的位号从最高位开始；保留原板卡的直接映射及本机字节序。 */
#define SWD_GPIO_BIT(n) (1U << (31U - (n)))

static __inline__ volatile u32 *swd_gpio_reg(unsigned long offset)
{
    return (volatile u32 *)(MPC5xxx_MBAR + offset);
}

static __inline__ void swd_gpio_update(unsigned long offset, u32 clear, u32 set)
{
    volatile u32 *reg = swd_gpio_reg(offset);
    *reg = (*reg & ~clear) | set;
    /* 防止编译器或 PowerPC 把相邻 GPIO 读写重排。 */
    __asm__ __volatile__("eieio" : : : "memory");
}

/* PSC3_0 为 SWCLK，PSC3_5 为 SWDIO；PSC3_2 的 NRST 仅初始化为高。 */
static __inline__ void swd_gpio_clock(int high)
{
    swd_gpio_update(0x0b10, SWD_GPIO_BIT(23),
                    high ? SWD_GPIO_BIT(23) : 0);
}

static __inline__ void swd_gpio_data(int high)
{
    swd_gpio_update(0x0b2c, SWD_GPIO_BIT(6),
                    high ? SWD_GPIO_BIT(6) : 0);
}

static __inline__ void swd_gpio_direction(int output)
{
    swd_gpio_update(0x0b28, SWD_GPIO_BIT(6),
                    output ? SWD_GPIO_BIT(6) : 0);
}

static __inline__ u32 swd_gpio_sample(void)
{
    u32 value = *swd_gpio_reg(0x0b3c);
    __asm__ __volatile__("eieio" : : : "memory");
    return (value & SWD_GPIO_BIT(14)) ? 1U : 0U;
}

int swd_gpio_init(void);
void swd_gpio_exit(void);

#endif
