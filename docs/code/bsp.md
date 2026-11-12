# bsp/ — 外设驱动层

[← 代码层索引](README.md) · [service](service.md) · [device](device.md)

**职责**：一个外设「怎么初始化、怎么收发、出错怎么恢复」。
**不负责**：帧格式（→ `device/motor/emmv5_protocol`）、设备语义（→ `device/`）、重连与调度策略（→ `service/`、`tasks/`）。

---

## 1. 文件清单

| 文件 | 职责 | 关键符号 |
| --- | --- | --- |
| `bsp_can.hpp/.cpp` | CAN 入口：H7 转给 `bsp_fdcan`；F4 在这里实现 bxCAN（回调在 `bsp_can.cpp`） | `BspCanT<RX, TX>`、`CanRxMsg`、`CanTxMsg` |
| `bsp_fdcan.hpp/.cpp` | H7 的 FDCAN（经典 CAN 模式）：收发缓冲、Bus-Off 恢复、错误状态、诊断计数；回调在 `.cpp` | 同上 |
| `bsp_uart.hpp/.cpp` | 串口：IDLE 断帧 + DMA/中断收发 + FreeRTOS 流缓冲 | `BspUartT<Tp, RX, TX>`、`UartDir`、`uart_mode::dma/it` |
| `bsp_spi.hpp/.cpp` | SPI 主机 + 片选设备封装 | `BspSpi`、`BspSpiDevice` |
| `bsp_pwm.hpp/.cpp` | PWM 输出（占空比 / 脉宽 / 频率） | `BspPwm` |
| `bsp_gpio.hpp` | GPIO 读写（纯头文件，`constexpr` 构造） | `BspGpio` |
| `bsp_key.hpp/.cpp` | 按键消抖 + 中断 | `BspKey` |
| `bsp_encoder.hpp/.cpp` | 编码器（定时器编码器模式）增量读取 | `BspEncoder` |
| `bsp_dwt.hpp/.cpp` | DWT 计时：64 位时间轴、微秒 / 纳秒延时、周期耗时统计 | `BspDwt`、`bsp_dwt` |
| `bsp_series.hpp` | 库内唯一包含 HAL 的地方：按系列选 `stm32h7xx_hal.h` / `stm32f4xx_hal.h`，把 H7 / F4 的命名差异收成宏 | `ICBSP_SERIES_H7` / `ICBSP_SERIES_F4` |
| `bsp_dma_buffer.hpp` | `.dma_buffer` 段宏与 DMA 可达性判断 | `ICBSP_DMA_BUFFER(name)`、`icbsp_dma_reachable()` |

---

## 2. 三条通用规则

### 2.1 实例放哪

实例**不在库里**。构造是 constexpr 的（`BspCanT`、`BspUartT`、`BspSpi`、`BspPwm`、`BspGpio`）在宿主 `board_cfg.hpp` 里写成 `inline` 全局变量（C++20），
不是 constexpr 的（`BspSpiDevice` 等）定义在 `board_cfg.cpp`、头文件里 `extern`。库通过 `board_instances.hpp` 的 X-macro 拿到 CAN / UART / SPI 实例的名字。
这样库不含任何板级信息，换板只改宿主。

### 2.2 `init()` 一律返回 `Status`

```cpp
ICBSP_CHECK(bsp_can1.init() == Status::OK);
```

失败不要打印后继续：`board_init()` 里每个 `init()` 单独一行 `ICBSP_CHECK`（`service/system/fault_record.hpp`，总会求值），
失败时把文件 / 行号写进 `fault_record` 再停机，比静默半初始化好查得多。不要写成 `configASSERT(x.init() == …)`：断言可能被定义成空，`init()` 就不调了。

### 2.3 `.dma_buffer` 段

DMA 用到的实例（`BspUartT` 在 dma 模式、`BspSpi` 开 DMA）必须放 `.dma_buffer`：

```cpp
inline ICBSP_DMA_BUFFER(bsp_uart1) BspUartT<uart_mode::dma, 128, 128> bsp_uart1(...);
```

