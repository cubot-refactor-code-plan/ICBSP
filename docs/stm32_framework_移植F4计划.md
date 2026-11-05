# stm32_framework 移植到 STM32F4 实施计划

- 版本：v2（2026-11-05）
- 范围：**STM32H7（现有，行为不变）+ STM32F4（本次目标）**；**不做 G4、不做 F1**
- 原则：**不新增文件（除 `bsp_series.hpp`）、不删、不重写**，差异全部用 `#if` 分支和宏补丁在原文件里套出来
- 不提交、不暂存（改动都在 `Middlewares/Third_Party/stm32_framework` 子模块内）

---

## 0. 目标

同一份 `code/` 在 H7 和 F4 的 CubeMX 工程里都能编译、都能跑。差异收敛在两处：

1. 新增 `code/bsp/bsp_series.hpp`：库内唯一 include HAL 的地方 + 系列标识 + 公共补丁宏
2. 各 BSP 外设文件内部的 `#if BSP_SERIES_F4` 分支，每处写明「F4 做了什么」

上层 `service/`、`tasks/`、`device/`、`algorithm/` 的**接口与调用方式完全不变**。

---

## 1. 实测结论（当前 F4 工程，STM32F407xx + CubeMX 生成的 HAL）

| 结论                                                                                                                       | 影响                                                                                          |
| -------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------- |
| F4 有 `DMA_Stream_TypeDef`、`DMA_SxCR_EN`、`DMA_FIFOMODE_DISABLE`、`DMA_InitTypeDef.FIFOMode`                      | **F4 与 H7 同属 DMA-stream 架构，`bsp_uart.hpp` 的 DMA 代码几乎全兼容**               |
| F4 **没有** `IS_DMA_STREAM_INSTANCE` 宏                                                                            | `bsp_series.hpp` 补一个等价宏即可                                                           |
| F4 有 `HAL_UARTEx_ReceiveToIdle_DMA` / `HAL_UART_AbortReceive` / `HAL_UART_AbortTransmit` / `huart->ReceptionType` | UART 主流程零改动                                                                             |
| F4 **没有** `USART_CR1_RXNEIE_RXFNEIE`、`USART_CR1_TXEIE_TXFNFIE`、`USART_CR3_RXFTIE`、`USART_CR3_TXFTIE`    | 用宏补丁，`bsp_uart.hpp` 里那些位操作零改动                                                 |
| F4 **没有** `HAL_UARTEx_EnableFifoMode`、`huart->FifoMode`、`UART_FIFOMODE_ENABLE`                             | 需要 2 处 `#if`关闭                                                                         |
| CM4 的 `DWT_Type` 有 `LAR` 成员                                                                                        | `bsp_dwt.cpp:18` 的 `DWT->LAR` 在 F4 上**可直接编译，无需改**                       |
| `RCC->D1CFGR` 只有 H7 有                                                                                                 | `get_cpu_clock_hz()` 需要 1 处 `#if`                                                      |
| F4 只有 bxCAN（`HAL_CAN`），无 FDCAN                                                                                     | CAN 需要新增 bxCAN 实现（写在 `bsp_can.hpp/.cpp` 的 `#elif` 分支里）                      |
| `CAN_ID_STD=0x0` / `CAN_ID_EXT=0x4`                                                                                    | 与 H7 的 `FDCAN_STANDARD_ID=0x0` / `FDCAN_EXTENDED_ID=0x4` 数值相同，上层比较逻辑原样复用 |
| F4 的 `SRAM` 在 `0x20000000`，CCM RAM 在 `0x10000000`（不可 DMA）                                                    | 不再有 ITCM/DTCM/AXI 概念                                                                     |

---

## 2. 设计原则

1. **一个 include 入口**：所有库文件把 `#include "stm32h7xx_hal.h"` 换成 `#include "bsp_series.hpp"`。
2. **能探测宏就不写系列判断**：F4 上缺什么宏就在 `bsp_series.hpp` 里补一个同名宏，让原代码不用改。
3. **H7 分支必须是现在的代码原文**：所有 `#if` 的 H7 侧保持逐字不变。
4. **不引入 STL、不用 `friend`、不为简单参数造类型别名。**

---

