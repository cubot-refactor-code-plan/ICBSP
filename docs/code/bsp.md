# bsp/ — 外设驱动层

[← 代码层索引](README.md) · [service](service.md) · [device](device.md)

**职责**：一个外设「怎么初始化、怎么收发、出错怎么恢复」。
**不负责**：帧格式（→ `protocol/`）、设备语义（→ `device/`）、重连与调度策略（→ `service/`、`tasks/`）。

---

## 1. 文件清单

| 文件 | 职责 | 关键符号 |
| --- | --- | --- |
| `bsp_can.hpp/.cpp` | 经典 CAN：收发队列、Bus-Off 恢复、错误中断 | `BspCanT<RX, TX>`、`CanRxMsg`、`CanTxMsg` |
| `bsp_uart.hpp/.cpp` | 串口：IDLE 断帧 + DMA/中断收发 + FreeRTOS 流缓冲 | `BspUartT<Tp, RX, TX>`、`UartDir`、`uart_mode::dma/it` |
| `bsp_spi.hpp/.cpp` | SPI 主机 + 片选设备封装 | `BspSpi`、`BspSpiDevice` |
| `bsp_pwm.hpp/.cpp` | PWM 输出（占空比 / 脉宽 / 频率） | `BspPwm` |
| `bsp_gpio.hpp` | GPIO 读写（纯头文件，`constexpr` 构造） | `BspGpio` |
| `bsp_key.hpp/.cpp` | 按键消抖 + 中断 | `BspKey` |
| `bsp_encoder.hpp/.cpp` | 编码器（定时器编码器模式）增量读取 | `BspEncoder` |
| `bsp_dwt.hpp/.cpp` | DWT 计时：微秒/纳秒延时、周期耗时统计 | `BspDwt` |

---

## 2. 三条通用规则

### 2.1 实例放哪

实例**不在库里**，都在宿主的 `user/board/board_cfg.hpp` 里用 `inline` 定义（C++20 inline 变量），
库里通过 `board_instances.hpp` 的 X-macro 拿到名字。这样库不含任何板级信息，换板只改宿主。

### 2.2 `init()` 一律返回 `Status`

```cpp
configASSERT(bsp_can1.init() == Status::OK);
```

失败不要在库里 `printf` 后继续，直接在 `board_init()` 里 `configASSERT` 停机：
`fault_record` 会记下调用行号，比静默半初始化好查得多。

### 2.3 `.dma_buffer` 段

DMA 用到的实例（`BspUartT` 在 dma 模式、`BspSpi` 开 DMA）必须放 `.dma_buffer`：

```cpp
inline __attribute__((section(".dma_buffer"))) BspUartT<uart_mode::dma, 128, 128> bsp_uart1(...);
```

原因：DMA1/DMA2 访问不到 ITCM/DTCM，只能访问 AXI SRAM（D1）。段必须是**可加载**的
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
| `Status init()` / `void deinit()` | 启动 FDCAN、开中断、建队列 |
| `bool is_ready() const` | 是否已初始化且未停机 |
| `Status send(uint32_t std_id, const uint8_t *data)` | 标准帧，8 字节 |
| `Status receive(CanRxMsg *msg, uint32_t timeout_ms)` | 从**回退缓冲**取未被节点认领的帧 |
| `Status read_raw(CanRxMsg *msg, uint32_t timeout_ms)` | 接收任务专用：不经过回退缓冲 |
| `bool return_frame(const CanRxMsg &msg)` | 认领失败时把帧还回去 |
| `Status service_recovery()` | `sys_task` 周期调用：Bus-Off 后重建 |
| `bool tx_recover()` | 发送卡死时强制恢复 |

`CanRxMsg` = `FDCAN_RxHeaderTypeDef header` + `uint8_t data[8]`；`CanTxMsg` = `std_id` + `data[8]`。

**队列长度怎么估**：`RX_DEPTH` 是每路 CAN 的接收队列帧数，ISR 写、`can_rx_task` 每 1 ms 取走。
一条总线上挂 N 个设备、每帧 1 ms 内到达时，16 帧约够 8 个设备同时回包；挂满就加到 32/48。

### BspUartT&lt;Transport, RX_SIZE, TX_SIZE&gt;

```cpp
inline __attribute__((section(".dma_buffer"))) BspUartT<uart_mode::dma, 128, 128> bsp_uart1(
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
- `init()` 核对实例在 `BSP_UART_INSTANCES` 清单里、且是清单里唯一绑该句柄的实例，否则返回 `BAD_ARG`；
  DMA 模式下还要求配了用到的 RX / TX DMA、实例在 DMA 可达的内存里（`.dma_buffer`）。
- RS485 只支持 UART 硬件 DE（CubeMX 里选 RS485 模式），库不提供软件 DE。

### BspSpi / BspSpiDevice

```cpp
inline __attribute__((section(".dma_buffer"))) BspSpi bsp_spi2(
    {.hspi = &hspi2, .use_dma = true, .dma_threshold = 16U});