原因：DMA1/DMA2 访问不到 ITCM/DTCM，而 H723 等型号的 CubeMX 链接脚本把 `.data` / `.bss` 放在 DTCM；库统一把 DMA 对象放进 AXI SRAM（D1，DMA1/DMA2 可达）。段必须是**可加载**的
（`>RAM_D1 AT> FLASH`），框架在全局构造之前把初值从 Flash 拷进 RAM；写成 `NOLOAD`
会让句柄、波特率、DMA 配置上电变随机值。CMake 配置阶段会检查这一点。

---

## 3. 各驱动的关键接口

### BspCanT&lt;RX_DEPTH, TX_DEPTH&gt;

```cpp
inline BspCanT<16, 16> bsp_can1({.hfdcan = &hfdcan1, .name = "CAN1"});
```

| 方法 | 说明 |
| --- | --- |
| `Status init()` / `void deinit()` | 停外设、清缓冲、按 CubeMX 配置重新启动（可重复调用，失败回滚）；实例不在 `ICBSP_CAN_INSTANCES` 里或句柄重复 → `BAD_ARG` |
| `bool is_ready() const` | 构造时是否给了句柄 |
| `Status send(uint32_t id, const uint8_t *data, uint8_t dlc = 8, uint32_t id_type = FDCAN_STANDARD_ID)` | 标准帧 / 扩展帧，DLC 0~8；非阻塞，**只能在任务里调用** |
| `Status receive(CanRxMsg *msg, uint32_t timeout_ms)` | `ICBSP_CAN_RX_ENABLE = 1`：取没被节点认领的帧（回退缓冲，`ICBSP_CAN_RX_RETURN_FRAMES = 0` 时返回 `NOT_SUPPORTED`）；`= 0`：取所有收到的帧 |
| `Status send_latest(...)` | 参数同 `send()`；同 ID 的上一帧还在驱动里时不入队、返回 `BUSY`（周期帧用，发送节点内部就用它） |
| `Status read_raw(...)` / `bool return_frame(...)` | `can_rx_task` 专用 |
| `Status service_recovery()` / `bool tx_recover()` | `sys_task` 每 10 ms 调用：Bus-Off 恢复 / 发送兜底 |

- `CanRxMsg` = `FDCAN_RxHeaderTypeDef header` + `uint8_t data[8]`（DLC 之后的字节为 0）；`CanTxMsg` = `id` + `id_type` + `dlc` + `data[8]`。
- 只有头文件。H7 的实现在 `bsp_fdcan.hpp`、HAL 回调在 `bsp_fdcan.cpp`；F4（bxCAN）的实现在 `bsp_can.hpp` 的 `#elif` 分支、回调在 `bsp_can.cpp`，Config 的句柄字段是 `hcan`（F4 分支只编译验证过）。H7 下库占用 `HAL_FDCAN_RxFifo0Callback`、`HAL_FDCAN_TxFifoEmptyCallback`、`HAL_FDCAN_ErrorStatusCallback`，宿主不能再定义；
  不在 `ICBSP_CAN_INSTANCES` 里的 FDCAN 的这三个回调转到弱函数 `icbsp_unhandled_fdcan_*()`，宿主定义同名函数即可接收（见下文“清单外外设的回调”）。
- 远程帧由硬件拒收；DLC > 8 的帧丢弃并计入 `diagnostics.rx_len_drop`。
- TX 队列模式（CubeMX `TxFifoQueueMode = Queue`）下，同一 ID 的帧保证按发送顺序上线（硬件里已有同 ID 的帧时，下一帧先在软件里等）。
- **总线上有更高优先级的流量、可能被占满时用 Queue 模式**：FIFO 模式严格按入队顺序发，排在前面的低优先级帧发不出去时，后面更高优先级的帧也只能等；Queue 模式按 ID 优先级发，配合发送节点的 `send_latest()`（一个周期 ID 最多占一个硬件缓冲），高优先级帧总能先发（实测 C06c / C06d）。
- 诊断：`diagnostics` 的各项计数（`rx_unclaimed`、`rx_return_dropped`、`tx_buf_full`、`tx_stall_recover`、`bus_off_events` 等），含义见 `bsp_can.hpp`。

**队列长度怎么估**：`RX_DEPTH` 是每路 CAN 的接收队列帧数，ISR 写、`can_rx_task` 每 1 ms 取走。
一条总线上挂 N 个设备、每帧 1 ms 内到达时，16 帧约够 8 个设备同时回包；挂满就加到 32/48。

