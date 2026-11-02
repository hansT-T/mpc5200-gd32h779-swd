#include "swd_gpio.h"

int swd_gpio_init(void)
{
    volatile struct mpc5xxx_gpio *gpio;
    u32 psc3_mask;

    gpio = (volatile struct mpc5xxx_gpio *)MPC5xxx_GPIO;
    psc3_mask = SWD_GPIO_BIT(20) | SWD_GPIO_BIT(21) |
                SWD_GPIO_BIT(22) | SWD_GPIO_BIT(23);
    gpio->port_config &= ~psc3_mask;
    __asm__ __volatile__("eieio" : : : "memory");

    /* SWDIO：GPIO、普通 CMOS、输出，禁止中断，初始低电平。 */
    swd_gpio_update(0x0b20, 0, SWD_GPIO_BIT(6));
    swd_gpio_update(0x0b24, SWD_GPIO_BIT(6), 0);
    swd_gpio_update(0x0b28, 0, SWD_GPIO_BIT(6));
    swd_gpio_update(0x0b30, SWD_GPIO_BIT(6), 0);
    swd_gpio_data(0);

    /* 保留原板卡 NRST 高、SWCLK 低的初始状态，不产生复位脉冲。 */
    swd_gpio_update(0x0b00, psc3_mask, 0);
    swd_gpio_update(0x0b04, 0, SWD_GPIO_BIT(21));
    swd_gpio_update(0x0b08, SWD_GPIO_BIT(21), 0);
    swd_gpio_update(0x0b0c, 0, SWD_GPIO_BIT(21));
    swd_gpio_update(0x0b10, 0, SWD_GPIO_BIT(21));
    swd_gpio_update(0x0b04, 0, SWD_GPIO_BIT(23));
    swd_gpio_update(0x0b08, SWD_GPIO_BIT(23), 0);
    swd_gpio_update(0x0b0c, 0, SWD_GPIO_BIT(23));
    swd_gpio_clock(0);
    return 0;
}

void swd_gpio_exit(void)
{
    /* 不主动改线或复位；卸载驱动不能干扰已经运行的目标程序。 */
}
