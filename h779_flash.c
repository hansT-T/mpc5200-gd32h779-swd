#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <asm/uaccess.h>

#include "swd_transport.h"
#include "h779_flash.h"
#include "h779_regs.h"
#include "swd_h779_ioctl.h"

/* H779 烧录逻辑；HEX 解析和镜像下载计划由用户态负责。 */
static struct file *h779_owner;
static struct {
    struct swd_h779_info info;
    struct swd_h779_status status;
    u32 scratch, timeout_ms, csw32;
    u32 original_perf, cnvm_prepared; /* bit n 表示本会话已完成第 n 个 CNVM 扇区的擦除准备。 */
    int linked, ready, finished, dma_owned, nvm_owned, perf_changed;
} h779;

#define H779_ERR(fmt, args...) \
    printk(KERN_ERR "h779: %s: " fmt, __FUNCTION__, ##args)
#define H779_TRY(call) do { \
    int hret = (call); \
    if (hret) { \
        H779_ERR("%s failed=%d\n", #call, hret); \
        return hret; \
    } \
} while (0)

static void h779_pause(void)
{
    set_current_state(TASK_UNINTERRUPTIBLE);
    schedule_timeout(1);
}

static unsigned long h779_deadline(void)
{
    return jiffies + (h779.timeout_ms * HZ + 999U) / 1000U;
}

static int h779_dp_ok(void)
{
    u32 value;
    if (swd_dp_read(DP_CTRL_STAT, &value)) {
        H779_ERR("cannot read DP CTRL/STAT\n");
        return -EIO;
    }
    if (value & (STICKYERR | WDATAERR | STICKYORUN | STICKYCMP)) {
        H779_ERR("sticky DP error CTRL/STAT=0x%08x\n", value);
        return -EIO;
    }
    return 0;
}

/* AP 读取存在流水线：实际结果必须从 DP.RDBUFF 取出。 */
static int h779_ap_read(u32 reg, u32 *value)
{
    u32 dummy, actual, ack;
    if (swd_dp_write(DP_SELECT, reg & APBANKSEL)) {
        H779_ERR("DP SELECT failed for AP0 register=0x%02x\n", reg);
        return -EIO;
    }
    ack = swd_transfer(SWD_REG_AP | SWD_REG_R | SWD_REG_ADR(reg), &dummy);
    if (ack != DAP_TRANSFER_OK) {
        H779_ERR("AP0 read register=0x%02x ACK=0x%x\n", reg, ack);
        return -EIO;
    }
    if (swd_dp_read(DP_RDBUFF, &actual)) {
        H779_ERR("DP RDBUFF failed after AP0 read register=0x%02x\n", reg);
        return -EIO;
    }
    H779_TRY(h779_dp_ok());
    *value = actual;
    return 0;
}

/* 排空 AP 写请求后再检查粘滞错误；不重放可能已执行的写入。 */
static int h779_ap_write(u32 reg, u32 value)
{
    u32 dummy, ack;
    if (swd_dp_write(DP_SELECT, reg & APBANKSEL)) {
        H779_ERR("DP SELECT failed for AP0 register=0x%02x\n", reg);
        return -EIO;
    }
    ack = swd_transfer(SWD_REG_AP | SWD_REG_W | SWD_REG_ADR(reg), &value);
    if (ack != DAP_TRANSFER_OK) {
        H779_ERR("AP0 write register=0x%02x ACK=0x%x\n", reg, ack);
        return -EIO;
    }
    if (swd_dp_read(DP_RDBUFF, &dummy)) {
        H779_ERR("DP RDBUFF failed after AP0 write register=0x%02x\n", reg);
        return -EIO;
    }
    return h779_dp_ok();
}

static int h779_read32(u32 address, u32 *value)
{
    int ret;
    if (address & 3U) {
        H779_ERR("unaligned read32 address=0x%08x\n", address);
        return -EINVAL;
    }
    ret = h779_ap_write(AP_TAR, address);
    if (!ret)
        ret = h779_ap_read(AP_DRW, value);
    if (ret)
        H779_ERR("read32 address=0x%08x failed=%d\n", address, ret);
    return ret;
}

static int h779_write32(u32 address, u32 value)
{
    int ret;
    if (address & 3U) {
        H779_ERR("unaligned write32 address=0x%08x\n", address);
        return -EINVAL;
    }
    ret = h779_ap_write(AP_TAR, address);
    if (!ret)
        ret = h779_ap_write(AP_DRW, value);
    if (ret)
        H779_ERR("write32 address=0x%08x failed=%d\n", address, ret);
    return ret;
}

/* 只用于稳定的可读写寄存器，不能用于 START、SWREQ 或写 1 清零位。 */
static int h779_write_checked(u32 address, u32 value)
{
    u32 actual;
    H779_TRY(h779_write32(address, value));
    H779_TRY(h779_read32(address, &actual));
    if (actual != value) {
        H779_ERR("register readback mismatch address=0x%08x expected=0x%08x actual=0x%08x\n",
                  address, value, actual);
        return -EIO;
    }
    return 0;
}

static int h779_set_bits(u32 address, u32 bits)
{
    u32 value;
    H779_TRY(h779_read32(address, &value));
    return h779_write_checked(address, value | bits);
}

static int h779_set_csw(u32 value)
{
    u32 actual;
    u32 mask = CSW_SIZE | CSW_ADDRINC | H779_CSW_PRIVILEGED |
               H779_CSW_BUFFERABLE | H779_CSW_CACHEABLE;
    H779_TRY(h779_ap_write(AP_CSW, value));
    H779_TRY(h779_ap_read(AP_CSW, &actual));
    if ((actual & mask) != (value & mask) || !(actual & CSW_DBGSTAT)) {
        H779_ERR("AP CSW mismatch requested=0x%08x actual=0x%08x mask=0x%08x DeviceEn=%u\n",
                  value, actual, mask, !!(actual & CSW_DBGSTAT));
        return -EIO;
    }
    return 0;
}

static int h779_read16(u32 address, u32 *value)
{
    u32 data;
    int ret, restore;
    ret = h779_set_csw((h779.csw32 & ~CSW_SIZE) | CSW_SIZE16);
    if (!ret)
        ret = h779_ap_write(AP_TAR, address);
    if (!ret)
        ret = h779_ap_read(AP_DRW, &data);
    restore = h779_set_csw(h779.csw32);
    if (ret || restore) {
        H779_ERR("read16 address=0x%08x failed=%d CSW_restore=%d\n",
                  address, ret, restore);
        return ret ? ret : restore;
    }
    *value = (data >> ((address & 2U) * 8U)) & 0xFFFFU;
    return 0;
}

static int h779_link(void)
{
    unsigned long deadline;
    u32 value, ack;
    swd_wire_connect();
    ack = swd_transfer(SWD_REG_DP | SWD_REG_R | SWD_REG_ADR(DP_IDCODE),
                       &h779.info.dp_idr);
    if (ack != DAP_TRANSFER_OK) {
        H779_ERR("DPIDR read failed ACK=0x%x\n", ack);
        return -EIO;
    }
    if (swd_dp_write(DP_ABORT, STKCMPCLR | STKERRCLR | WDERRCLR | ORUNERRCLR)) {
        H779_ERR("DP ABORT error-clear write failed\n");
        return -EIO;
    }
    if (swd_dp_write(DP_SELECT, 0U)) {
        H779_ERR("DP SELECT AP0 write failed\n");
        return -EIO;
    }
    if (swd_dp_write(DP_CTRL_STAT, CDBGPWRUPREQ | CSYSPWRUPREQ)) {
        H779_ERR("DP power-up request write failed\n");
        return -EIO;
    }
    deadline = h779_deadline();
    for (;;) {
        if (swd_dp_read(DP_CTRL_STAT, &value)) {
            H779_ERR("cannot read DP power-up acknowledgement\n");
            return -EIO;
        }
        if ((value & (CDBGPWRUPACK | CSYSPWRUPACK)) ==
            (CDBGPWRUPACK | CSYSPWRUPACK))
            break;
        if (time_after_eq(jiffies, deadline)) {
            H779_ERR("DP power-up timeout CTRL/STAT=0x%08x\n", value);
            return -ETIMEDOUT;
        }
        h779_pause();
    }
    H779_TRY(h779_ap_read(AP_IDR, &h779.info.ap_idr));
    if (((h779.info.ap_idr & H779_AP_IDR_CLASS_MASK) >>
         H779_AP_IDR_CLASS_SHIFT) != H779_AP_IDR_CLASS_MEM_AP ||
        (h779.info.ap_idr & H779_AP_IDR_TYPE_MASK) != H779_AP_IDR_TYPE_AHB3) {
        H779_ERR("unsupported AP0 IDR=0x%08x CLASS=%u TYPE=%u (expected CLASS=%u TYPE=%u)\n",
                  h779.info.ap_idr,
                  (h779.info.ap_idr & H779_AP_IDR_CLASS_MASK) >> H779_AP_IDR_CLASS_SHIFT,
                  h779.info.ap_idr & H779_AP_IDR_TYPE_MASK,
                  H779_AP_IDR_CLASS_MEM_AP, H779_AP_IDR_TYPE_AHB3);
        return -ENODEV;
    }
    H779_TRY(h779_ap_read(AP_CSW, &value));
    h779.csw32 = (value & ~(CSW_SIZE | CSW_ADDRINC |
                            H779_CSW_BUFFERABLE | H779_CSW_CACHEABLE)) |
                  CSW_SIZE32 | H779_CSW_PRIVILEGED;
    H779_TRY(h779_set_csw(h779.csw32));
    /* 暂停期间冻结两个看门狗，不在连接阶段复位目标或修改选项字节。 */
    H779_TRY(h779_set_bits(H779_DBG_CTL1, 1U << 6));
    H779_TRY(h779_set_bits(H779_DBG_CTL4, 1U << 18));
    H779_TRY(h779_write32(DHCSR_ADDRESS, H779_DHCSR_DBGKEY |
                          H779_DHCSR_C_DEBUGEN | H779_DHCSR_C_HALT));
    deadline = h779_deadline();
    for (;;) {
        H779_TRY(h779_read32(DHCSR_ADDRESS, &value));
        if (value & H779_DHCSR_S_HALT)
            break;
        if (time_after_eq(jiffies, deadline)) {
            H779_ERR("core halt timeout DHCSR=0x%08x\n", value);
            return -ETIMEDOUT;
        }
        h779_pause();
    }
    h779.linked = 1;
    return 0;
}

static int h779_status(void)
{
    struct swd_h779_status *s = &h779.status;
    H779_TRY(h779_read32(H779_CSTAT, &s->cnvm_stat));
    H779_TRY(h779_read32(H779_ESTAT, &s->ecnvm_stat));
    H779_TRY(h779_read32(H779_CCTL, &s->cnvm_ctl));
    H779_TRY(h779_read32(H779_ECTL, &s->ecnvm_ctl));
    H779_TRY(h779_read32(H779_DMA_STAT, &s->mdma_stat));
    H779_TRY(h779_read32(H779_RAM_ECC_STAT, &s->ram_ecc));
    s->dirty_cnvm = 0U; /* ABI 兼容保留字段，不再跟踪扇区是否写满。 */
    s->ready = h779.ready;
    return 0;
}

static int h779_idle(void)
{
    H779_TRY(h779_status());
    if ((h779.status.cnvm_stat & H779_CBUSY) ||
        (h779.status.ecnvm_stat & H779_EBUSY)) {
        H779_ERR("NVM busy CNVM_STAT=0x%08x ECNVM_STAT=0x%08x\n",
                  h779.status.cnvm_stat, h779.status.ecnvm_stat);
        return -EBUSY;
    }
    return 0;
}

static int h779_no_errors(void)
{
    if ((h779.status.cnvm_stat & H779_CERRORS) ||
        (h779.status.ecnvm_stat & H779_EERRORS) ||
        (h779.status.ram_ecc & 7U)) {
        H779_ERR("NVM/RAM error CNVM_STAT=0x%08x ECNVM_STAT=0x%08x RAM_ECC=0x%08x CERR=0x%08x EERR=0x%08x\n",
                  h779.status.cnvm_stat, h779.status.ecnvm_stat,
                  h779.status.ram_ecc,
                  h779.status.cnvm_stat & H779_CERRORS,
                  h779.status.ecnvm_stat & H779_EERRORS);
        return -EIO;
    }
    return 0;
}

static int h779_wait_end(int cnvm)
{
    unsigned long deadline = h779_deadline();
    u32 selected;
    for (;;) {
        H779_TRY(h779_status());
        if (!(h779.status.cnvm_stat & H779_CBUSY) &&
            !(h779.status.ecnvm_stat & H779_EBUSY)) {
            H779_TRY(h779_no_errors());
            selected = cnvm ? h779.status.cnvm_stat : h779.status.ecnvm_stat;
            if (selected & H779_END)
                return 0;
        }
        if (time_after_eq(jiffies, deadline)) {
            H779_ERR("%s completion timeout CNVM_STAT=0x%08x ECNVM_STAT=0x%08x CNVM_CTL=0x%08x ECNVM_CTL=0x%08x\n",
                      cnvm ? "CNVM" : "ECNVM",
                      h779.status.cnvm_stat, h779.status.ecnvm_stat,
                      h779.status.cnvm_ctl, h779.status.ecnvm_ctl);
            return -ETIMEDOUT;
        }
        h779_pause();
    }
}

/* 只有本驱动的 MDMA 已停止且两个 NVMC 均空闲，才允许清 PG 或恢复 PERF。 */
static int h779_cleanup(void)
{
    unsigned long deadline;
    u32 ctl, stat, perf;
    if (h779.dma_owned) {
        H779_TRY(h779_write32(H779_DMA_CTL, 0U));
        deadline = h779_deadline();
        for (;;) {
            H779_TRY(h779_read32(H779_DMA_CTL, &ctl));
            H779_TRY(h779_read32(H779_DMA_STAT, &stat));
            if (!(ctl & 1U) && !(stat & (1U << 16)))
                break;
            if (time_after_eq(jiffies, deadline)) {
                H779_ERR("MDMA stop timeout CTL=0x%08x STAT=0x%08x\n", ctl, stat);
                return -ETIMEDOUT;
            }
            h779_pause();
        }
        h779.dma_owned = 0;
    }
    if (h779.nvm_owned || h779.perf_changed) {
        H779_TRY(h779_idle());
        H779_TRY(h779_write_checked(H779_CCTL, H779_LK));
        H779_TRY(h779_write_checked(H779_ECTL, H779_LK | H779_PGCHEN));
        h779.nvm_owned = 0;
        if (h779.perf_changed) {
            H779_TRY(h779_read32(H779_PERF, &perf));
            perf = (perf & ~(H779_PGSZ_MASK | H779_PREFETCH_MASK | H779_EMPTY_CBUF)) |
                   (h779.original_perf & (H779_PGSZ_MASK | H779_PREFETCH_MASK));
            H779_TRY(h779_write_checked(H779_PERF, perf));
            h779.perf_changed = 0;
        }
    }
    return 0;
}

static int h779_flush_cnvm(void)
{
    u32 perf;
    H779_TRY(h779_idle());
    H779_TRY(h779_read32(H779_PERF, &perf));
    H779_TRY(h779_write32(H779_PERF, perf | H779_EMPTY_CBUF));
    /* 该位是只写触发位，不能靠回读判断完成；触发后留出空闲时间。 */
    h779_pause();
    return 0;
}

/* ECNVM 有四行读缓冲；校验前读取同一有效页的其它行以驱逐旧数据。 */
static int h779_evict_ecnvm(u32 address)
{
    u32 p, dummy, perf, base = address & ~(H779_PAGE - 1U);
    int ret = 0, clean, changed = 0;
    H779_TRY(h779_idle());
    H779_TRY(h779_read32(H779_PERF, &perf));
    if (perf & H779_PREFETCH_MASK) {
        if (h779.perf_changed) {
            H779_ERR("cannot change ECNVM prefetch: PERF restoration pending, address=0x%08x\n", address);
            return -EBUSY;
        }
        h779.original_perf = perf;
        h779.perf_changed = changed = 1;
        ret = h779_write_checked(H779_PERF,
                                perf & ~(H779_PREFETCH_MASK | H779_EMPTY_CBUF));
        if (ret)
            goto out;
    }
    for (p = base; p < base + H779_PAGE; p += H779_BLOCK) {
        if (p == (address & ~(H779_BLOCK - 1U)))
            continue;
        ret = h779_read32(p, &dummy);
        if (ret)
            break;
    }
out:
    if (changed) {
        clean = h779_cleanup();
        if (!ret)
            ret = clean;
    }
    return ret;
}

static int h779_geometry(void)
{
    struct swd_h779_info *i = &h779.info;
    u32 density, size, otp0, otp1, dcrp, scr;
    H779_TRY(h779_read16(H779_CSIZE, &size));
    H779_TRY(h779_read32(H779_DENSITY, &density));
    H779_TRY(h779_read32(H779_OBSTAT, &i->obstat0));
    H779_TRY(h779_read32(H779_OBREP, &i->obrepctl));
    H779_TRY(h779_read32(H779_WPC, &i->wp_cnvm));
    H779_TRY(h779_read32(H779_WPE0, &i->wp0_ecnvm));
    H779_TRY(h779_read32(H779_WPE1, &i->wp1_ecnvm));
    H779_TRY(h779_read32(H779_OTP0, &otp0));
    H779_TRY(h779_read32(H779_OTP1, &otp1));
    H779_TRY(h779_read32(H779_DCRP, &dcrp));
    H779_TRY(h779_read32(H779_SCR, &scr));
    /* 目前只支持已验证的 W 型布局，不推断其它容量型号的地址范围。 */
    if (size != H779_W_CNVM_KB ||
        (density & H779_DENSITY_ECNVM_MASK) != H779_W_ECNVM_KB ||
        ((h779.status.cnvm_stat & H779_CSTAT_NVMVER_MASK) >>
         H779_CSTAT_NVMVER_SHIFT) != H779_CSTAT_NVMVER_W) {
        H779_ERR("unsupported NVM layout: CNVM=%uKB ECNVM=%uKB NVMVER=%u; expected CNVM=%uKB ECNVM=%uKB NVMVER=%u (DENSITY=0x%08x CSTAT=0x%08x)\n",
                  size, density & H779_DENSITY_ECNVM_MASK,
                  (h779.status.cnvm_stat & H779_CSTAT_NVMVER_MASK) >> H779_CSTAT_NVMVER_SHIFT,
                  H779_W_CNVM_KB, H779_W_ECNVM_KB, H779_CSTAT_NVMVER_W,
                  density, h779.status.cnvm_stat);
        return -ENODEV;
    }
    if (((i->obstat0 & H779_OBSTAT_SPC_MASK) >> H779_OBSTAT_SPC_SHIFT) !=
            H779_OBSTAT_SPC_UNPROTECTED) {
        H779_ERR("unsupported SPC=0x%02x, expected unprotected=0x%02x (OBSTAT=0x%08x)\n",
                  (i->obstat0 & H779_OBSTAT_SPC_MASK) >> H779_OBSTAT_SPC_SHIFT,
                  H779_OBSTAT_SPC_UNPROTECTED, i->obstat0);
        return -EACCES;
    }
    if (i->obstat0 & (H779_OBSTAT_SCR | H779_OBSTAT_FW_UPDATE)) {
        H779_ERR("unsupported security/update mode SCR=%u FW_UPDATE=%u OBSTAT=0x%08x\n",
                  !!(i->obstat0 & H779_OBSTAT_SCR),
                  !!(i->obstat0 & H779_OBSTAT_FW_UPDATE), i->obstat0);
        return -EACCES;
    }
    if (dcrp & H779_DCRP_EN_MASK) {
        H779_ERR("DCRP region enabled DCRPADDR_EFT=0x%08x enable_mask=0x%08x\n",
                  dcrp, H779_DCRP_EN_MASK);
        return -EACCES;
    }
    if (scr & H779_SCR_EN_MASK) {
        H779_ERR("secure region enabled SCRADDR_EFT=0x%08x enable_mask=0x%08x\n",
                  scr, H779_SCR_EN_MASK);
        return -EACCES;
    }
    if (otp0 & H779_OTP0_UNSUPPORTED_PROTECTION) {
        H779_ERR("unsupported OTP protection OTP_USER_CTL0=0x%08x active_mask=0x%08x\n",
                  otp0, otp0 & H779_OTP0_UNSUPPORTED_PROTECTION);
        return -EACCES;
    }
    i->abi_version = SWD_H779_ABI_VERSION;
    i->cnvm_bytes = size * 1024U;
    i->ecnvm_bytes = (density & H779_DENSITY_ECNVM_MASK) * 1024U;
    i->sram_kb = density >> H779_DENSITY_SRAM_SHIFT;
    i->program_bytes = 32U;
    i->cnvm_erase_bytes = 64U * 1024U;
    i->ecnvm_page_bytes = 4U * 1024U;
    i->ecnvm_sector_bytes = 64U * 1024U;
    i->enhanced_bytes = 0;
    i->active_bank = 0; /* 仅在增强功能开启时有意义。 */
    if (((i->obrepctl & H779_OBREP_DIEEN) || (otp1 & H779_OTP1_DIE_EN)) &&
        !(otp1 & H779_OTP1_ECNVM_DIE_DISABLE)) {
        i->enhanced_bytes =
            (((i->obrepctl & H779_OBREP_DIESZ_MASK) >> H779_OBREP_DIESZ_SHIFT) +
             1U) * H779_SECTOR;
        i->active_bank = ((i->obstat0 & H779_OBSTAT_SWAP) &&
                          !(otp1 & H779_OTP1_SWAP_DISABLE))
                         ? 1U : 0U;
    }
    return 0;
}

static int h779_connect(struct swd_h779_connect *request)
{
    u32 ctl, stat;
    int n;
    if ((request->scratch_address & (H779_BLOCK - 1U)) ||
        request->scratch_address < H779_SRAM0_BEGIN ||
        request->scratch_address > H779_SRAM0_END - H779_BLOCK ||
        (request->timeout_ms && (request->timeout_ms < 100U ||
                                request->timeout_ms > 120000U))) {
        H779_ERR("invalid CONNECT parameters scratch=0x%08x (32-byte aligned SRAM0 required) timeout_ms=%u (0 or 100..120000 required)\n",
                  request->scratch_address, request->timeout_ms);
        return -EINVAL;
    }
    h779.ready = 0;
    h779.linked = 0;
    h779.finished = 0;
    h779.timeout_ms = request->timeout_ms ? request->timeout_ms :
                       SWD_H779_DEFAULT_TIMEOUT;
    h779.scratch = request->scratch_address;
    memset(&h779.info, 0, sizeof(h779.info));
    h779.cnvm_prepared = 0U;
    H779_TRY(h779_link());
    H779_TRY(h779_set_bits(H779_AHB3EN, 1U << 3));
    H779_TRY(h779_set_bits(H779_AHB2EN, 1U << 8));
    H779_TRY(h779_set_bits(H779_RAM_ECC_CTL, 1U << 5));
    /* 只恢复本驱动已经占用的资源，不接管其它使用者的资源。 */
    H779_TRY(h779_cleanup());
    for (n = 0; n < 16; n++) {
        H779_TRY(h779_read32(H779_DMA_CTL + 0x40U * n, &ctl));
        H779_TRY(h779_read32(H779_DMA_STAT + 0x40U * n, &stat));
        if ((ctl & 1U) || (stat & (1U << 16))) {
            H779_ERR("MDMA channel %d active CTL=0x%08x STAT=0x%08x\n", n, ctl, stat);
            return -EBUSY;
        }
        if (!n && (ctl & (1U << 8))) {
            H779_ERR("MDMA channel 0 secure mode not supported CTL=0x%08x\n", ctl);
            return -EACCES;
        }
    }
    H779_TRY(h779_idle());
    H779_TRY(h779_no_errors());
    H779_TRY(h779_geometry());
    if (h779.status.cnvm_ctl != H779_LK ||
        h779.status.ecnvm_ctl != (H779_LK | H779_PGCHEN)) {
        H779_ERR("unexpected NVMC controls CNVM_CTL=0x%08x ECNVM_CTL=0x%08x (expected 0x%08x/0x%08x)\n",
                  h779.status.cnvm_ctl, h779.status.ecnvm_ctl,
                  H779_LK, H779_LK | H779_PGCHEN);
        return -EBUSY;
    }
    if (!(h779.status.cnvm_stat & H779_CSTAT_CNVMECCF) ||
        !(h779.status.ecnvm_stat & H779_ESTAT_ECNVM_ECCF)) {
        H779_ERR("required ECC disabled CNVM_STAT=0x%08x ECNVM_STAT=0x%08x\n",
                  h779.status.cnvm_stat, h779.status.ecnvm_stat);
        return -EACCES;
    }
    h779.ready = 1;
    request->timeout_ms = h779.timeout_ms;
    request->info = h779.info;
    return 0;
}

static int h779_in_range(u32 address, u32 length, u32 begin, u32 end)
{
    return length && address >= begin && address < end && length <= end - address;
}

/* CNVM 返回 1，Data ECNVM 返回 0，范围无效返回负错误码。 */
static int h779_write_region(u32 address, u32 length)
{
    if (h779_in_range(address, length, H779_CNVM_BEGIN, H779_CNVM_END))
        return 1;
    if (h779_in_range(address, length, H779_DATA0_BEGIN, H779_DATA0_END) ||
        h779_in_range(address, length, H779_DATA1_BEGIN, H779_DATA1_END))
        return 0;
    H779_ERR("unsupported write range address=0x%08x length=%u\n", address, length);
    return -EINVAL;
}

static int h779_ec_writable(u32 address)
{
    u32 group = ((address - H779_CNVM_BEGIN) / H779_SECTOR - 32U) / 2U;
    u32 wp = group < 32U ? h779.info.wp0_ecnvm : h779.info.wp1_ecnvm;
    if (!(wp & (1U << (group & 31U)))) {
        H779_ERR("ECNVM write-protected address=0x%08x group=%u WP=0x%08x\n",
                  address, group, wp);
        return -EACCES;
    }
    return 0;
}

static int h779_writable(u32 address, int cnvm)
{
    u32 sector;
    if (!cnvm)
        return h779_ec_writable(address);
    sector = (address - H779_CNVM_BEGIN) / H779_SECTOR;
    if (!(h779.info.wp_cnvm & (1U << (sector / 2U)))) {
        H779_ERR("CNVM write-protected address=0x%08x sector=%u WP=0x%08x\n",
                  address, sector, h779.info.wp_cnvm);
        return -EACCES;
    }
    if (sector * H779_SECTOR < h779.info.enhanced_bytes)
        return h779_ec_writable((h779.info.active_bank ? H779_BANK1_BEGIN : H779_BANK0_BEGIN) +
                                sector * H779_SECTOR);
    return 0;
}

/* 下载前检查与 FINISH 后的 RESET 共用；此处不依赖 ready。 */
static int h779_check_idle_halted(void)
{
    u32 value;
    if (h779.dma_owned || h779.nvm_owned || h779.perf_changed) {
        H779_ERR("cleanup pending dma_owned=%d nvm_owned=%d perf_changed=%d\n",
                  h779.dma_owned, h779.nvm_owned, h779.perf_changed);
        return -EBUSY;
    }
    H779_TRY(h779_set_csw(h779.csw32));
    H779_TRY(h779_read32(DHCSR_ADDRESS, &value));
    if (!(value & H779_DHCSR_S_HALT)) {
        H779_ERR("core is not halted DHCSR=0x%08x\n", value);
        return -EBUSY;
    }
    H779_TRY(h779_idle());
    H779_TRY(h779_no_errors());
    if (h779.status.cnvm_ctl != H779_LK ||
        h779.status.ecnvm_ctl != (H779_LK | H779_PGCHEN)) {
        H779_ERR("unexpected NVMC controls CNVM_CTL=0x%08x ECNVM_CTL=0x%08x (expected 0x%08x/0x%08x)\n",
                  h779.status.cnvm_ctl, h779.status.ecnvm_ctl,
                  H779_LK, H779_LK | H779_PGCHEN);
        return -EBUSY;
    }
    H779_TRY(h779_read32(H779_DMA_CTL, &value));
    if (value & (H779_DMA_CTL_SMODEN | H779_DMA_CTL_CHEN)) {
        H779_ERR("MDMA control not idle/normal CTL=0x%08x\n", value);
        return -EBUSY;
    }
    if (h779.status.mdma_stat & H779_DMA_STAT_REQAF) {
        H779_ERR("MDMA request still active STAT=0x%08x\n", h779.status.mdma_stat);
        return -EBUSY;
    }
    return 0;
}

/* 每次擦除或编程前检查会话、硬件空闲状态及核心暂停状态。 */
static int h779_preflight(void)
{
    if (!h779.ready) {
        H779_ERR("download session is not ready; CONNECT required\n");
        return -ENOTCONN;
    }
    return h779_check_idle_halted();
}

static int h779_unlock(int cnvm)
{
    u32 key = cnvm ? H779_CKEY : H779_EKEY;
    h779.nvm_owned = 1;
    H779_TRY(h779_write32(key, 0x45670123U));
    H779_TRY(h779_write32(key, 0xCDEF89ABU));
    return h779_write_checked(cnvm ? H779_CCTL : H779_ECTL,
                              cnvm ? 0U : H779_PGCHEN);
}

static int h779_clear_end(void)
{
    H779_TRY(h779_write32(H779_CSTAT, H779_END));
    H779_TRY(h779_write32(H779_ESTAT, H779_END));
    H779_TRY(h779_idle());
    H779_TRY(h779_no_errors());
    if ((h779.status.cnvm_stat | h779.status.ecnvm_stat) & H779_END) {
        H779_ERR("old ENDF did not clear CNVM_STAT=0x%08x ECNVM_STAT=0x%08x\n",
                  h779.status.cnvm_stat, h779.status.ecnvm_stat);
        return -EIO;
    }
    return 0;
}

static int h779_erase_one(u32 address, u32 unit, int cnvm)
{
    u32 ctl = cnvm ? H779_CCTL : H779_ECTL;
    u32 mode = (cnvm ? 0U : H779_PGCHEN) |
               (unit == H779_PAGE ? H779_PER : H779_SER);
    H779_TRY(h779_unlock(cnvm));
    H779_TRY(h779_clear_end());
    H779_TRY(h779_write_checked(ctl, mode));
    H779_TRY(h779_write_checked(cnvm ? H779_CADDR : H779_EADDR, address));
    H779_TRY(h779_write32(ctl, mode | H779_START));
    H779_TRY(h779_wait_end(cnvm));
    return h779_cleanup();
}

static int h779_erase(struct swd_h779_erase *request)
{
    u32 address, end, sector;
    int cnvm, ret;
    request->completed = 0;
    cnvm = h779_write_region(request->address, request->length);
    if (cnvm < 0 ||
        (request->unit != H779_SECTOR && request->unit != H779_PAGE) ||
        (cnvm && request->unit != H779_SECTOR) ||
        (request->address & (request->unit - 1U)) ||
        (request->length & (request->unit - 1U))) {
        H779_ERR("invalid ERASE address=0x%08x length=%u unit=%u (CNVM=64KB; Data ECNVM=4KB/64KB; unit alignment required)\n",
                  request->address, request->length, request->unit);
        return -EINVAL;
    }
    end = request->address + request->length;
    /* 先检查整个请求的写保护，避免擦到一半才发现后面的单元受保护。 */
    for (address = request->address; address < end; address += request->unit)
        H779_TRY(h779_writable(address, cnvm));
    H779_TRY(h779_preflight());
    for (address = request->address; address < end; address += request->unit) {
        if (cnvm) {
            sector = (address - H779_CNVM_BEGIN) / H779_SECTOR;
            h779.cnvm_prepared &= ~(1U << sector);
        }
        ret = h779_erase_one(address, request->unit, cnvm);
        if (ret) {
            H779_ERR("%s erase address=0x%08x unit=%u completed=%u/%u failed=%d\n",
                      cnvm ? "CNVM" : "Data ECNVM", address, request->unit,
                      request->completed, request->length, ret);
            return ret;
        }
        if (cnvm) {
            if (address - H779_CNVM_BEGIN < h779.info.enhanced_bytes) {
                ret = h779_erase_one(
                      (h779.info.active_bank ? H779_BANK1_BEGIN : H779_BANK0_BEGIN) +
                      address - H779_CNVM_BEGIN, H779_SECTOR, 0);
                if (ret) {
                    H779_ERR("enhanced Bank%u erase failed for CNVM address=0x%08x completed=%u/%u failed=%d\n",
                              h779.info.active_bank, address,
                              request->completed, request->length, ret);
                    return ret;
                }
            }
            h779.cnvm_prepared |= 1U << sector;
        }
        request->completed += request->unit;
        schedule();
    }
    if (cnvm)
        return h779_flush_cnvm();
    return 0;
}

/* 主机 PPC 为大端，目标 H779 为小端；显式按字节拼装目标数值。 */
static u32 h779_get_le32(const unsigned char *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int h779_stage(const unsigned char *data)
{
    u32 value, offset;
    /* 先完整初始化暂存 ECC 单元再回读，不读取或备份原有 RAM 内容。 */
    for (offset = 0; offset < H779_BLOCK; offset += 4U)
        H779_TRY(h779_write32(h779.scratch + offset, h779_get_le32(data + offset)));
    for (offset = 0; offset < H779_BLOCK; offset += 4U) {
        H779_TRY(h779_read32(h779.scratch + offset, &value));
        if (value != h779_get_le32(data + offset)) {
            H779_ERR("scratch readback mismatch address=0x%08x expected=0x%08x actual=0x%08x\n",
                      h779.scratch + offset, h779_get_le32(data + offset), value);
            return -EIO;
        }
    }
    H779_TRY(h779_read32(H779_RAM_ECC_STAT, &value));
    if (value & 7U) {
        H779_ERR("SRAM0 ECC error scratch=0x%08x RAM_ECC=0x%08x\n", h779.scratch, value);
        return -EIO;
    }
    return 0;
}

static int h779_dma_setup(u32 address)
{
    u32 stat, offset;
    h779.dma_owned = 1;
    H779_TRY(h779_write_checked(H779_DMA_CTL, 0U));
    H779_TRY(h779_write32(H779_DMA_CLEAR, 0x1FU));
    H779_TRY(h779_read32(H779_DMA_STAT, &stat));
    if (stat & (0x1FU | (1U << 16))) {
        H779_ERR("MDMA status not clear STAT=0x%08x target=0x%08x\n", stat, address);
        return -EIO;
    }
    H779_TRY(h779_write_checked(H779_DMA_CFG, H779_DMA_CFG32));
    H779_TRY(h779_write_checked(H779_DMA_COUNT, H779_BLOCK));
    H779_TRY(h779_write_checked(H779_DMA_SRC, h779.scratch));
    H779_TRY(h779_write_checked(H779_DMA_DST, address));
    for (offset = 0x60U; offset <= 0x74U; offset += 4U) {
        if (offset != 0x6CU)
            H779_TRY(h779_write_checked(H779_MDMA_BASE + offset, 0U));
    }
    return 0;
}

static int h779_dma_run(void)
{
    unsigned long deadline = h779_deadline();
    u32 stat, ctl, count;
    H779_TRY(h779_write_checked(H779_DMA_CTL, 1U));
    H779_TRY(h779_write32(H779_DMA_CTL, (1U << 16) | 1U));
    for (;;) {
        H779_TRY(h779_read32(H779_DMA_STAT, &stat));
        if (stat & 1U) {
            H779_ERR("MDMA transfer error STAT=0x%08x\n", stat);
            return -EIO;
        }
        if (stat & 2U)
            break;
        if (time_after_eq(jiffies, deadline)) {
            H779_ERR("MDMA completion timeout STAT=0x%08x\n", stat);
            return -ETIMEDOUT;
        }
        h779_pause();
    }
    H779_TRY(h779_read32(H779_DMA_CTL, &ctl));
    H779_TRY(h779_read32(H779_DMA_COUNT, &count));
    if ((stat & 0x0AU) != 0x0AU || (stat & (1U << 16)) ||
        (ctl & 1U) || (count & 0x1FFFFU)) {
        H779_ERR("MDMA incomplete/active STAT=0x%08x CTL=0x%08x COUNT=0x%08x\n",
                  stat, ctl, count);
        return -EIO;
    }
    return 0;
}

static int h779_program_one(u32 address, const unsigned char *data, int cnvm)
{
    u32 value, offset;
    H779_TRY(h779_preflight());
    if (cnvm)
        H779_TRY(h779_flush_cnvm());
    else
        H779_TRY(h779_evict_ecnvm(address));
    for (offset = 0; offset < H779_BLOCK; offset += 4U) {
        H779_TRY(h779_read32(address + offset, &value));
        if (value != (cnvm ? 0U : 0xFFFFFFFFU)) {
            H779_ERR("%s software blank precheck failed address=0x%08x expected=0x%08x actual=0x%08x\n",
                      cnvm ? "CNVM" : "Data ECNVM", address + offset,
                      cnvm ? 0U : 0xFFFFFFFFU, value);
            return -EEXIST;
        }
    }
    H779_TRY(h779_idle());
    H779_TRY(h779_no_errors());
    H779_TRY(h779_stage(data));
    H779_TRY(h779_dma_setup(address));
    if (!cnvm) {
        H779_TRY(h779_read32(H779_PERF, &h779.original_perf));
        h779.perf_changed = 1;
        value = (h779.original_perf & ~(H779_PGSZ_MASK | H779_EMPTY_CBUF)) |
                 H779_PGSZ32;
        H779_TRY(h779_write_checked(H779_PERF, value));
    }
    H779_TRY(h779_unlock(cnvm));
    H779_TRY(h779_clear_end());
    /* 增强 CNVM 的关联 ECNVM 写入由硬件处理，只设置 CNVM.PG。 */
    H779_TRY(h779_write_checked(cnvm ? H779_CCTL : H779_ECTL,
                              H779_PG | (cnvm ? 0U : H779_PGCHEN)));
    H779_TRY(h779_dma_run());
    H779_TRY(h779_wait_end(cnvm));
    H779_TRY(h779_cleanup());
    if (cnvm)
        H779_TRY(h779_flush_cnvm());
    else
        H779_TRY(h779_evict_ecnvm(address));
    for (offset = 0; offset < H779_BLOCK; offset += 4U) {
        H779_TRY(h779_read32(address + offset, &value));
        if (value != h779_get_le32(data + offset)) {
            H779_ERR("%s program verify mismatch address=0x%08x expected=0x%08x actual=0x%08x\n",
                      cnvm ? "CNVM" : "Data ECNVM", address + offset,
                      h779_get_le32(data + offset), value);
            return -EIO;
        }
    }
    H779_TRY(h779_idle());
    return h779_no_errors();
}

static int h779_program(struct swd_h779_transfer *request,
                         const unsigned char *data)
{
    u32 address, end, sector;
    int cnvm, ret;
    request->completed = 0;
    cnvm = h779_write_region(request->address, request->length);
    if (cnvm < 0 || ((request->address | request->length) & (H779_BLOCK - 1U))) {
        H779_ERR("invalid PROGRAM address=0x%08x length=%u (supported region and 32-byte alignment required)\n",
                  request->address, request->length);
        return -EINVAL;
    }
    end = request->address + request->length;
    /* 写入前检查所有涉及扇区的擦除准备，不限制块与块之间的地址顺序。 */
    for (address = request->address; address < end; address += H779_BLOCK) {
        H779_TRY(h779_writable(address, cnvm));
        if (cnvm) {
            sector = (address - H779_CNVM_BEGIN) / H779_SECTOR;
            if (!(h779.cnvm_prepared & (1U << sector))) {
                H779_ERR("CNVM sector not prepared address=0x%08x sector=%u prepared_mask=0x%08x; erase in this connection first\n",
                          address, sector, h779.cnvm_prepared);
                return -EINVAL;
            }
        }
    }
    for (address = request->address; address < end; address += H779_BLOCK) {
        ret = h779_program_one(address, data + request->completed, cnvm);
        if (ret) {
            if (cnvm) {
                sector = (address - H779_CNVM_BEGIN) / H779_SECTOR;
                h779.cnvm_prepared &= ~(1U << sector);
            }
            h779.ready = 0;
            h779.finished = 0;
            H779_ERR("%s PROGRAM address=0x%08x completed=%u/%u failed=%d; reconnect required\n",
                      cnvm ? "CNVM" : "Data ECNVM", address,
                      request->completed, request->length, ret);
            return ret; /* 本次 PROGRAM 已失败，必须重新 CONNECT。 */
        }
        request->completed += H779_BLOCK;
        if (!(request->completed & 0x3FFU))
            schedule();
    }
    return 0;
}

static int h779_read(struct swd_h779_transfer *request, unsigned char *data)
{
    u32 done = 0, address, word, index, count;
    request->completed = 0;
    if (!h779_in_range(request->address, request->length,
                       H779_CNVM_BEGIN, H779_DATA1_END)) {
        H779_ERR("invalid READ range address=0x%08x length=%u\n",
                  request->address, request->length);
        return -EINVAL;
    }
    if (h779.dma_owned || h779.nvm_owned || h779.perf_changed) {
        H779_ERR("READ blocked by pending cleanup dma_owned=%d nvm_owned=%d perf_changed=%d\n",
                  h779.dma_owned, h779.nvm_owned, h779.perf_changed);
        return -EBUSY;
    }
    H779_TRY(h779_set_csw(h779.csw32));
    H779_TRY(h779_idle());
    H779_TRY(h779_flush_cnvm());
    while (done < request->length) {
        address = request->address + done;
        if (address >= H779_CNVM_END &&
            (!done || !(address & (H779_PAGE - 1U))))
            H779_TRY(h779_evict_ecnvm(address));
        H779_TRY(h779_read32(address & ~3U, &word));
        index = address & 3U;
        count = 4U - index;
        if (count > request->length - done)
            count = request->length - done;
        while (count--) {
            data[done++] = word >> (index * 8U);
            index++;
        }
        if (!(done & 0x3FFU))
            schedule();
    }
    H779_TRY(h779_idle());
    H779_TRY(h779_no_errors());
    request->completed = done;
    return 0;
}

static int h779_finish(void)
{
    if (!h779.ready) {
        H779_ERR("FINISH requires a ready download session\n");
        return -ENOTCONN;
    }
    H779_TRY(h779_preflight());
    H779_TRY(h779_cleanup());
    H779_TRY(h779_flush_cnvm());
    h779.ready = 0;
    h779.finished = 1;
    return 0;
}

/* 捕获复位后的入口，再释放暂停；不会直接继续执行复位前的 PC。 */
static int h779_reset(void)
{
    unsigned long deadline;
    u32 value, demcr, aircr;
    u32 halt = H779_DHCSR_C_DEBUGEN | H779_DHCSR_C_HALT;
    int saw_reset = 0;

    if (!h779.finished) {
        H779_ERR("RESET requires successful FINISH in this connection\n");
        return -EINVAL; /* 本会话必须已经成功 FINISH。 */
    }
    H779_TRY(h779_check_idle_halted());

    /* 按 Armv7-M 要求，保持 C_HALT 为 1 时先清除 STEP 和 MASKINTS。 */
    H779_TRY(h779_write32(DHCSR_ADDRESS, H779_DHCSR_DBGKEY | halt));
    H779_TRY(h779_read32(DEMCR_ADDRESS, &demcr));
    H779_TRY(h779_write_checked(DEMCR_ADDRESS,
                                demcr | H779_DEMCR_VC_CORERESET));
    /* SYSRESETREQ 保留 PRIGROUP 并写入密钥，不设置 VECTRESET/VECTCLRACTIVE。 */
    H779_TRY(h779_read32(AIRCR_ADDRESS, &aircr));
    aircr = H779_AIRCR_VECTKEY | (aircr & H779_AIRCR_PRIGROUP_MASK) |
            H779_AIRCR_SYSRESETREQ;
    /* 读取 DHCSR 清除旧的复位粘滞标志，再发起本次复位。 */
    H779_TRY(h779_read32(DHCSR_ADDRESS, &value));
    if ((value & H779_DHCSR_CONTROL_MASK) != halt ||
        !(value & H779_DHCSR_S_HALT)) {
        H779_ERR("unexpected debug controls before reset DHCSR=0x%08x expected_control=0x%08x\n",
                  value, halt);
        return -EIO;
    }

    /* 从此处起，确认失败也必须重新连接；保留占用到 close，但禁止 close 再访问目标。 */
    h779.ready = 0;
    h779.finished = 0;
    h779.linked = 0;
    /* AIRCR 触发命令只写一次；结果不确定时不重放，也不以回读相等作为确认。 */
    H779_TRY(h779_write32(AIRCR_ADDRESS, aircr));
    mdelay(H779_RESET_SETTLE_MS);
    H779_TRY(h779_set_csw(h779.csw32));

    deadline = h779_deadline();
    for (;;) {
        H779_TRY(h779_read32(DHCSR_ADDRESS, &value));
        if (value & H779_DHCSR_S_RESET_ST)
            saw_reset = 1;
        else if (saw_reset && (value & H779_DHCSR_S_HALT))
            break; /* 已观察到新复位，随后复位结束且核心被捕获为暂停。 */
        if (time_after_eq(jiffies, deadline)) {
            H779_ERR("system reset confirmation timeout DHCSR=0x%08x saw_reset=%d S_RESET_ST=%u S_HALT=%u\n",
                      value, saw_reset, !!(value & H779_DHCSR_S_RESET_ST),
                      !!(value & H779_DHCSR_S_HALT));
            return -ETIMEDOUT;
        }
        h779_pause();
    }
    /* 复位可能改变调试控制；释放前在暂停状态下再次确认 STEP 和 MASKINTS 为 0。 */
    H779_TRY(h779_write32(DHCSR_ADDRESS, H779_DHCSR_DBGKEY | halt));
    H779_TRY(h779_read32(DHCSR_ADDRESS, &value));
    if ((value & H779_DHCSR_CONTROL_MASK) != halt ||
        !(value & H779_DHCSR_S_HALT)) {
        H779_ERR("unexpected debug controls after reset DHCSR=0x%08x expected_control=0x%08x\n",
                  value, halt);
        return -EIO;
    }
    H779_TRY(h779_read32(DEMCR_ADDRESS, &demcr));
    H779_TRY(h779_write_checked(DEMCR_ADDRESS,
                                demcr & ~H779_DEMCR_VC_CORERESET));
    /* 释放核心后不再读取目标内存，但仍排空最后一笔 AP 写入。 */
    return h779_write32(DHCSR_ADDRESS, H779_DHCSR_DBGKEY);
}

/* 在复制结果到用户态之前记录硬件失败，避免后续复制错误掩盖会话状态。 */
static void h779_invalidate_on_error(int ret)
{
    if (ret == -EIO || ret == -ETIMEDOUT || ret == -EBUSY) {
        h779.ready = 0;
        h779.finished = 0;
    }
}

/* 调用者必须持有驱动总线锁；这里只处理 H779 生产接口。 */
int h779_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct swd_h779_version version;
    struct swd_h779_connect connection;
    struct swd_h779_erase erase;
    struct swd_h779_transfer transfer;
    unsigned char *data = NULL;
    void *user = (void *)arg;
    int ret = 0, clean;
    if (cmd == SWD_H779_IOC_GET_VERSION) {
        version.abi_version = SWD_H779_ABI_VERSION;
        if (copy_to_user(user, &version, sizeof(version))) {
            H779_ERR("GET_VERSION result copy to user failed\n");
            return -EFAULT;
        }
        return 0;
    }
    if (cmd != SWD_H779_IOC_CONNECT && cmd != SWD_H779_IOC_GET_INFO &&
        cmd != SWD_H779_IOC_GET_STATUS && cmd != SWD_H779_IOC_ERASE &&
        cmd != SWD_H779_IOC_PROGRAM && cmd != SWD_H779_IOC_READ &&
        cmd != SWD_H779_IOC_FINISH && cmd != SWD_H779_IOC_RESET) {
        H779_ERR("unsupported ioctl command=0x%lx\n", (unsigned long)cmd);
        return -ENOTTY;
    }
    if (h779_owner && h779_owner != file) {
        H779_ERR("ioctl 0x%lx blocked: device owned by another H779 session\n",
                  (unsigned long)cmd);
        return -EBUSY;
    }
    if (cmd == SWD_H779_IOC_CONNECT) {
        if (copy_from_user(&connection, user, sizeof(connection))) {
            H779_ERR("CONNECT parameter copy from user failed\n");
            return -EFAULT;
        }
        /* 连接失败也保留文件占用，供调用方检查状态或安全关闭。 */
        h779_owner = file;
        ret = h779_connect(&connection);
        if (copy_to_user(user, &connection, sizeof(connection))) {
            H779_ERR("CONNECT result copy to user failed (operation_ret=%d)\n", ret);
            return -EFAULT;
        }
        return ret;
    }
    if (h779_owner != file || !h779.linked) {
        H779_ERR("ioctl 0x%lx has no active connection (linked=%d)\n",
                  (unsigned long)cmd, h779.linked);
        return -ENOTCONN;
    }
    switch (cmd) {
    case SWD_H779_IOC_GET_INFO:
        if (copy_to_user(user, &h779.info, sizeof(h779.info))) {
            H779_ERR("GET_INFO result copy to user failed\n");
            return -EFAULT;
        }
        return 0;
    case SWD_H779_IOC_GET_STATUS:
        ret = h779_status();
        if (ret)
            break;
        if (copy_to_user(user, &h779.status, sizeof(h779.status))) {
            H779_ERR("GET_STATUS result copy to user failed\n");
            return -EFAULT;
        }
        return 0;
    case SWD_H779_IOC_ERASE:
        if (copy_from_user(&erase, user, sizeof(erase))) {
            H779_ERR("ERASE parameter copy from user failed\n");
            return -EFAULT;
        }
        if (!h779.ready) {
            H779_ERR("ERASE requires a ready download session\n");
            return -ENOTCONN;
        }
        ret = h779_erase(&erase);
        clean = h779_cleanup();
        if (!ret)
            ret = clean;
        if (ret && ret != -EINVAL && ret != -EACCES)
            h779.ready = 0;
        h779_invalidate_on_error(ret);
        if (copy_to_user(user, &erase, sizeof(erase))) {
            H779_ERR("ERASE result copy to user failed (operation_ret=%d completed=%u/%u)\n",
                      ret, erase.completed, erase.length);
            return -EFAULT;
        }
        break;
    case SWD_H779_IOC_PROGRAM:
    case SWD_H779_IOC_READ:
        if (copy_from_user(&transfer, user, sizeof(transfer))) {
            H779_ERR("transfer parameter copy from user failed command=0x%lx\n",
                      (unsigned long)cmd);
            return -EFAULT;
        }
        if (!transfer.length || transfer.length > SWD_H779_MAX_TRANSFER ||
            !transfer.buffer || transfer.buffer > 0xFFFFFFFFU - transfer.length) {
            H779_ERR("invalid transfer length=%u buffer=0x%08x command=0x%lx (length 1..%u and valid PPC32 buffer required)\n",
                      transfer.length, transfer.buffer, (unsigned long)cmd,
                      SWD_H779_MAX_TRANSFER);
            return -EINVAL;
        }
        if (cmd == SWD_H779_IOC_PROGRAM && !h779.ready) {
            H779_ERR("PROGRAM requires a ready download session\n");
            return -ENOTCONN;
        }
        data = vmalloc(transfer.length);
        if (!data) {
            H779_ERR("cannot allocate transfer buffer length=%u\n", transfer.length);
            return -ENOMEM;
        }
        transfer.completed = 0;
        if (cmd == SWD_H779_IOC_PROGRAM) {
            if (copy_from_user(data, (void *)(unsigned long)transfer.buffer,
                               transfer.length)) {
                H779_ERR("PROGRAM payload copy from user failed address=0x%08x length=%u\n",
                          transfer.address, transfer.length);
                ret = -EFAULT;
            } else
                ret = h779_program(&transfer, data);
            clean = h779_cleanup();
            if (!ret)
                ret = clean;
            if (ret && ret != -EINVAL && ret != -EACCES && ret != -EFAULT &&
                ret != -EEXIST)
                h779.ready = 0;
        } else {
            ret = h779_read(&transfer, data);
            if (!ret && copy_to_user((void *)(unsigned long)transfer.buffer,
                                     data, transfer.length)) {
                H779_ERR("READ data copy to user failed address=0x%08x length=%u\n",
                          transfer.address, transfer.length);
                transfer.completed = 0;
                ret = -EFAULT;
            }
        }
        vfree(data);
        h779_invalidate_on_error(ret);
        if (copy_to_user(user, &transfer, sizeof(transfer))) {
            H779_ERR("transfer result copy to user failed (operation_ret=%d completed=%u/%u command=0x%lx)\n",
                      ret, transfer.completed, transfer.length, (unsigned long)cmd);
            return -EFAULT;
        }
        break;
    case SWD_H779_IOC_FINISH:
        ret = h779_finish();
        break;
    case SWD_H779_IOC_RESET:
        ret = h779_reset();
        break;
    default:
        H779_ERR("unhandled ioctl command=0x%lx\n", (unsigned long)cmd);
        return -ENOTTY;
    }
    h779_invalidate_on_error(ret);
    if (ret && ret != -EINVAL && ret != -EACCES && ret != -EFAULT &&
        ret != -EEXIST)
        H779_ERR("ioctl command=0x%lx failed=%d\n", (unsigned long)cmd, ret);
    return ret;
}


/* close 只回收会话，不会启动程序；RESET 后 linked 为 0，不再碰目标硬件。 */
void h779_release(struct file *file)
{
    if (h779_owner != file)
        return;
    if (h779.linked && h779_cleanup())
        printk(KERN_ERR "h779: close cleanup unconfirmed; target left halted\n");
    h779.ready = 0;
    h779.linked = 0;
    h779.finished = 0;
    h779_owner = NULL;
}

#undef H779_TRY
#undef H779_ERR