## 3. 新增 `code/bsp/bsp_series.hpp`（唯一新增文件）

| 区块             | 内容                                                                                                                                                                                                     | H7     | F4   |
| ---------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------ | ---- |
| ① HAL 引入      | `__has_include("stm32h7xx_hal.h")` → H7；`__has_include("stm32f4xx_hal.h")` → F4；都不是则 `#error`（用 `__has_include` 是因为 `STM32H7xx` 这类宏是 HAL 头内部才定义的）                     | 原生   | 原生 |
| ② 系列标识      | `BSP_SERIES_H7` / `BSP_SERIES_F4`（0/1）                                                                                                                                                             | —     | —   |
| ③ UART 位名补丁 | `USART_CR1_RXNEIE_RXFNEIE` → `USART_CR1_RXNEIE`；`USART_CR1_TXEIE_TXFNFIE` → `USART_CR1_TXEIE`；`USART_CR3_RXFTIE` / `USART_CR3_TXFTIE` → `0U`（仅在未定义时定义）                      | 原生   | 补丁 |
| ④ FIFO 能力     | `BSP_UART_HAS_FIFO`                                                                                                                                                                                    | 1      | 0    |
| ⑤ DMA 实例判定  | `IS_DMA_STREAM_INSTANCE()`：F4 补 8+8 条 `== DMAx_Streamn` 判断                                                                                                                                      | 原生   | 补丁 |
| ⑥ DMA 可达性    | `bsp_dma_reachable(addr)`：H7 走原 ITCM/DTCM 判定；F4 直接 `return true`                                                                                                                             | 原逻辑 | 恒真 |
| ⑦ CAN 开关      | `BSP_CAN_MODULE_ENABLED` = `defined(HAL_FDCAN_MODULE_ENABLED) \|\| defined(HAL_CAN_MODULE_ENABLED)`                                                                                                    | 1      | 1    |
| ⑧ CAN 兼容常量  | `FDCAN_STANDARD_ID`=`CAN_ID_STD`、`FDCAN_EXTENDED_ID`=`CAN_ID_EXT`、`FDCAN_DATA_FRAME`=`CAN_RTR_DATA`、`FDCAN_CLASSIC_CAN`=`0U`、`FDCAN_DLC_BYTES_0..8`=`0U..8U`（仅在未定义时定义） | 原生   | 补丁 |
| ⑨ CAN 兼容头    | `FDCAN_RxHeaderTypeDef` 同名字段结构体（`Identifier` / `IdType` / `RxFrameType` / `FDFormat` / `DataLength`）                                                                                | 原生   | 补丁 |
| ⑩ CPU 时钟      | `bsp_cpu_clock_hz()`：H7 走 `SYSCLK / D1CPRE`；F4 走 `HAL_RCC_GetHCLKFreq()`                                                                                                                       | 原逻辑 | HCLK |

> ⑧⑨ 是让 `can_rx_node.hpp`、`dji_motor.hpp`、`can_rx_task.cpp` 里 `rx.header.Identifier` / `FDCAN_CLASSIC_CAN` 这些**一行都不用改**的关键。

---

## 4. BSP 层改动（逐文件，含「F4 做了什么」）

### 4.1 `bsp_uart.hpp` —— 共 4 处

| 位置                                       | 改动                                                                              | F4 行为                                                                                                                            |
| ------------------------------------------ | --------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| `:36`                                    | `#include "stm32h7xx_hal.h"` → `"bsp_series.hpp"`                            | —                                                                                                                                 |
| `:66-72` `dma_reachable()`             | 函数体改为 `return bsp_dma_reachable(addr);`                                    | F4 恒 `true`：主 SRAM 全部可 DMA，不再被 DTCM 判定误杀（这是移植后第一个会撞到的运行时故障，表现为 `init()` 返回 `BAD_ARG`） |
| `:508` `ItTransport::rx_irq_enabled()` | `if (huart->FifoMode == UART_FIFOMODE_ENABLE)` 外面套 `#if BSP_UART_HAS_FIFO` | F4 没有 FIFO，只看 `RXNEIE｜IDLEIE`                                                                                              |
| `:922-925` `init()`                    | `#if UART_ENABLE_FIFO` → `#if UART_ENABLE_FIFO && BSP_UART_HAS_FIFO`         | F4 上整段不编译（`HAL_UARTEx_EnableFifoMode` 不存在）                                                                            |

