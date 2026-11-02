# 基于 MPC5200 的 GD32H779 SWD 烧录驱动

本项目在 MPC5200 平台上通过 GPIO 模拟 SWD 协议，实现 GD32H779 的调试连接、Flash 擦除、编程、回读校验及系统复位。驱动面向 PowerPC 32 位大端架构和 Linux 2.4，使用 `ppc_82xx-` 工具链编译，无需外接硬件调试器。

驱动通过字符设备的 `ioctl` 接口为上层提供烧录功能。上层程序负责解析 HEX 文件、组织擦除和编程计划；驱动负责执行目标寄存器访问、数据搬运、Flash 操作及结果检查。

## 主要功能

- **GPIO 操作**：配置 MPC5200 的 SWCLK、SWDIO 引脚，实现时钟输出、双向数据传输和采样。
- **SWD 协议**：实现 JTAG-to-SWD 切换、Dormant 激活、请求头、总线换向、ACK、32 位数据和奇偶校验。WAIT 最多尝试 3 次，FAULT 和校验错误直接返回。
- **DP/AP 访问**：通过 DP 建立调试连接，通过 AP 的 CSW、TAR、DRW 访问目标寄存器和内存；AP 读取通过 DP.RDBUFF 获取实际结果。
- **烧录准备**：暂停目标核心、冻结调试暂停期间的看门狗，并检查容量、内存布局、保护状态及所需资源。
- **Flash 擦除**：支持 CNVM 扇区擦除，以及 Data ECNVM 页擦除和扇区擦除；根据增强配置处理 CNVM 对应的增强区域。
- **Flash 编程**：通过 SWD 将数据写入 SRAM0 暂存区，再由 MDMA 通道 0 以 64 位目标访问送入 NVM，按 32 字节单元完成编程和回读比较。
- **烧录收尾与复位**：FINISH 完成硬件收尾并保持核心暂停；RESET 通过 AIRCR.SYSRESETREQ 请求系统复位，随后释放调试暂停。
- **会话与错误处理**：串行化总线访问，管理文件描述符对应的烧录会话，提供超时检测、资源清理及具体错误日志。

GPIO 使用 `volatile u32` 访问和 PowerPC `eieio` 排序。SWD 默认半周期延时为 100ns，实际频率还受 GPIO 访问及软件开销影响。数据按最低有效位先传输，只有奇偶校验通过后才向调用方返回读值。

## 支持范围

支持 GD32H779 W 型布局：CNVM 2MB、ECNVM 7.5MB。

| 可擦写区域 | 起始地址 | 结束地址（含） | 擦除单位 | 编程单位 |
|---|---|---|---|---|
| CNVM | `0x08000000` | `0x081FFFFF` | 64KB | 32 字节 |
| Data ECNVM0 | `0x08200000` | `0x084FFFFF` | 4KB 或 64KB | 32 字节 |
| Data ECNVM1 | `0x08900000` | `0x0897FFFF` | 4KB 或 64KB | 32 字节 |

CNVM 擦除后为全 0，ECNVM 擦除后为全 FF。增强 Bank 由 CNVM 擦除和增强机制管理，不作为普通擦写目标。READ 支持读取有效 NVM 地址范围 `0x08000000～0x0897FFFF`。

驱动不修改启动选项、OTP、保护等级或目标时钟配置。

## 文件结构

| 文件 | 职责 |
|---|---|
| `swd.c` | 字符设备注册、ioctl 入口、总线互斥、关闭时清理 |
| `swd_gpio.c` / `swd_gpio.h` | MPC5200 GPIO 初始化、电平控制、方向切换及采样 |
| `swd_transport.c` / `swd_transport.h` | SWD 位传输、请求与响应、DP 读写、连接激活序列 |
| `h779_flash.c` / `h779_flash.h` | H779 会话、AP/内存访问、配置检查、擦除、编程、校验和复位 |
| `h779_regs.h` | H779 寄存器地址、位定义及内存布局 |
| `swd_h779_ioctl.h` | 用户态与驱动共用的 ABI 3 接口 |
| `Makefile` | Linux 2.4 多对象模块构建 |

调用关系为：

```text
上层解析 HEX，得到目标地址和原始字节
  → ioctl：提交擦除、编程或读取请求
  → swd.c：串行化总线访问
  → h779_flash.c：执行 H779 烧录流程
  → swd_transport.c：传输 DP/AP 请求与数据
  → swd_gpio.h：操作 MPC5200 GPIO
```

