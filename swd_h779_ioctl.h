#ifndef SWD_H779_IOCTL_H
#define SWD_H779_IOCTL_H

#include <linux/types.h>
#include <asm/ioctl.h>

/* 供用户态下载程序调用的接口，不提供旧诊断和测试命令。 */
#define SWD_H779_ABI_VERSION       3U
#define SWD_H779_IOC_MAGIC         'H'

#define SWD_H779_PROGRAM_BYTES     32U
#define SWD_H779_ERASE_PAGE        4096U
#define SWD_H779_ERASE_SECTOR      65536U
#define SWD_H779_MAX_TRANSFER      65536U
#define SWD_H779_DEFAULT_TIMEOUT   30000U

struct swd_h779_version {
    __u32 abi_version;
};

struct swd_h779_info {
    __u32 abi_version;
    __u32 dp_idr;
    __u32 ap_idr;
    __u32 cnvm_bytes;
    __u32 ecnvm_bytes;
    __u32 sram_kb;
    __u32 program_bytes;
    __u32 cnvm_erase_bytes;
    __u32 ecnvm_page_bytes;
    __u32 ecnvm_sector_bytes;
    __u32 enhanced_bytes;
    __u32 active_bank;       /* 0 为 Bank0，1 为 Bank1；仅增强功能开启时有意义。 */
    __u32 wp_cnvm;
    __u32 wp0_ecnvm;
    __u32 wp1_ecnvm;
    __u32 obstat0;
    __u32 obrepctl;
};

struct swd_h779_connect {
    __u32 scratch_address;   /* 目标 SRAM0 中独占的 32 字节暂存区。 */
    __u32 timeout_ms;        /* 0 使用默认值，否则为 100～120000 毫秒。 */
    struct swd_h779_info info;
};

struct swd_h779_status {
    __u32 cnvm_stat;
    __u32 ecnvm_stat;
    __u32 cnvm_ctl;
    __u32 ecnvm_ctl;
    __u32 mdma_stat;
    __u32 ram_ecc;
    __u32 dirty_cnvm;        /* 为 ABI 兼容保留，固定返回 0。 */
    __u32 ready;
};

struct swd_h779_erase {
    __u32 address;
    __u32 length;
    __u32 unit;              /* 实际擦除字节数：4096 或 65536。 */
    __u32 completed;         /* 输出：已经完成的目标字节数，失败时也尽量回传。 */
};

struct swd_h779_transfer {
    __u32 address;
    __u32 length;            /* 单次调用不能超过 SWD_H779_MAX_TRANSFER。 */
    __u32 buffer;            /* 指向原始字节缓冲区的 32 位用户态地址。 */
    __u32 completed;         /* PROGRAM 为已确认前缀；READ 为完整 length 或 0。 */
};

#define SWD_H779_IOC_GET_VERSION \
    _IOR(SWD_H779_IOC_MAGIC, 0x50, struct swd_h779_version)
#define SWD_H779_IOC_CONNECT \
    _IOWR(SWD_H779_IOC_MAGIC, 0x51, struct swd_h779_connect)
#define SWD_H779_IOC_GET_INFO \
    _IOR(SWD_H779_IOC_MAGIC, 0x52, struct swd_h779_info)
#define SWD_H779_IOC_GET_STATUS \
    _IOR(SWD_H779_IOC_MAGIC, 0x53, struct swd_h779_status)
#define SWD_H779_IOC_ERASE \
    _IOWR(SWD_H779_IOC_MAGIC, 0x54, struct swd_h779_erase)
#define SWD_H779_IOC_PROGRAM \
    _IOWR(SWD_H779_IOC_MAGIC, 0x55, struct swd_h779_transfer)
#define SWD_H779_IOC_READ \
    _IOWR(SWD_H779_IOC_MAGIC, 0x56, struct swd_h779_transfer)
#define SWD_H779_IOC_FINISH \
    _IO(SWD_H779_IOC_MAGIC, 0x57)

/* 同一文件会话成功 FINISH 后，通过 SYSRESETREQ 复位并释放调试暂停。 */
#define SWD_H779_IOC_RESET \
    _IO(SWD_H779_IOC_MAGIC, 0x58)

#endif
