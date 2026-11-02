#ifndef H779_SWD_TRANSPORT_H
#define H779_SWD_TRANSPORT_H

#include <linux/types.h>

/* 请求参数只保存 APnDP、RnW、A2、A3；其余请求位由传输层生成。 */
#define SWD_REG_AP             0x01U
#define SWD_REG_DP             0x00U
#define SWD_REG_R              0x02U
#define SWD_REG_W              0x00U
#define SWD_REG_ADR(a)         ((a) & 0x0cU)

#define DAP_TRANSFER_OK        0x01U
#define DAP_TRANSFER_WAIT      0x02U
#define DAP_TRANSFER_FAULT     0x04U
#define DAP_TRANSFER_ERROR     0x08U

/* DP 同一地址的读写含义可能不同，例如 IDCODE 与 ABORT。 */
#define DP_IDCODE              0x00U
#define DP_ABORT               0x00U
#define DP_CTRL_STAT           0x04U
#define DP_SELECT              0x08U
#define DP_RDBUFF              0x0cU
#define STKCMPCLR              0x00000002U
#define STKERRCLR              0x00000004U
#define WDERRCLR               0x00000008U
#define ORUNERRCLR             0x00000010U
#define STICKYORUN             0x00000002U
#define STICKYCMP              0x00000010U
#define STICKYERR              0x00000020U
#define WDATAERR               0x00000080U
#define CDBGPWRUPREQ           0x10000000U
#define CDBGPWRUPACK           0x20000000U
#define CSYSPWRUPREQ           0x40000000U
#define CSYSPWRUPACK           0x80000000U
#define APBANKSEL              0x000000f0U

#define AP_CSW                 0x00U
#define AP_TAR                 0x04U
#define AP_DRW                 0x0cU
#define AP_IDR                 0xfcU
#define CSW_SIZE               0x00000007U
#define CSW_SIZE16             0x00000001U
#define CSW_SIZE32             0x00000002U
#define CSW_ADDRINC            0x00000030U
#define CSW_DBGSTAT            0x00000040U

/* 调用方须持有总线锁。仅 WAIT 最多尝试三次，其余结果不重发。 */
u8 swd_transfer(u32 request, u32 *data);
/* 与原底层约定一致：DP 访问返回 0 成功、1 失败。 */
u8 swd_dp_read(u8 reg, u32 *value);
u8 swd_dp_write(u8 reg, u32 value);
/* 保留原 JTAG-to-SWD 与 Dormant 激活顺序，不操作 NRST。 */
void swd_wire_connect(void);

#endif