inline BspSpiDevice bmi088_acc_spi(bsp_spi2,
    {.cs_port = GYRO_ACC_CS_GPIO_Port, .cs_pin = GYRO_ACC_CS_Pin, .read_flag = 0x80U, .read_dummy = 1U});
```

- `BspSpi` 管总线：`lock()/unlock()` 互斥、`transfer_locked()` 传输；引脚 NSS 必须是**软件**模式
- `BspSpiDevice` 管片选与寄存器语义：`read_reg()` / `write_reg()` / `read_reg8()` / `write_reg8()`，
  内部做 `select → 发命令 → dummy → 读 → deselect`
- `.read_flag`：读命令要或上的位（BMI088 是 `0x80`）；`.read_dummy`：读之前要先发的空字节数

### BspPwm

```cpp
inline BspPwm bsp_pwm_buzzer(
    {.htim = &htim12, .channel = TIM_CHANNEL_2,
     .timer_clk_hz = 275000000UL, .prescaler = 23U, .period = 1999U});
```

频率 = `timer_clk_hz / (prescaler + 1) / (period + 1)`。`timer_clk_hz` 要填 CubeMX 时钟树里
这个定时器的**输入时钟**（APB 定时器时钟），填错音调/舵机时序就偏。

`set_duty(pct)`、`set_pulse_us(us)`、`set_freq(hz)`、`off()`、`get_freq_hz()`。

### BspGpio / BspKey / BspEncoder

| 类 | 构造 | 常用 |
| --- | --- | --- |
| `BspGpio` | `{port, pin}`，`constexpr` | `set() / reset() / toggle() / set_level(bool) / get_level()` |
| `BspKey` | `{port, pin, active_low, ...}` | `init()` 开中断，`pressed()` 读稳定态 |
| `BspEncoder` | `{htim, ...}` | `init()`，`read_delta()` 取本周期增量，`position()` 累计值，`reset()` 清零 |

### BspDwt

```cpp
bsp_dwt.get_delta_s(&last);   // 实例在库内，system_init() 已经 init() 过
```

实例由库定义（`code/service/system.cpp`），`init()` 在 `system_init()` 里调用，宿主**不要**再定义或 `init()`。
CPU 频率由 `init()` 从 RCC 自动推算，**没有配置项**，全局一个实例就够。

| 方法 | 说明 |
| --- | --- |
| `delay_us(double)` / `delay_ns` / `delay_ms` / `delay_s` | 忙等延时 |
| `double get_delta_s(uint32_t *last)` | 传入上次时间戳，返回间隔秒数（`sys_task` 用它算循环耗时） |
| `uint32_t update_timeline()` | 刷新并返回当前计数 |

`get_delta_s()` 的时间戳由调用方保存，**同一个 `last` 不要跨任务共用**。

---

## 4. 实例清单（X-macro）

库没有运行时登记表，靠宿主 `board_instances.hpp` 在编译期展开：

```cpp
#define BSP_CAN_INSTANCES(X) X(bsp_can1)          // 中断里按这份清单分发
#define BSP_UART_INSTANCES(X) X(bsp_uart1)        // sys_task 串口巡检按此展开
#define BSP_SPI_INSTANCES(X) /* 无 */
```

`code/bsp/bsp_can.cpp` 里三段 `BSP_CAN_INSTANCES(X)` 分别展开成「中断分发」「发送节拍」「恢复」。

**漏写不会报错**：编译通过、运行也正常，只是那个实例的中断不会被分发、`sys_task` 也不巡检它。
表现为「数据收不到」。所以加实例时 `board_cfg.hpp` 与 `board_instances.hpp` 必须一起改。

---

## 5. 常见坑

| 现象 | 原因 |
| --- | --- |
| 串口参数上电变随机值 | `.dma_buffer` 写成 `NOLOAD`，或漏了 `AT> FLASH` |
| 串口 `init()` 返回 `BAD_ARG` | 实例没写进 `BSP_UART_INSTANCES` / 两个实例绑同一句柄 / DMA 模式没配 DMA / 实例不在 `.dma_buffer` |
| CAN 收不到帧 | FDCAN 中断没开 / 过滤器没放行 / 实例没写进 `BSP_CAN_INSTANCES` |
| 多个 SPI 设备互相踩数据 | 没用 `lock()` 包住整笔事务 |
| 浮点打印空或乱码 | 宿主 CMake 少了 `-u _printf_float`（nano.specs） |
| `get_delta_s()` 返回值离谱 | 两个任务共用了同一个 `last` |
| 蜂鸣器音调不对 | `timer_clk_hz` 与时钟树不一致 |
