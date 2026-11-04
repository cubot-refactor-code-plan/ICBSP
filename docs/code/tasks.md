# tasks/ — 库任务层

[← 代码层索引](README.md) · [service](service.md) · [bsp](bsp.md)

**职责**：三个常驻任务的任务体。
**不负责**：任务的创建（在 `code/service/system.cpp`）、周期/优先级/栈深的配置（在宿主 `user/board/hardware_config.h`）。

任务体是**薄**的：每个周期只做一次遍历，真正的逻辑都在 `service/` 与 `bsp/` 里。

---

## 1. 三个任务一览

| 任务 | 文件 | 周期 | 优先级宏 | 栈深宏 | 谁创建 | 创建时机 |
| --- | --- | --- | --- | --- | --- | --- |
| `sys_task` | `sys_task.cpp` | **10 ms**（`SYS_TASK_PERIOD_MS`） | `SYS_TASK_PRIORITY`（idle+6） | `SYS_TASK_STACK`（256 字） | `system_init()` | 无条件（`SYS_TASK_ENABLE`） |
| `can_rx_task` | `can_rx_task.cpp` | **1 ms** | `CAN_TASK_PRIORITY`（idle+8） | `CAN_TASK_STACK`（512 字） | `system_init()` | 开了 FDCAN 且 `CAN_RX_ENABLE` |
| `can_tx_task` | `can_tx_task.cpp` | **1 ms** | `CAN_TASK_PRIORITY` | `CAN_TASK_STACK` | `system_start()` | 板级**有发送节点**才建 |

创建代码在 `code/service/system.cpp`：

```cpp
system_init():   xTaskCreate(sys_task,     "sys",    SYS_TASK_STACK, NULL, SYS_TASK_PRIORITY, NULL);
                 xTaskCreate(can_rx_task,  "can_rx", CAN_TASK_STACK, NULL, CAN_TASK_PRIORITY, NULL);

system_start():  if (has_tx_nodes()) xTaskCreate(can_tx_task, "can_tx", CAN_TASK_STACK, NULL, CAN_TASK_PRIORITY, NULL);
```

`can_rx_task` / `can_tx_task` 进循环后第一句都是 `sys_state.wait_running()`：
初始化没完成或失败，它们不会碰硬件。

---

## 2. sys_task：系统维护（10 ms）

```cpp
static_assert(configTICK_RATE_HZ == 1000U, "sys_task requires a 1 kHz FreeRTOS tick");
```

每个周期做三件事，全部靠 X-macro 遍历板级实例，**没有运行时登记表**：

```cpp
#  define X(name) (void)name.service_recovery();
   BSP_CAN_INSTANCES(X)        // ① CAN Bus-Off 自恢复

#  define X(name) (void)name.rx_recover(); (void)name.tx_recover();
   BSP_UART_INSTANCES(X)       // ② 串口收发卡死补救（UART_PATROL_ENABLE）

   Online::update();           // ③ 在线计时：超过 timeout_gap 判离线
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
   BSP_CAN_INSTANCES(X)
   vTaskDelayUntil(&wake_time, pdMS_TO_TICKS(1U));
}
```

`can_rx_service()` 每个 tick 最多取 `CAN_RX_DISPATCH_PER_TICK` 帧（防止一帧风暴饿死其它任务），
每帧先校验（标准/扩展帧、数据帧、经典 CAN、DLC=8），再交给 `CanRxNodeT::dispatch()`：

- 有节点认领：在**本任务里同步**执行回调（要短、不能阻塞）
- 没人认领：`can.return_frame(rx)` 放回**回退缓冲**，业务可以用 `can.receive()` 自己取

---

## 4. can_tx_task：聚合发送（1 ms）

```cpp
sys_state.wait_running();

#  define X(name) CanTxNodeT<decltype(name)>::freeze(); CanTxNodeT<decltype(name)>::init_send_ticks(name, start_tick);
   BSP_CAN_INSTANCES(X)        // 冻结注册 + 记录起始 tick

for (;;)
{
#  define X(name) CanTxNodeT<decltype(name)>::service(name);
   BSP_CAN_INSTANCES(X)        // 槽位聚合 + 保底心跳 + 有效期检查
   vTaskDelayUntil(&wake_time, pdMS_TO_TICKS(1U));
}
```

- **槽位聚合**：同一路 CAN 上的控制帧合并成一帧（DJI 协议一帧带 4 个电机）
- **保底心跳**：100 ms 没有新指令也要重发上一次的值，避免电调超时保护
- **指令有效期**：超过 100 ms 的旧指令视为失效，不再重复发

---

## 5. 调周期 / 优先级 / 栈深

全部在宿主 `user/board/hardware_config.h`：

```c
#define SYS_TASK_ENABLE     1
#define SYS_TASK_PRIORITY   (tskIDLE_PRIORITY + 6)
#define SYS_TASK_STACK      256
#define CAN_TASK_PRIORITY   (tskIDLE_PRIORITY + 8)
#define CAN_TASK_STACK      512
#define CAN_RX_ENABLE       1
#define CAN_TX_ENABLE       1
#define UART_PATROL_ENABLE  1
```

改这里不需要动库代码。`sys_task` 的 10 ms 是 `sys_task.cpp` 里的 `constexpr`
（和 Online 计时、CAN 恢复节拍绑在一起），**不建议按板子改**。

---

## 6. 常见坑

| 现象 | 原因 |
| --- | --- |
| 编译报 `static_assert configTICK_RATE_HZ == 1000` | CubeMX 里 tick 不是 1 kHz |
| `can_tx_task` 没创建 | 板级没有注册任何发送节点（没建 `DjiMotor` 之类） |
| 电机过一会儿停一下 | 保底心跳被改短/改长，或 `can_tx_task` 优先级被业务任务压住 |
| 接收回调里卡死整个系统 | 回调里做了阻塞操作（等信号量/打印大段日志） |
| 串口偶发卡死 | `UART_PATROL_ENABLE = 0`，`sys_task` 不再做 `rx/tx_recover()` |