### BspUartT&lt;Transport, RX_SIZE, TX_SIZE&gt;

```cpp
inline ICBSP_DMA_BUFFER(bsp_uart1) BspUartT<uart_mode::dma, 128, 128> bsp_uart1(
    {.huart = &huart1, .mode = UartDir::TxRx, .baudrate = 115200U});
```

- `Transport`：`uart_mode::dma`（收发都走 DMA）或 `uart_mode::it`（中断）
- `RX_SIZE` / `TX_SIZE`：环形缓冲字节数，按协议一帧大小 × 几帧估
- `UartDir::RxOnly` / `TxOnly` / `TxRx`：只收的设备不占用 TX DMA

| 方法 | 说明 |
| --- | --- |
| `Status send(const uint8_t*, size_t, timeout, size_t *written)` | 阻塞到写进流缓冲为止 |
| `Status printf(const char *fmt, ...)` | 调试打印；浮点需要宿主加 `-u _printf_float` |
| `Status receive(uint8_t*, size_t, timeout, size_t *received)` | 读字节流（不保证整帧，切帧是协议层的事） |
| `bool tx_recover()` / `bool rx_recover()` | 传输卡死后的补救，`sys_task` 调用 |
| `size_t get_tx_free_space()` / `get_rx_available_data()` | 诊断用 |

- `BspUartT` 只有头文件，`bsp_uart.cpp` 里只有 HAL 回调分发，任意 Transport / 缓冲尺寸都不用补显式实例化。
- `uart_mode::dma` 的接收是**循环 DMA**：`init()` 把 `hdmarx` 改成 Circular + 直接模式（CubeMX 里配 Normal 也可以），
  IDLE / 半满 / 写满事件只报告 DMA 写到的位置，接收路径不调用阻塞版 `HAL_DMA_Abort()`。接收流缓冲区是 `2 × RX_SIZE`。
- `init()` 核对实例在 `ICBSP_UART_INSTANCES` 清单里、且是清单里唯一绑该句柄的实例，否则返回 `BAD_ARG`；
  DMA 模式下还要求配了用到的 RX / TX DMA、实例在 DMA 可达的内存里（`.dma_buffer`）。
- RS485 只支持 UART 硬件 DE（CubeMX 里选 RS485 模式），库不提供软件 DE。
- `receive()` **单读者**：同一个串口只能由一个任务读（FreeRTOS 流缓冲区的限制）；`send()` / `printf()` 多写者安全。
- `RxDiag` 里 `pe_cnt` / `fe_cnt` / `ne_cnt` / `ore_cnt` 分别计数线路错误：DMA 模式下 FE / NE / PE 不会中止接收，坏字节照常进流缓冲区，只能靠它们发现（评审 F7）。
- `printf()`：取锁超时返回 `TIMEOUT`，空串返回 `OK`；`tx_recover()` 在 `Config::baudrate = 0` 时按 CubeMX 的波特率估算卡死阈值。
- **循环接收的前提**：串口 / RX DMA 中断被挡住（临界区、更高优先级中断）的时间要短于收满半个 `RX_SIZE` 的时间（`RX_SIZE / 2 × 10 / 波特率`，128 字节、921600 下约 0.69 ms）。超过时 DMA 可能写满一圈、覆盖还没投递的数据，库的计数察觉不到（阶段 3 U19 实测）。高波特率或有长临界区时加大 `RX_SIZE`。
- `TxDiag`：`stall_recover_cnt` 是巡检恢复发送的总次数；其中 `idle_kick_cnt` 是“通道空闲、却有数据没人启动”由巡检补发的次数（漏唤醒，正常应为 0），`idle_kick_uart_err` / `idle_kick_dma_err` 是当时 HAL ErrorCode 的快照。
- DMA 可达性按 DMA 控制器判断（`icbsp_dma_reachable()`，`bsp_dma_buffer.hpp`）：DMA1 / DMA2 访问不到 ITCM / DTCM；
  BDMA（LPUART1、SPI6 等）只能访问 SRAM4，`.dma_buffer` 在 AXI SRAM，这些外设的实例 `init()` 会返回 `BAD_ARG`（评审 A4）。