其余**全部零改动**：`IS_DMA_STREAM_INSTANCE`、`DMA_Stream_TypeDef`、`DMA_SxCR_EN`、`prepare_rx()` 的 `Init.FIFOMode`、`HAL_UARTEx_ReceiveToIdle_DMA`、`ReceptionType`、流缓冲区、发送状态机、恢复流程。

### 4.2 `bsp_spi.cpp` / `bsp_spi.hpp`

| 位置                               | 改动                                                                                                                                                      | F4 行为                                                                               |
| ---------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------- |
| `.cpp:1`、`.hpp:61`            | include → `bsp_series.hpp`                                                                                                                             | —                                                                                    |
| `.cpp:38-44` `dma_reachable()` | 函数体改为 `return bsp_dma_reachable(addr);`                                                                                                            | F4 恒 `true`：调用方缓冲可直接 DMA，弹跳缓冲只在单段超 `SPI_DMA_BUFFER_SIZE` 时用 |
| `.cpp:243`、`.hpp:49` 注释     | 「MasterKeepIOState 必须为 ENABLE」补一句：F4 的 `SPI_InitTypeDef` 无此字段，一次片选内多段传输是否会出假时钟沿需实测；有问题时改为「一次片选只发一段」 | 待实测                                                                                |

### 4.3 `bsp_dwt.cpp` / `bsp_dwt.hpp`

| 位置                                 | 改动                                                                                | F4 行为                |
| ------------------------------------ | ----------------------------------------------------------------------------------- | ---------------------- |
| `.hpp:21`、`.cpp`                | include → `bsp_series.hpp`                                                       | —                     |
| `:18` `DWT->LAR`                 | **不改**（CM4 的 `DWT_Type` 有 `LAR`）；只补一行注释说明 CM4 也有该寄存器 | 直接可用               |
| `:136-144` `get_cpu_clock_hz()`  | 套 `#if BSP_SERIES_H7 / #else`，F4 分支 `return HAL_RCC_GetHCLKFreq();`         | CM4 的 CPU 时钟 = HCLK |
| `:112` 注释「只能是 1.818 的倍数」 | 补一句 F4 168 MHz 下步进约 5.95 ns                                                  | —                     |

### 4.4 `bsp_dma_buffer.hpp` —— 原地改，不新开文件

| 位置                               | 改动                                                                                                                                                                                     |
| ---------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 文件头                             | 加 `#include "bsp_series.hpp"`                                                                                                                                                         |
| `:19` `ICBSP_DMA_BUFFER(name)` | 套 `#if BSP_SERIES_F4` → `#define ICBSP_DMA_BUFFER(name) /* F4：主 SRAM 可直接 DMA，不需要专段 */`；`#else` 保留原 `__attribute__((section(".dma_buffer." #name)))`；`#endif` |
| 文件头注释                         | 补一段：F4 上 `.dma_buffer` 段**彻底不生成**，链接脚本不用加、`system.cpp` 的早拷贝自动跳过（weak 符号守卫已有）                                                               |

**连带收益**：`board_cfg.hpp` 里 `inline ICBSP_DMA_BUFFER(bsp_uart1) BspUartT<...>` 一个字都不用改，在 F4 上自然展开成普通全局变量。

### 4.5 `bsp_can.hpp` —— 内联 bxCAN 实现，不新开文件

| 位置                          | 改动                                                                                                |
| ----------------------------- | --------------------------------------------------------------------------------------------------- |
| `:30`                       | include → `bsp_series.hpp`                                                                       |
| `:33`                       | 保留 `#ifdef HAL_FDCAN_MODULE_ENABLED`（H7 全部内容不动）                                         |
| 文件末尾 `#endif`（:887）前 | 插入 `#elif defined(HAL_CAN_MODULE_ENABLED)` 分支，内联 bxCAN 版 `BspCanT<RX, TX>`（约 450 行） |

bxCAN 版必须提供的公开接口（与 FDCAN 版逐一对齐）：

