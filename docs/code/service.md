# service/ — 系统服务层

[← 代码层索引](README.md) · [bsp](bsp.md) · [device](device.md) · [tasks](tasks.md)

**职责**：系统级状态与生命周期（EventState）、设备在线检查、CAN 收发节点、故障现场记录、库入口 `system_*`。
**不负责**：具体设备协议（→ `device/`）、外设时序（→ `bsp/`）。

---

## 1. 文件清单

| 文件 | 职责 | 关键符号 |
| --- | --- | --- |
| `system.hpp/.cpp` | 库入口 + 三个任务体声明；`.dma_buffer` 早期搬运 | `system_init()`、`system_start()`、`sys_state` |
| `status.hpp/.cpp` | 状态码 + 事件状态树 | `Status`、`EventState`、`status_bit()` |
| `online_check.hpp/.cpp` | 设备在线判定（内部单向链表） | `Online` |
| `can_rx_node.hpp/.cpp` | 按 ID 分发 CAN 帧 | `CanRxNodeT<CanT>::regist/dispatch` |
| `can_tx_node.hpp/.cpp` | 发送槽位聚合 + 保底心跳 | `CanTxNodeT<CanT>` |
| `fault_record.hpp/.cpp` | 停机现场（assert / 栈溢出 / 分配失败） | `FaultKind`、`FaultRecord`、`fault_on_*()` |
| `motor_definition.hpp` | 电机通用数据结构 | 供 device / app 共用 |
| `imu_definition.hpp` | IMU 数据结构（`ImuData` 等） | 供 device / app 共用 |

---

## 2. 入口与生命周期

```cpp
void system_init(void);   // 建 sys_state，创建 sys_task / can_rx_task（调度器未启动，任务不会跑）
void system_start(void);  // 有发送节点才建 can_tx_task；检查初始化错误；置运行位
extern EventState sys_state;
```

调用顺序见 [启动顺序](README.md#3-启动顺序)。要点：

- `system_init()` 里所有失败都用 `configASSERT` 停机，不会「半初始化继续跑」
- `system_start()` 里 `sys_state.complete_init()` 会把 `board_init()` 阶段登记的 `init_error` 一次性检查
- `system.cpp` 里还有 `.dma_buffer` 的早期搬运（`constructor(101)`），必须在全局构造之前完成

---

## 3. EventState：状态树

一个 `EventState` = 一个 FreeRTOS 事件组 = 一份「阶段 + 错误登记」。

```cpp
enum class Status : uint8_t { OK, BUSY, TIMEOUT, FULL, IO_ERROR, BAD_ARG, NOT_INIT, NOT_SUPPORTED };
```

| 方法 | 说明 |
| --- | --- |
| `Status init()` | 建事件组；未 init 时其余接口安全返回 |
| `Status set_parent(EventState &parent)` | 把子系统挂到父状态上 |
| `Status complete_init()` | 初始化阶段结束：有 `init_error` 就返回该错误 |
| `Status error(Status)` | 运行期错误，**向上传播**到父节点 |
| `Status init_error(Status)` | 初始化失败登记（不立即停，等 `complete_init()` 统一查） |
| `bool is_ready()` / `bool running()` | 阶段查询 |
| `Status error_code()` | 取当前错误码 |
| `void wait_running()` | 阻塞到运行位；**初始化失败时删除当前任务** |

父子关系只用于「错误向上传播」，**不聚合 running**：子系统 running 不代表父级 running。

业务任务的标准开头：

```cpp
void my_task(void *)
{
  sys_state.wait_running();   // 初始化失败时这里会把任务删掉，不会跑在半初始化系统上
  for (;;) { /* ... */ }
}
```

---

## 4. Online：在线检查

`Online` 的构造函数就完成了「注册到内部链表」：

```cpp
class Online
{
public:
  explicit Online(uint16_t timeout_gap = 30);  // 超过多少 ms 没刷新就判离线
  Status refresh_task();   // 任务上下文：收到有效数据时刷新
  Status refresh_isr();    // 中断上下文：同上，ISR 安全
  Status is_online() const;// OK=在线，TIMEOUT=离线
};
```

`sys_task` 每 10 ms 调一次 `Online::update()`（静态方法，内部遍历链表、用无符号差值比较 tick，
回绕安全）刷新所有节点状态。**注意**：对象析构会从链表摘除，若要动态创建/销毁，
必须与 `Online::update()` 互斥（通常做法是全部在 `board_init()` 前建好，运行期不销毁）。

---

## 5. CAN 收发节点

### 接收：`CanRxNodeT<CanT>`

```cpp
using Callback = Status (*)(void *context, const CanRxMsg &rx);

static CanRxNodeT *regist(CanT &can, uint32_t can_id, Callback cb, void *ctx = nullptr,
                          uint32_t id_type = FDCAN_STANDARD_ID);
static Status      unregist(CanT &can, CanRxNodeT *node);
static void        freeze();                       // 运行后冻结注册
static bool        dispatch(CanT &can, const CanRxMsg &rx);  // 有节点消费返回 true
```

- 在**初始化阶段**（`board_init()`）注册，同一 CAN + 帧类型 + ID 不允许重复
- `dispatch()` 由 `can_rx_task` 调用，回调里要短、不能阻塞，`rx` 只在回调期间有效
- 返回 `false` 表示没人认领，帧进回退缓冲，可通过 `BspCan::receive()` 自己取

### 发送：`CanTxNodeT<CanT>`

```cpp
Status filldata(uint8_t data[], uint8_t slot = 0);  // 往聚合缓冲的某个槽位填 8 字节
Status init();
```

`can_tx_task` 每 1 ms 把同一 CAN 上所有节点按槽位聚合成一帧发出，并做两件事：
保底心跳（100 ms 没新指令也要发）与指令有效期（超过 100 ms 的旧指令视为失效）。

---

## 6. fault_record：停机现场

```cpp
enum class FaultKind : uint32_t { /* assert / stack_overflow / malloc_failed ... */ };
struct alignas(8) FaultRecord { /* 种类、文件、行号、任务名、计数 */ };

void fault_on_assert(const char *file, int line);
void fault_on_stack_overflow(const char *task_name);
void fault_on_malloc_failed(void);
```

- 记录区放在 `.noinit` 段，**复位后不清零**，下次上电还能读上次的停机现场
- `configASSERT` 被 `integrate.py` 接到 `fault_on_assert()`（`FreeRTOSConfig.h` 的 USER CODE 1）
- 栈溢出与分配失败钩子在 `freertos.c` 的 USER CODE 4 / 5 里接好

---

## 7. 常见坑

| 现象 | 原因 |
| --- | --- |
| 业务任务一直不跑 | 前面某步 `init_error`，`system_start()` 断言停机；或忘了调 `wait_running()` 前的 `system_start()` |
| `regist()` 返回 `nullptr` | 同一 CAN + ID 重复注册，或注册发生在 `freeze()` 之后 |
| 设备一直判离线 | 回调里没调 `refresh_task()` / `refresh_isr()`，或 `timeout_gap` 比实际帧间隔小 |
| 复位后读不到上次的故障 | 链接脚本缺 `.noinit`，或该段被 `NOLOAD` 之外的方式初始化了 |
| 错误码一直 OK 但系统没起来 | 子系统 `set_parent()` 前就 `error()`，父节点收不到 |
