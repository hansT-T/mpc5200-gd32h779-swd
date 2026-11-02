#ifndef H779_REGS_H
#define H779_REGS_H

/* Cortex-M 调试寄存器。 */
#define DHCSR_ADDRESS 0xE000EDF0U
#define DEMCR_ADDRESS 0xE000EDFCU
#define AIRCR_ADDRESS 0xE000ED0CU

/* AP IDR 字段：CLASS[16:13]、TYPE[3:0]。 */
#define H779_AP_IDR_CLASS_SHIFT  13U
#define H779_AP_IDR_CLASS_MASK   (0xFU << H779_AP_IDR_CLASS_SHIFT)
#define H779_AP_IDR_CLASS_MEM_AP 8U
#define H779_AP_IDR_TYPE_MASK    0xFU
#define H779_AP_IDR_TYPE_AHB3    1U

/* H779 AHB-AP 的 CSW 访问属性。 */
#define H779_CSW_PRIVILEGED      (1U << 25)
#define H779_CSW_BUFFERABLE      (1U << 26)
#define H779_CSW_CACHEABLE       (1U << 27)

/* 支持的容量与 NVM 版本字段，用户手册第 147～148、336 页。 */
#define H779_W_CNVM_KB                  2048U
#define H779_W_ECNVM_KB                 7680U
#define H779_DENSITY_ECNVM_MASK         0x0000FFFFU
#define H779_DENSITY_SRAM_SHIFT         16U
#define H779_CSTAT_NVMVER_SHIFT         14U
#define H779_CSTAT_NVMVER_MASK          (3U << H779_CSTAT_NVMVER_SHIFT)
#define H779_CSTAT_NVMVER_W             0U

/* NVMC_OBSTAT0_EFT 字段，用户手册第 303～304 页。 */
#define H779_OBSTAT_SPC_SHIFT           8U
#define H779_OBSTAT_SPC_MASK            (0xFFU << H779_OBSTAT_SPC_SHIFT)
#define H779_OBSTAT_SPC_UNPROTECTED     0xAAU
#define H779_OBSTAT_FW_UPDATE           (1U << 19)
#define H779_OBSTAT_SWAP                (1U << 20)
#define H779_OBSTAT_SCR                 (1U << 21)

/* *_ADDR_EFT 区域使能位：CNVM[15]、Bank0[14]、Bank1[13]。 */
#define H779_DCRP_EN_MASK               ((1U << 15) | (1U << 14) | (1U << 13))
#define H779_SCR_EN_MASK                ((1U << 15) | (1U << 14) | (1U << 13))

/* NVMC_OTP_USER_CTL0 保护字段，用户手册第 351～352 页。 */
#define H779_OTP0_SPC_L                 (1U << 19)
#define H779_OTP0_SPC_H                 (1U << 20)
#define H779_OTP0_SCR                   (1U << 21)
#define H779_OTP0_SCR_EFFCTL            (1U << 22)
#define H779_OTP0_DCRP_EFFCTL           (1U << 23)
/* 本后端不支持的配置组合掩码，并非独立的硬件字段。 */
#define H779_OTP0_UNSUPPORTED_PROTECTION \
    (H779_OTP0_SPC_L | H779_OTP0_SPC_H | H779_OTP0_SCR | \
     H779_OTP0_SCR_EFFCTL | H779_OTP0_DCRP_EFFCTL)

/* 有效的数据完整性增强配置，用户手册第 322、354 页。 */
#define H779_OBREP_DIEEN                (1U << 0)
#define H779_OBREP_DIESZ_SHIFT          8U
#define H779_OBREP_DIESZ_MASK           (31U << H779_OBREP_DIESZ_SHIFT)
#define H779_OTP1_DIE_EN                (1U << 2)
#define H779_OTP1_SWAP_DISABLE          (1U << 4)
#define H779_OTP1_ECNVM_DIE_DISABLE     (1U << 6)