```
Config{.hcan, .name}
init / deinit / is_ready / handle
send / send_latest / receive / read_raw / return_frame
tx_recover / service_recovery
process_fifo0_isr / trigger_tx_from_isr / process_error_isr
diagnostics
```

FDCAN → bxCAN 映射：

| FDCAN                                       | bxCAN                                                     | 说明                                                                                       |
| ------------------------------------------- | --------------------------------------------------------- | ------------------------------------------------------------------------------------------ |
| TX FIFO + `HAL_FDCAN_AddMessageToTxFifoQ` | 3 个发送邮箱 + `HAL_CAN_AddTxMessage`                   | HAL 自动挑空邮箱                                                                           |
| `FDCAN_IT_TX_FIFO_EMPTY` 续发             | `CAN_IT_TX_MAILBOX_EMPTY`                               | 邮箱空出来就续发软件缓冲                                                                   |
| `send_latest()`（覆盖最新槽位）           | 无硬件对应                                                | **退化为 `send()`**，`diagnostics` 增加 `latest_degraded` 计数（已确认可接受） |
| RX FIFO0 中断                               | `CAN_RX_FIFO0` 中断                                     | `HAL_CAN_GetRxMessage(..., CAN_RX_FIFO0, ...)`                                           |
| `ConfigGlobalFilter` 拒收远程帧           | `HAL_CAN_ConfigFilter` + `CAN_FILTERMODE_IDMASK` 全通 | filter bank 0，`FilterFIFOAssignment = CAN_RX_FIFO0`                                     |
| `CCCR.INIT` 清位恢复 Bus-Off              | `CAN_MCR_INRQ` 清位 + `HAL_CAN_Start()`               | `CAN_ESR_BOFF` 判 Bus-Off                                                                |
| `PSR.EP` / `PSR.EW`                     | `CAN_ESR_EPV` / `CAN_ESR_EWGF`                        | 只计数                                                                                     |

### 4.6 `bsp_can.cpp` —— 内联 bxCAN 回调，不新开文件

| 位置                          | 改动                                                                                                                                                                                                          |
| ----------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `:1`                        | include → `bsp_series.hpp`                                                                                                                                                                                 |
| `:4`                        | 保留 `#ifdef HAL_FDCAN_MODULE_ENABLED`（H7 回调不动）                                                                                                                                                       |
| 文件末尾 `#endif`（:119）前 | 插入 `#elif defined(HAL_CAN_MODULE_ENABLED)` 分支：`bsp_bxcan_is_listed()`、`HAL_CAN_RxFifo0MsgPendingCallback`、`HAL_CAN_TxMailbox0/1/2CompleteCallback`、`HAL_CAN_ErrorCallback` 三个清单外弱函数 |

### 4.7 其余 bsp 文件（只换 include，零逻辑改动）

`bsp_gpio.hpp`、`bsp_key.hpp`、`bsp_encoder.hpp/.cpp`、`bsp_pwm.hpp/.cpp`、`bsp_uart.cpp`、`bsp_dwt.hpp`

> `bsp_pwm.cpp` 用的 `IS_TIM_32B_COUNTER_INSTANCE` / `IS_TIM_CCX_INSTANCE` 在 F4 HAL 里都有，无需处理。

---

## 5. service / tasks / device 层

只有一类改动：CAN 开关从「FDCAN」放宽到「FDCAN 或 CAN」。

| 文件:行                            | 改动                                                                                              |
| ---------------------------------- | ------------------------------------------------------------------------------------------------- |
| `service/system.cpp:12, 97, 105` | `#if defined(HAL_FDCAN_MODULE_ENABLED) && ...` → `#if BSP_CAN_MODULE_ENABLED && ...`         |
| `service/can_rx_node.hpp:8`      | 同上                                                                                              |
| `service/can_tx_node.hpp:8`      | 同上                                                                                              |
| `device/dji_motor.hpp:20`        | 同上                                                                                              |
| `tasks/can_rx_task.cpp:5`        | 同上                                                                                              |
| `tasks/can_tx_task.cpp:5`        | 同上                                                                                              |
| `tasks/sys_task.cpp:60`          | `#  if defined(HAL_FDCAN_MODULE_ENABLED) && (...)` → `#  if BSP_CAN_MODULE_ENABLED && (...)` |
| `service/fault_record.cpp:4`     | include → `bsp_series.hpp`；`:98` ECC 挤出逻辑保留，注释补「F4 无 ECC，此处只起屏障作用」    |
| `device/*.cpp / *.hpp`（12 个）  | 只换 include                                                                                      |