## 用户态接口

上层程序包含 `swd_h779_ioctl.h`，使用同一个文件描述符完成一次下载。

| ioctl 命令 | 功能 |
|---|---|
| `SWD_H779_IOC_GET_VERSION` | 查询 ABI 版本 |
| `SWD_H779_IOC_CONNECT` | 建立连接、暂停核心、配置烧录资源，返回芯片信息 |
| `SWD_H779_IOC_GET_INFO` | 获取容量、擦写单位、增强和保护信息 |
| `SWD_H779_IOC_GET_STATUS` | 获取 NVM、MDMA、RAM ECC 和会话状态 |
| `SWD_H779_IOC_ERASE` | 按指定单位擦除目标地址范围 |
| `SWD_H779_IOC_PROGRAM` | 编程原始字节数据，并回读比较 |
| `SWD_H779_IOC_READ` | 读取 NVM 数据到用户缓冲区 |
| `SWD_H779_IOC_FINISH` | 完成烧录收尾，保持核心暂停 |
| `SWD_H779_IOC_RESET` | 成功 FINISH 后复位目标并释放调试暂停 |

推荐调用顺序：

```text
open → GET_VERSION → CONNECT → GET_INFO（可选）
     → READ 并合并原内容（需要保留同一擦除单元内其他数据时）
     → ERASE → PROGRAM → READ 校验
     → FINISH → RESET（需要立即启动时）→ close
```

GET_VERSION 不建立目标连接。CONNECT 占用烧录会话，直到关闭文件描述符才释放；其他文件描述符可以查询 ABI，但不能抢占该会话。FINISH 和 close 均不会自动启动目标程序。

### 参数与完成语义

- `CONNECT.scratch_address` 指向目标 SRAM0 中独占的 32 字节暂存区，要求 32 字节对齐，范围为 `0x30000000～0x30003FE0`。`timeout_ms=0` 使用默认 30000ms，非零允许 100～120000ms。
- `ERASE.unit` 传实际字节数 4096 或 65536；CNVM 只允许 65536。擦除地址和长度均须按所选单位对齐，一次请求应位于同一可擦写区域内。
- `PROGRAM.address` 要求 32 字节对齐，length 为 32 的正整数倍，每次最多 65536 字节。驱动不自动擦除或补齐数据。
- `READ` 每次最多 65536 字节，只读取 NVM，不提供通用寄存器或 SRAM 读取接口。
- `PROGRAM` 和 `READ` 的 buffer 是 MPC5200 用户态的 32 位缓冲区地址。数据以原始字节传递，驱动处理主机与目标的字节序差异，调用方无需交换字节序。

CNVM 编程要求本会话已完成相关扇区的擦除准备，允许从扇区内任意 32 字节对齐地址开始，不要求写满整个扇区。编程时执行软件空白检查；Data ECNVM 同时使用 PGCHEN 硬件检查。

PROGRAM 是同步操作：成功且 `completed == length` 时，本次请求已经完成 MDMA 搬运、NVM 编程和回读比较。全部 HEX 是否下载完成，由上层根据下载计划判断。失败时的 completed 仅表示已确认完成的前缀，不代表出错单元未发生改变。

若需保留同一擦除单元中未被 HEX 覆盖的数据，上层应先读取并合并，再擦除和重建该单元。目标 SRAM 暂存区和 MDMA 通道 0 需由下载过程独占。

## 编译与加载

使用已配置好的目标 Linux 2.4 BSP，在本目录执行：

```sh
make KERNEL_DIR=/path/to/configured/linux-2.4 \
    ARCH=ppc CROSS_COMPILE=ppc_82xx-
```

构建过程将 4 个源文件编译并链接为一个模块：

```text
swd.o + swd_gpio.o + swd_transport.o + h779_flash.o → swd_h779.o
```

字符设备注册名为 `swd_gd32`，主设备号为 246。目标板加载及创建设备节点的示例：

```sh
insmod swd_h779.o
# 设备节点不存在时创建：
mknod /dev/gd32 c 246 0
```

上层下载程序通过 `open("/dev/gd32", O_RDWR)` 打开设备后调用 ioctl。Linux 2.4 的 `file_operations.owner` 管理模块引用计数；信号量保护整次 ioctl 和关闭清理，H779 层管理会话归属。