### BspSpi / BspSpiDevice

```cpp
inline ICBSP_DMA_BUFFER(bsp_spi2) BspSpi bsp_spi2(
    {.hspi = &hspi2, .use_dma = true, .dma_threshold = 16U});

// board_cfg.cpp（BspSpiDevice 的构造不是 constexpr）
BspSpiDevice bmi088_acc_spi(bsp_spi2,
    {.cs_port = ACC_CS_GPIO_Port, .cs_pin = ACC_CS_Pin, .read_flag = 0x80U, .read_dummy = 1U});
```

- `BspSpi` 管总线：`lock()/unlock()` 互斥、`transfer_locked()` 传输；引脚 NSS 必须是**软件**模式
- `BspSpi` 是 constexpr 构造、平凡析构，和 CAN / UART 一样写成 `board_cfg.hpp` 的 inline 实例；`init()` 可重复调用（先 `deinit()` 再重建，
  前提是没有任务正在用这条总线），`deinit()` 中止进行中的传输并释放资源。`init()` 核对实例在 `ICBSP_SPI_INSTANCES` 里、且清单里只有它绑这个句柄，否则 `BAD_ARG`
- `BspSpiDevice` 管片选与寄存器语义：`read_reg()` / `write_reg()` / `read_reg8()` / `write_reg8()`，
  内部做 `select → 发命令 → dummy → 读 → deselect`
- `.read_flag`：读命令要或上的位（BMI088 是 `0x80`）；`.read_dummy`：读之前要先发的空字节数

### 清单外外设的回调

库占用了 UART / SPI / FDCAN 的若干 HAL 回调（宿主不能再定义它们）。宿主自己用 HAL 管、不在 `ICBSP_*_INSTANCES` 清单里的外设，
这些回调转到下面的弱函数（默认什么也不做），宿主定义同名强函数即可：

| 库占用的回调 | 清单外转到 |
| --- | --- |
| `HAL_UARTEx_RxEventCallback` / `HAL_UART_TxCpltCallback` / `HAL_UART_ErrorCallback` | `icbsp_unhandled_uart_rx_event(huart, size)` / `icbsp_unhandled_uart_tx_cplt(huart)` / `icbsp_unhandled_uart_error(huart)` |
| `HAL_SPI_TxRxCpltCallback` / `HAL_SPI_ErrorCallback` | `icbsp_unhandled_spi_txrx_cplt(hspi)` / `icbsp_unhandled_spi_error(hspi)` |
| `HAL_FDCAN_RxFifo0Callback` / `HAL_FDCAN_TxFifoEmptyCallback` / `HAL_FDCAN_ErrorStatusCallback` | `icbsp_unhandled_fdcan_rx_fifo0(h, its)` / `icbsp_unhandled_fdcan_tx_fifo_empty(h)` / `icbsp_unhandled_fdcan_error_status(h, its)` |

它们都声明在对应的 `bsp_*.hpp` 里（`extern "C"`）。库没占用的回调（例如 `HAL_UART_RxCpltCallback`、`HAL_SPI_TxCpltCallback`）宿主照常定义。

### BspPwm

```cpp
inline BspPwm bsp_pwm_buzzer(
    {.htim = &htim12, .channel = TIM_CHANNEL_2,
     .timer_clk_hz = 275000000UL, .prescaler = 23U, .period = 1999U});
```

频率 = `timer_clk_hz / (prescaler + 1) / (period + 1)`。`timer_clk_hz` 要填 CubeMX 时钟树里
这个定时器的**输入时钟**（APB 定时器时钟），填错音调/舵机时序就偏。

`set_duty(pct)`、`set_pulse_us(us)`、`set_freq(hz)`、`off()`、`get_freq_hz()`。

- `init()` 打开 ARR 预装载（ARPE）：`set_freq()` 改的 ARR 在当前周期结束时才生效，不会因为新 ARR 小于当前计数值而多出一个超长周期（蜂鸣器爆音、舵机超长脉冲，评审 F10）。
- `set_freq()` 返回 `Status`：`OK`、`NOT_SUPPORTED`（超出范围，已按边界生效，实际频率看 `get_freq_hz()`）、`BAD_ARG`（0 Hz）、`NOT_INIT`。
- 32 位定时器（TIM2 / 5 / 23 / 24）ARR = 0xFFFFFFFF 时周期计数是 2^32，`get_period_cnt()` 返回 `uint64_t`。
- 没有默认构造（必须给 Config）。