---

## 6. 模板与工具链

### 6.1 `templates/user/board/`

| 文件                                        | 改动                                                                                                                        |
| ------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| `hardware_config.h:33`                    | include → `bsp_series.hpp`                                                                                               |
| `hardware_config.h:48`                    | `#ifndef HAL_FDCAN_MODULE_ENABLED` → `#if !BSP_CAN_MODULE_ENABLED`                                                     |
| `hardware_config.h:100`                   | `UART_ENABLE_FIFO` 默认 **1 → 0**；并加 `#if BSP_UART_HAS_FIFO == 0` 时 `static_assert(UART_ENABLE_FIFO == 0)` |
| `board_cfg.hpp:68-72`                     | `#include "fdcan.h"` 改成条件编译：H7 用 `fdcan.h`，F4 用 `can.h`                                                     |
| `board_cfg.hpp:92-95`                     | CAN1 实例加注释：H7 写 `{.hfdcan = &hfdcan1}`，F4 写 `{.hcan = &hcan1}`                                                 |
| `board_cfg.hpp:114-120`                   | `timer_clk_hz = 275000000UL` 加注释：F4 按 TIM 时钟填（APB2 定时器 168 MHz）                                              |
| `board_cfg.cpp` / `board_instances.hpp` | 无改动                                                                                                                      |

### 6.2 `CMakeLists.txt`

| 位置                     | 改动                                                                                                                                           |
| ------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| `:84-135` 链接脚本检查 | 外面加「仅 H7 执行」条件（`file(GLOB "${CMAKE_SOURCE_DIR}/Core/Inc/stm32h7xx_hal_conf.h")` 非空才做）                                        |
| 新增 F4 分支             | 只检查 `.noinit` 段（`fault_record` 仍需要），跳过 `.dma_buffer` 的全部检查（`AT>` / `NOLOAD` / 2 空格缩进 / `__dma_buffer_load`） |

### 6.3 `templates/linker_sections.ld`

| 改动                                                                                                                   |
| ---------------------------------------------------------------------------------------------------------------------- |
| 顶部注释补一段：F4 只需 `.noinit` 段；`.dma_buffer` 在 F4 上不生成                                                 |
| **不新增文件**：`integrate.py` 用现有正则按段名切块，只把 `.noinit` 那块插进 F4 链接脚本，区名替换成 `RAM` |

### 6.4 `tools/integrate.py`

| 位置         | 改动                                                                                                                                             |
| ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------ |
| `:9`       | 文档串 `STM32H7` → `STM32H7 / F4`                                                                                                           |
| `:215`     | `Core/Inc/stm32h7xx_hal_conf.h` → glob `stm32h7xx_hal_conf.h` / `stm32f4xx_hal_conf.h`，从文件名取系列；其他系列报错「只支持 H7 / F4」    |
| `:244`     | `stm32h7xx_hal_timebase_tim.c` → 按探测到的系列拼名                                                                                           |
| `:256-257` | `AXI_SRAM_ORIGIN` / `DMA_UNREACHABLE` → 按系列给值：H7 `0x24000000` + ITCM/DTCM；F4 `0x20000000` + CCM `0x10000000~0x10010000`        |
| `:279-330` | `.dma_buffer` 检查在 F4 上整段跳过；`.noinit` 保留；`>region` 模板按系列选 `RAM_D1` / `RAM`（F4 优先按 `0x20000000` 找区，不按名字） |
| `:913-990` | SVD 分组 / `DEVICE` / `OPENOCD_TARGET` / J-Link device 推断增加 F4 分支（`STM32F4xx` / `stm32f4x.cfg`）                                  |

### 6.5 `templates/Flash/` 与 `templates/vscode/`

| 文件                                                    | 改动                                                                                                  |
| ------------------------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| `Flash/*.cfg`（4 个，**不新增文件**）           | 把写死的 H7 OpenOCD target 改成占位，由 `integrate.py` 按系列填 `stm32h7x.cfg` / `stm32f4x.cfg` |
| `vscode/launch.json` `settings.json` `tasks.json` | device / SVD / cmsis-pack 名改占位，由 `integrate.py` 填                                            |

