# 基于 MPC5200 的 GD32H779 SWD 烧录驱动

本项目在 MPC5200 平台上通过 GPIO 模拟 SWD 协议，实现 GD32H779 的连接、Flash 擦除、编程、回读校验及系统复位。驱动面向 PowerPC 32 位大端架构和 Linux 2.4，使用 ppc_82xx 工具链编译。

代码分为 GPIO 操作、SWD 协议传输、H779 烧录和字符设备驱动四个部分，通过 ioctl 向上层提供下载接口。上层程序负责解析 HEX 文件、组织擦除与编程计划；驱动负责将数据写入目标 SRAM，并利用 H779 的 MDMA 完成 Flash 编程。

支持 GD32H779 W 型的 CNVM 和 Data ECNVM 擦写，不依赖外部硬件调试器。

## 文件职责

| 文件 | 职责 |
|---|---|
| `swd.c` | 字符设备注册、ioctl 入口、总线互斥、close 清理 |
| `swd_gpio.c` / `swd_gpio.h` | MPC5200 引脚配置、GPIO 电平、方向及采样 |
| `swd_transport.c` / `swd_transport.h` | SWD 请求头、ACK、32 位数据与奇偶校验、WAIT 重试、DP 读写、连接激活序列 |
| `h779_flash.c` / `h779_flash.h` | 原 H779 会话、AP/内存访问、几何检查、擦除、MDMA 编程、校验、FINISH 和 RESET |
| `h779_regs.h` | H779 目标寄存器地址、位定义、内存布局 |
| `swd_h779_ioctl.h` | 用户态与驱动共用的 ABI 3 接口 |
| `Makefile` | Linux 2.4 多对象模块构建 |

字符设备入口不再绕经 F450 分发函数。调用关系为：

```text
上层解析 HEX，得到地址和原始字节
  → ioctl(fd, SWD_H779_IOC_..., 参数)
  → swd.c：锁住总线
  → h779_flash.c：处理当前会话和烧录操作
  → swd_transport.c：DP/AP 寄存器的 SWD 传输
  → swd_gpio.h：操作 MPC5200 GPIO
```

H779 的 AP 选择、TAR/DRW 内存访问仍在 H779 模块中，以保留现有烧录框架。底层只封装实际用到的功能，不增加其他芯片的抽象或接口。

## 本次改变和保留内容

删除 F450 的 FMC 定义、擦写函数、旧 `read/write` 下载通路、数字 ioctl 1～14、测试命令、统计打印及未使用的其他芯片定义。新版本不提供通用目标寄存器读写的用户态命令。

底层用小函数组织时钟、位读写、请求、数据和总线换向，显式处理最低有效位与奇偶校验；读数据只有在校验成功后才交给调用方。GPIO 使用 `volatile u32` 访问和 PowerPC `eieio` 排序，避免编译器省略硬件访问或打乱先后关系。引脚、原采样相位、连接序列及 8 个空闲时钟保留。默认半周期延时仍为 100ns，实际频率受 GPIO 操作、屏障和函数开销影响，不能直接视为精确 5MHz。

WAIT 仍最多尝试 3 次；仅 WAIT 允许重试。FAULT、奇偶校验错误和异常 ACK 直接返回，避免不确定写入被重新执行。AP 读仍经 DP.RDBUFF 取得真实数据，AP 写仍排空并检查 DP 错误，不增加 SELECT/CSW 缓存或批量事务。

保留以下 H779 行为：

- ABI 3 的所有命令编码、结构体布局、返回码及 `completed` 含义。
- 连接时暂停核心、冻结看门狗、检查几何、保护与资源。
- SWD 写 SRAM0，再由 MDMA 通道 0 以 64 位目标访问写入 NVM。
- 32 字节编程及地址对齐、CNVM 64KB 扇区擦除、Data ECNVM 4KB 页或 64KB 扇区擦除。
- 现有 CNVM 擦除准备位图、软件空白检查、ECNVM PGCHEN、编程后回读比较。
- FINISH 只收尾并保持暂停，RESET 使用 AIRCR.SYSRESETREQ 后释放调试暂停。
- 失败诊断、超时、资源清理和不自动重放触发命令的规则。

