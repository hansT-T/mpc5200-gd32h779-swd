#include <linux/delay.h>
#include <linux/mtd/compatmac.h>

#include "swd_gpio.h"
#include "swd_transport.h"

#define SWD_HALF_PERIOD_NS    100U
#define SWD_WAIT_ATTEMPTS     3U
#define SWD_IDLE_CLOCKS       8U

static void swd_clock_cycle(void)
{
    swd_gpio_clock(0);
    ndelay(SWD_HALF_PERIOD_NS);
    swd_gpio_clock(1);
    ndelay(SWD_HALF_PERIOD_NS);
}

static void swd_write_bit(u32 bit)
{
    swd_gpio_data(bit & 1U);
    swd_clock_cycle();
}

static u32 swd_read_bit(void)
{
    u32 bit;
    /* 沿用此板已验证的采样位置：拉低时钟，延时后采样，再拉高。 */
    swd_gpio_clock(0);
    ndelay(SWD_HALF_PERIOD_NS);
    bit = swd_gpio_sample();
    swd_gpio_clock(1);
    ndelay(SWD_HALF_PERIOD_NS);
    return bit;
}

static void swd_clock_cycles(unsigned int count)
{
    while (count--)
        swd_clock_cycle();
}

/* 请求、数据和激活序列都按最低位先发送。 */
static void swd_write_bits(u32 value, unsigned int count)
{
    while (count--) {
        swd_write_bit(value & 1U);
        value >>= 1;
    }
}

static void swd_idle_cycles(void)
{
    swd_write_bits(0, SWD_IDLE_CLOCKS);
}

static u8 swd_transfer_once(u32 request, u32 *data)
{
    u32 ack, bit, value, parity;
    unsigned int i;

    /* 没有有效写入数据时不发送请求，避免目标接受一个不完整写事务。 */
    if (!(request & SWD_REG_R) && !data)
        return DAP_TRANSFER_ERROR;

    swd_write_bit(1);
    parity = 0;
    for (i = 0; i < 4; i++) {
        bit = (request >> i) & 1U;
        swd_write_bit(bit);
        parity ^= bit;
    }
    swd_write_bit(parity);
    swd_write_bit(0);
    swd_write_bit(1);

    /* 请求头后让出 SWDIO，一个 turnaround 后接收三位 ACK。 */
    swd_gpio_direction(0);
    swd_clock_cycle();
    ack = 0;
    for (i = 0; i < 3; i++)
        ack |= swd_read_bit() << i;

    if (ack == DAP_TRANSFER_OK) {
        parity = 0;
        if (request & SWD_REG_R) {
            value = 0;
            for (i = 0; i < 32; i++) {
                bit = swd_read_bit();
                value |= bit << i;
                parity ^= bit;
            }
            if (swd_read_bit() != parity)
                ack = DAP_TRANSFER_ERROR;
            else if (data)
                *data = value;
            swd_clock_cycle();
            swd_gpio_direction(1);
        } else {
            swd_clock_cycle();
            swd_gpio_direction(1);
            value = *data;
            for (i = 0; i < 32; i++) {
                bit = value & 1U;
                swd_write_bit(bit);
                parity ^= bit;
                value >>= 1;
            }
            swd_write_bit(parity);
        }
        swd_idle_cycles();
        swd_gpio_data(1);
        return ack;
    }

    /* 连接时关闭 ORUNDETECT，WAIT/FAULT 没有 32 位数据阶段。 */
    if (ack == DAP_TRANSFER_WAIT || ack == DAP_TRANSFER_FAULT)
        swd_clock_cycle();
    else
        swd_clock_cycles(34); /* 无效 ACK：保留原回退时钟后收回总线。 */
    swd_gpio_direction(1);
    swd_gpio_data(1);
    swd_idle_cycles();
    return ack;
}

u8 swd_transfer(u32 request, u32 *data)
{
    unsigned int attempt;
    u8 ack = DAP_TRANSFER_WAIT;

    for (attempt = 0; attempt < SWD_WAIT_ATTEMPTS; attempt++) {
        ack = swd_transfer_once(request, data);
        if (ack != DAP_TRANSFER_WAIT)
            break;
    }
    return ack;
}

u8 swd_dp_read(u8 reg, u32 *value)
{
    return swd_transfer(SWD_REG_DP | SWD_REG_R | SWD_REG_ADR(reg), value)
           == DAP_TRANSFER_OK ? 0 : 1;
}

u8 swd_dp_write(u8 reg, u32 value)
{
    return swd_transfer(SWD_REG_DP | SWD_REG_W | SWD_REG_ADR(reg), &value)
           == DAP_TRANSFER_OK ? 0 : 1;
}

static void swd_jtag_to_swd(void)
{
    swd_gpio_data(1);
    swd_clock_cycles(56);
    swd_write_bits(0xe79eU, 16);
    swd_clock_cycles(56);
    swd_gpio_data(0);
    swd_clock_cycles(16);
}

static void swd_dormant_activate(void)
{
    static const u8 alert[16] = {
        0x92, 0xf3, 0x09, 0x62, 0x95, 0x2d, 0x85, 0x86,
        0xe9, 0xaf, 0xdd, 0xe3, 0xa2, 0x0e, 0xbc, 0x19
    };
    unsigned int i;

    swd_gpio_direction(1);
    swd_gpio_data(1);
    swd_clock_cycles(64);
    swd_write_bits(0xe3bcU, 16);
    swd_gpio_data(1);
    swd_clock_cycles(8);
    for (i = 0; i < sizeof(alert); i++)
        swd_write_bits(alert[i], 8);
    swd_gpio_data(0);
    swd_clock_cycles(4);
    swd_write_bits(0x1aU, 8);
    swd_gpio_data(1);
    swd_clock_cycles(64);
    swd_gpio_data(0);
    swd_clock_cycles(8);
}

void swd_wire_connect(void)
{
    swd_gpio_direction(1);
    swd_gpio_clock(0);
    swd_jtag_to_swd();
    swd_dormant_activate();
}