### BspGpio / BspKey / BspEncoder

| 类 | 构造 | 常用 |
| --- | --- | --- |
| `BspGpio` | `{port, pin}`，`constexpr` | `set() / reset() / toggle() / set_level(bool) / get_level()` |
| `BspKey` | `{port, pin, active_low, ...}` | `init()` 开中断，`is_pressed()` 读稳定态 |
| `BspEncoder` | `{htim, ...}` | `init()`，`read_delta()` 取本周期增量，`get_position()` 累计值，`reset()` 清零；增量按计数器位宽回绕算，CubeMX 的 Period 必须是计数器最大值：16 位定时器 0xFFFF、32 位定时器 0xFFFFFFFF，否则 `init()` 返回 `BAD_ARG`（N37） |

### BspDwt

```cpp
bsp_dwt.get_delta_s(&last);   // 实例在库内，system_init() 已经 init() 过
```

实例由库定义（`code/service/system/system.cpp`），`init()` 在 `system_init()` 里调用，宿主**不要**再定义或 `init()`。
CPU 频率由 `init()` 从 RCC 自动推算，**没有配置项**，全局一个实例就够。

| 方法 | 说明 |
| --- | --- |
| `delay_us(double)` / `delay_ns` / `delay_ms` / `delay_s` | 忙等延时 |
| `double get_delta_s(uint32_t *last)` | 传入上次时间戳，返回间隔秒数（`sys_task` 用它算循环耗时） |
| `uint64_t update_timeline()` | 刷新 64 位时间轴并返回当前计数 |

`get_delta_s()` 的时间戳由调用方保存，**同一个 `last` 不要跨任务共用**。

---

## 4. 实例清单（X-macro）

库没有运行时登记表，靠宿主 `board_instances.hpp` 在编译期展开：

```cpp
#define ICBSP_CAN_INSTANCES(X) X(bsp_can1)          // 中断里按这份清单分发
#define ICBSP_UART_INSTANCES(X) X(bsp_uart1)        // sys_task 串口巡检按此展开
#define ICBSP_SPI_INSTANCES(X) /* 无 */
```

展开的地方：`bsp_fdcan.cpp` / `bsp_uart.cpp` / `bsp_spi.cpp` 的 HAL 回调分发与“是否在清单里”的核对，`can_rx_task` / `can_tx_task` 逐总线处理，
`sys_task` 的 Bus-Off 恢复与串口巡检，`state_tree` 给每个实例建总线节点。

**漏写会被 `init()` 拒绝**：实例不在清单里、或清单里两个实例绑同一句柄时，`init()` 返回 `BAD_ARG`（`board_init()` 的 `ICBSP_CHECK` 停机）。
加实例时 `board_cfg.hpp`、`board_instances.hpp`、`board_cfg.cpp`（`init()`）三处一起改。

---

## 5. 常见坑

| 现象 | 原因 |
| --- | --- |
| 串口参数上电变随机值 | `.dma_buffer` 写成 `NOLOAD`，或漏了 `AT> FLASH` |
| 串口 `init()` 返回 `BAD_ARG` | 实例没写进 `ICBSP_UART_INSTANCES` / 两个实例绑同一句柄 / DMA 模式没配 DMA / 实例不在 `.dma_buffer` |
| CAN 收不到帧 | FDCAN 中断没开 / 过滤器没放行 / 没有接收节点认领（进了回退缓冲，`diagnostics.rx_unclaimed` 在涨） |
| `init()` 返回 `BAD_ARG` | 实例没写进 `ICBSP_*_INSTANCES`，或清单里两个实例绑同一句柄 |
| 多个 SPI 设备互相踩数据 | 没用 `lock()` 包住整笔事务 |
| 浮点打印空或乱码 | 宿主 CMake 少了 `-u _printf_float`（nano.specs） |
| `get_delta_s()` 返回值离谱 | 两个任务共用了同一个 `last` |
| 蜂鸣器音调不对 | `timer_clk_hz` 与时钟树不一致 |