/* 以下均为 H779 目标地址，不能当作 MPC5200 本机地址解引用。 */
#define H779_NVMC_BASE           0x52002000U
#define H779_PERF                (H779_NVMC_BASE + 0x000U)
#define H779_CKEY                (H779_NVMC_BASE + 0x004U)
#define H779_CCTL                (H779_NVMC_BASE + 0x00CU)
#define H779_CSTAT               (H779_NVMC_BASE + 0x010U)
#define H779_CADDR               (H779_NVMC_BASE + 0x014U)
#define H779_OBSTAT              (H779_NVMC_BASE + 0x01CU)
#define H779_DCRP                (H779_NVMC_BASE + 0x028U)
#define H779_SCR                 (H779_NVMC_BASE + 0x030U)
#define H779_WPC                 (H779_NVMC_BASE + 0x038U)
#define H779_WPE0                (H779_NVMC_BASE + 0x080U)
#define H779_WPE1                (H779_NVMC_BASE + 0x084U)
#define H779_OBREP               (H779_NVMC_BASE + 0x090U)
#define H779_EKEY                (H779_NVMC_BASE + 0x150U)
#define H779_ECTL                (H779_NVMC_BASE + 0x154U)
#define H779_ESTAT               (H779_NVMC_BASE + 0x158U)
#define H779_EADDR               (H779_NVMC_BASE + 0x15CU)
#define H779_OTP0                (H779_NVMC_BASE + 0x20CU)
#define H779_OTP1                (H779_NVMC_BASE + 0x210U)
#define H779_DHCSR_DBGKEY         0xA05F0000U
#define H779_DHCSR_C_DEBUGEN      (1U << 0)
#define H779_DHCSR_C_HALT         (1U << 1)
#define H779_DHCSR_C_STEP         (1U << 2)
#define H779_DHCSR_C_MASKINTS     (1U << 3)
#define H779_DHCSR_CONTROL_MASK  (H779_DHCSR_C_DEBUGEN | H779_DHCSR_C_HALT | \
                                  H779_DHCSR_C_STEP | H779_DHCSR_C_MASKINTS)
#define H779_DHCSR_S_HALT         (1U << 17)
#define H779_DHCSR_S_RESET_ST     (1U << 25)
#define H779_DEMCR_VC_CORERESET   (1U << 0)
#define H779_AIRCR_VECTKEY        (0x05FAU << 16)
#define H779_AIRCR_PRIGROUP_MASK  (7U << 8)
#define H779_AIRCR_SYSRESETREQ    (1U << 2)
#define H779_RESET_SETTLE_MS     10U
#define H779_DBG_CTL1            0xE00E1034U
#define H779_DBG_CTL4            0xE00E1054U
#define H779_DENSITY             0x1FF0F7E0U
#define H779_CSIZE               0x1FF0F7DEU
#define H779_AHB2EN              0x58024434U
#define H779_AHB3EN              0x58024438U
#define H779_RAM_ECC_CTL         0x48023020U
#define H779_RAM_ECC_STAT        0x48023024U
#define H779_SRAM0_BEGIN          0x30000000U
#define H779_SRAM0_END            0x30004000U
#define H779_CNVM_BEGIN           0x08000000U
#define H779_CNVM_END             0x08200000U
#define H779_DATA0_BEGIN          0x08200000U
#define H779_DATA0_END            0x08500000U
#define H779_BANK1_BEGIN          0x08500000U
#define H779_BANK0_BEGIN          0x08700000U
#define H779_DATA1_BEGIN          0x08900000U
#define H779_DATA1_END            0x08980000U
#define H779_PAGE                 0x1000U
#define H779_SECTOR               0x10000U
#define H779_BLOCK                32U
#define H779_LK                   (1U << 0)
#define H779_PG                   (1U << 1)
#define H779_SER                  (1U << 2)
#define H779_PGCHEN               (1U << 4)
#define H779_PER                  (1U << 6)
#define H779_START                (1U << 7)
#define H779_END                  (1U << 16)
#define H779_CBUSY                ((1U << 0) | (1U << 3))
#define H779_EBUSY                (1U << 0)
#define H779_CSTAT_CNVMECCF        (1U << 31)
#define H779_ESTAT_ECNVM_ECCF      (1U << 31)
#define H779_CERRORS              0x15C60000U
#define H779_EERRORS              0x05860000U
#define H779_PGSZ_MASK            (3U << 20)
#define H779_PGSZ32               (1U << 20)
#define H779_PREFETCH_MASK         (3U << 22)
#define H779_EMPTY_CBUF           (1U << 31)
#define H779_MDMA_BASE            0x52000000U
#define H779_DMA_STAT             (H779_MDMA_BASE + 0x040U)
#define H779_DMA_CLEAR            (H779_MDMA_BASE + 0x044U)
#define H779_DMA_ERROR            (H779_MDMA_BASE + 0x048U)
#define H779_DMA_CTL              (H779_MDMA_BASE + 0x04CU)
#define H779_DMA_CFG              (H779_MDMA_BASE + 0x050U)
#define H779_DMA_COUNT            (H779_MDMA_BASE + 0x054U)
#define H779_DMA_SRC              (H779_MDMA_BASE + 0x058U)
#define H779_DMA_DST              (H779_MDMA_BASE + 0x05CU)
#define H779_DMA_CFG32            0x527C0EEAU

/* MDMA 通道控制与状态字段。 */
#define H779_DMA_CTL_CHEN         (1U << 0)
#define H779_DMA_CTL_SMODEN       (1U << 8)
#define H779_DMA_STAT_REQAF       (1U << 16)

#endif