本次仅重构和拆分，不调整上面这些烧录策略，也不修改启动选项、OTP 或保护配置。支持范围仍是已实现的 W 型布局：CNVM 2MB、ECNVM 7.5MB。

## 上层调用

上层只需包含 `swd_h779_ioctl.h`。常规顺序为：

```text
open → GET_VERSION → CONNECT → GET_INFO（可选）
     → READ 原内容并合并（需要保存擦除单元内其他数据时）
     → ERASE → PROGRAM → READ 校验
     → FINISH → RESET（需要立即启动时）→ close
```

所有烧录命令使用同一个文件描述符。GET_VERSION 不建立目标连接；CONNECT 占用会话，直到 close 才释放。FINISH 不启动目标，close 也不自动启动目标。另一个文件描述符可以查询 ABI，但不能抢占已连接的 H779 会话。

`CONNECT.scratch_address` 指向目标 SRAM0 中独占的 32 字节暂存区，需 32 字节对齐，范围为 `0x30000000～0x30003FE0`。`timeout_ms=0` 使用原默认 30000ms。

`ERASE.unit` 是实际字节数，CNVM 为 65536，Data ECNVM 可以为 4096 或 65536；地址、长度都按擦除单位对齐。`PROGRAM.address` 和 `length` 都按 32 字节对齐，每次最多 65536 字节；`READ` 每次最多 65536 字节。buffer 是 MPC5200 用户态 32 位地址，内容为原始字节，调用方无需调整字节序。

PROGRAM 成功且 `completed == length` 表示本次请求已经编程并通过回读比较；全部 HEX 是否下载完成由上层根据自己的计划判断。CNVM 允许在已准备扇区内从任意 32 字节对齐地址开始，不要求写满扇区。保留数据时，上层仍需在擦除之前读出、合并，再重建整个擦除单元。

现有 `../h779/test_h779_hex.c` 和已经使用相同 ABI 3 编译的下载程序可以继续调用本版本。这里不复制测试程序或固件。原 `check_h779_run.c` 依赖数字 ioctl 3；该命令已经删除，因此这个运行检查程序不能直接用于独立版。当前 H 类 READ 只开放 NVM 地址，不能用它替代 SRAM 邮箱读取。

## 编译与替换

本目录已经提供全部驱动源文件，不依赖 `../h779` 或 `../f450` 的头文件和对象。

```sh
make -C /Users/chenwen/work/swd/h779-new \
    KERNEL_DIR=/path/to/configured/linux-2.4 \
    ARCH=ppc CROSS_COMPILE=ppc_82xx-
```

产物为 `swd_h779.o`。沿用设备注册名 `swd_gd32`、主设备号 246；现有 `/dev/gd32` 若是 `c 246 0`，可以继续使用。

在目标板替换驱动时，先关闭下载进程、卸载原模块，再加载 `swd_h779.o`，不要同时加载两个版本。设备号注册失败时，新驱动不会先改动旧驱动正在使用的 GPIO。

若沿用自己的 BSP Makefile，必须把以下 4 个源文件都编译并用 `ld -r` 链接为同一个模块，而不再只编译一个 `swd.c`：

```text
swd.o + swd_gpio.o + swd_transport.o + h779_flash.o → swd_h779.o
```

源文件之间的接口只在该组合模块内链接，不导出内核符号。Linux 2.4 的 `file_operations.owner` 负责模块引用计数，不与旧手动 `MOD_INC_USE_COUNT/MOD_DEC_USE_COUNT` 叠加。信号量保护整次 ioctl 和 close；会话 owner 检查则由 H779 模块保留。