### 6.6 文档

| 文件:行                 | 改动                                                                                    |
| ----------------------- | --------------------------------------------------------------------------------------- |
| `docs/接入指南.md:5`  | 「接进一个 STM32H7 CubeMX 工程」→ 「STM32H7 / STM32F4」                                |
| `docs/接入指南.md:18` | 芯片表格扩成两行：H7 / F4，分别写 HAL conf 文件名、DMA 可达区、是否需要 `.dma_buffer` |
| `docs/code/bsp.md`    | 补一节「F4 差异：无 `.dma_buffer` 段、无 UART FIFO、bxCAN 后端」                      |

---

## 7. 执行顺序与验收

| 步骤        | 内容                                                                                                                                      | 验收标准                                                                                                                                                       |
| ----------- | ----------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **1** | 新增 `bsp_series.hpp` + 31 处 include 替换                                                                                              | 用当前 F4 工程对 `bsp_uart.hpp` / `bsp_spi.hpp` / `bsp_can.hpp` / `bsp_dwt.cpp` 做 `arm-none-eabi-g++ -fsyntax-only`，只剩 CAN 与 UART-FIFO 相关错误 |
| **2** | `bsp_uart.hpp` 2 处 `#if` + 2 处函数体、`bsp_spi.cpp` 1 处函数体、`bsp_dwt.cpp` 1 处 `#if`、`bsp_dma_buffer.hpp` 1 处 `#if` | 上述文件语法检查全部通过                                                                                                                                       |
| **3** | `CMakeLists.txt` 按系列跳过 `.dma_buffer` 检查                                                                                        | F4 上 `ICBSP_DMA_BUFFER` 展开为空，链接脚本只需加 `.noinit`                                                                                                |
| **4** | `bsp_can.hpp/.cpp` 的 `#elif` 分支（bxCAN 内联）+ 9 处开关宏                                                                          | F4 语法检查通过；H7 侧零改动                                                                                                                                   |
| **5** | `templates/` + `integrate.py` + 文档                                                                                                  | F4 空白 CubeMX 工程跑 `integrate.py` 一次通过                                                                                                                |
| **6** | 实机验证（需要硬件）                                                                                                                      | UART DMA 收发（115200 / 1 Mbps / 2 Mbps）、BMI088 SPI-DMA 读通、DJI 电机 CAN 收发、Bus-Off 自恢复、`fault_record` 复位后可读、`sys_task` 巡检正常          |

---

## 8. 已确认的决策

| # | 事项                                | 结论                                                                     |
| - | ----------------------------------- | ------------------------------------------------------------------------ |
| 1 | 适配范围                            | **只 H7 + F4**，G4/F1 不做                                         |
| 2 | `bsp_dma_buffer.hpp`              | 不新开文件、不引入开关宏，**在原文件里直接 `#if BSP_SERIES_F4`** |
| 3 | `.dma_buffer` 段                  | **F4 上彻底去掉**（主 SRAM 本身可 DMA，没必要）                    |
| 4 | `UART_ENABLE_FIFO`                | 模板默认改 **0**；非 FIFO 系列编译期强制 0                         |
| 5 | `send_latest()`                   | bxCAN 上退化为 `send()`，加 `latest_degraded` 计数                   |
| 6 | 新增文件                            | **只有 `bsp_series.hpp`**；bxCAN 实现内联进 `bsp_can.hpp/.cpp` |
| 7 | 浮点精度（`double` → `float`） | 本次不处理                                                               |

---

## 9. 改动统计预估

| 类别               | 数量                                                               |
| ------------------ | ------------------------------------------------------------------ |
| 新增文件           | **1**（`code/bsp/bsp_series.hpp`）                         |
| include 替换       | 31 处（纯机械）                                                    |
| `#if` 条件编译点 | BSP 层 5 处 + service/tasks 层 9 处 + 模板 4 处                    |
| 现有代码删除       | **0 行**                                                     |
| 新增代码           | 约 550~650 行（bxCAN 后端占 450~550，内联在 `bsp_can.hpp/.cpp`） |
