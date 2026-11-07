# tasks/ — 库任务层

[← 代码层索引](README.md) · [service](service.md) · [bsp](bsp.md)

**职责**：三个常驻任务的任务体。
**不负责**：任务的创建（在 `code/service/system/system.cpp`）、周期/优先级/栈深的配置（在宿主 `user/board/hardware_config.h`）。

任务体是**薄**的：每个周期只做一次遍历，真正的逻辑都在 `service/` 与 `bsp/` 里。

---

## 1. 三个任务一览

| 任务 | 文件 | 周期 | 优先级宏 | 栈深宏 | 谁创建 | 创建时机 |
| --- | --- | --- | --- | --- | --- | --- |
| `sys_task` | `sys_task.cpp` | **10 ms**（`SYS_TASK_PERIOD_MS`） | `ICBSP_SYS_TASK_PRIORITY`（默认 `configMAX_PRIORITIES − 4`） | `ICBSP_SYS_TASK_STACK`（256 字） | `system_init()` | `ICBSP_SYS_TASK_ENABLE = 1` |
| `can_rx_task` | `can_rx_task.cpp` | **1 ms** | `ICBSP_CAN_TASK_PRIORITY`（默认 `configMAX_PRIORITIES − 2`） | `ICBSP_CAN_TASK_STACK`（512 字） | `system_init()` | 开了 `HAL_FDCAN_MODULE_ENABLED` 且 `ICBSP_CAN_RX_ENABLE = 1` |
| `can_tx_task` | `can_tx_task.cpp` | **1 ms** | `ICBSP_CAN_TASK_PRIORITY` | `ICBSP_CAN_TASK_STACK` | `system_start()` | `ICBSP_CAN_TX_ENABLE = 1` 且板级**有发送节点** |

三个任务里的 CAN 部分目前只在 H7（`HAL_FDCAN_MODULE_ENABLED`）下编译；F4 的 bxCAN 驱动已有，但任务和节点还没接上。

创建代码在 `code/service/system/system.cpp`：

```cpp
system_init():   xTaskCreate(sys_task,     "sys",    ICBSP_SYS_TASK_STACK, NULL, ICBSP_SYS_TASK_PRIORITY, NULL);
                 xTaskCreate(can_rx_task,  "can_rx", ICBSP_CAN_TASK_STACK, NULL, ICBSP_CAN_TASK_PRIORITY, NULL);

system_start():  if (has_tx_nodes()) xTaskCreate(can_tx_task, "can_tx", ICBSP_CAN_TASK_STACK, NULL, ICBSP_CAN_TASK_PRIORITY, NULL);
```

`can_rx_task` / `can_tx_task` 进循环后第一句都是 `sys_state.wait_running()`：
初始化没完成或失败，它们不会碰硬件。

---

## 2. sys_task：系统维护（10 ms）

```cpp
static_assert(configTICK_RATE_HZ == 1000U, "sys_task requires a 1 kHz FreeRTOS tick");
```

每个周期按顺序做下面几件事，总线部分靠 X-macro 遍历板级实例，**没有运行时登记表**：

```cpp
   bsp_dwt.get_cycles();         // ① DWT 64 位时间轴的回绕跟踪（两次调用要短于 7.8 s）

#  define X(name) (void)name.service_recovery(); (void)name.tx_recover();
   ICBSP_CAN_INSTANCES(X)        // ② CAN：Bus-Off 自恢复 + 发送兜底（TX-FIFO-EMPTY 中断关着却有帧时补发，N27）

#  define X(name) (void)name.tx_recover(); (void)name.rx_recover();
   ICBSP_UART_INSTANCES(X)       // ③ 串口：卡死的发送续发、停摆的接收重建（ICBSP_UART_PATROL_ENABLE）

   Online::update();             // ④ 在线计时：超过 timeout_gap 判离线，关联了设备节点的记 TIMEOUT
   state_tree_poll();            // ⑤ 默认树：总线实例的诊断计数涨了就在总线节点上记错
```

另外用 DWT 统计本轮耗时，写进全局 `sys_task_stats`（`cycles` / `last_us` / `avg_us`（最近 1 s）/ `max_us`），
调试器里直接读。库本身**不打印**，也不引用宿主的任何宏（对比评估 N14）；要打印就由宿主接回调：

```cpp
system_set_report_hook(app_sys_report, 5000U);   // 每 5 s 在 sys_task 里调一次，参数是统计快照
```

回调跑在 `sys_task` 的栈上：打印要留够 vsnprintf 的栈，也不要阻塞太久（会推迟 CAN 恢复与串口巡检）。
---

## 3. can_rx_task：按 ID 分发（1 ms）

```cpp
for (;;)
{
#  define X(name) can_rx_service(name);
   ICBSP_CAN_INSTANCES(X)
   vTaskDelayUntil(&wake_time, pdMS_TO_TICKS(1U));
}
```

`can_rx_service()` 每个 tick 最多取 `ICBSP_CAN_RX_DISPATCH_PER_TICK` 帧（防止一帧风暴饿死其它任务），
每帧先校验（标准 / 扩展帧、数据帧、经典 CAN；远程帧由硬件拒收、DLC > 8 在中断里已丢弃），再交给 `CanRxNodeT::dispatch()`，长度合不合由各节点的 `dlc_mask` 决定：

- 有节点认领：在**本任务里同步**执行回调（要短、不能阻塞）
- 没人认领：`can.return_frame(rx)` 放回**回退缓冲**，业务可以用 `can.receive()` 自己取

---

## 4. can_tx_task：聚合发送（1 ms）

```cpp
sys_state.wait_running();

#  define X(name) CanTxNodeT<decltype(name)>::freeze(); CanTxNodeT<decltype(name)>::init_send_ticks(name, start_tick);
   ICBSP_CAN_INSTANCES(X)        // 冻结注册 + 记录起始 tick

for (;;)
{
#  define X(name) CanTxNodeT<decltype(name)>::service(name);
   ICBSP_CAN_INSTANCES(X)        // 槽位聚合 + 保底心跳 + 有效期检查
   vTaskDelayUntil(&wake_time, pdMS_TO_TICKS(1U));
}
```

- **槽位聚合**：同一个 ID 的控制帧由多个设备各填一段（DJI 协议一帧带 4 个电机），有效槽位都刷新过就发
- **凑不齐时限**：有槽位填了新数据、等了 `ICBSP_CAN_TX_GATHER_MS`（默认 1 ms）还没凑齐也发
- **保底心跳**：`ICBSP_CAN_TX_HEARTBEAT_MS`（默认 5 ms）内没发过就重发当前内容，避免电调超时保护
- **指令有效期**：槽位 `ICBSP_CAN_CMD_EXPIRE_MS`（默认 100 ms）没被刷新就清零（DJI 电机 = 零输出）并标为过期；达妙电机因此不走发送节点（清零 = MIT 满负扭矩，见 [device.md](device.md)）

---

## 5. 调周期 / 优先级 / 栈深

全部在宿主 `user/board/hardware_config.h`：

```c
#define ICBSP_SYS_TASK_ENABLE     1
#define ICBSP_SYS_TASK_PRIORITY   (configMAX_PRIORITIES - 4)   // CMSIS_V2 下 52
#define ICBSP_SYS_TASK_STACK      256
#define ICBSP_CAN_TASK_PRIORITY   (configMAX_PRIORITIES - 2)   // 54；ICBSP_BMI088_TASK_PRIORITY 默认 configMAX_PRIORITIES - 3
#define ICBSP_CAN_TASK_STACK      512
#define ICBSP_CAN_RX_ENABLE       1
#define ICBSP_CAN_TX_ENABLE       1
#define ICBSP_UART_PATROL_ENABLE  1
```

库任务默认按 `configMAX_PRIORITIES` 往下取（评审 A2），**业务任务必须低于它们**：CubeMX 的 `osPriorityNormal`（24）、
`osPriorityHigh`（40）都可以。原来的 `tskIDLE_PRIORITY + 6~8` 比 `osPriorityNormal` 还低，按 CubeMX 习惯建的业务任务一忙，
CAN 节拍、Bus-Off 恢复、串口巡检、Online 计时、IMU 刷新都会被推迟。库里有 `static_assert` 检查取值范围。

`sys_task` 每 10 ms 还推进一次 DWT 绝对时间的回绕跟踪（`bsp_dwt.get_cycles()`，C11）：`ICBSP_SYS_TASK_ENABLE = 0` 时宿主要自己定期调。

改这里不需要动库代码。`sys_task` 的 10 ms 是 `sys_task.cpp` 里的 `constexpr`
（和 Online 计时、CAN 恢复节拍绑在一起），**不建议按板子改**。

---

## 6. 常见坑

| 现象 | 原因 |
| --- | --- |
| 编译报 `static_assert configTICK_RATE_HZ == 1000` | CubeMX 里 tick 不是 1 kHz |
| `can_tx_task` 没创建 | 板级没有注册任何发送节点（没建 `DjiMotor` 之类） |
| 电机过一会儿停一下 | 控制任务刷新间隔超过了 `ICBSP_CAN_CMD_EXPIRE_MS`（槽位过期清零），或 `can_tx_task` 优先级被业务任务压住 |
| 接收回调里卡死整个系统 | 回调里做了阻塞操作（等信号量/打印大段日志） |
| 串口偶发卡死 | `ICBSP_UART_PATROL_ENABLE = 0`，`sys_task` 不再做 `rx/tx_recover()` |
