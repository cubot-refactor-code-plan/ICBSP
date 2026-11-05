# service/ — 系统服务层

[← 代码层索引](README.md) · [bsp](bsp.md) · [device](device.md) · [tasks](tasks.md)

**职责**：系统级状态与生命周期（EventState）、设备在线检查、CAN 收发节点、故障现场记录、库入口 `system_*`。
**不负责**：具体设备协议（→ `device/`）、外设时序（→ `bsp/`）。

---

## 1. 文件清单

| 文件 | 职责 | 关键符号 |
| --- | --- | --- |
| `system.hpp/.cpp` | 库入口 + 三个任务体声明；`.dma_buffer` 早期搬运 | `system_init()`、`system_start()`、`sys_state` |
| `status_code.hpp` | 状态码（纯定义，不依赖 RTOS，评审 A6） | `Status`、`status_bit()` |
| `status.hpp/.cpp` | 事件状态树（包含 `status_code.hpp`） | `EventState` |
| `online_check.hpp/.cpp` | 设备在线判定（内部单向链表） | `Online` |
| `can_rx_node.hpp/.cpp` | 按 ID 分发 CAN 帧 | `CanRxNodeT<CanT>::regist/dispatch` |
| `can_tx_node.hpp/.cpp` | 发送槽位聚合 + 保底心跳 | `CanTxNodeT<CanT>` |
| `fault_record.hpp/.cpp` | 停机现场（assert / 栈溢出 / 分配失败 / 故障异常） | `FaultKind`、`FaultRecord`、`fault_on_*()`、`ICBSP_CHECK()` |
| `newlib_lock.cpp` | newlib 堆的线程锁（弱符号，评审 A9） | `__malloc_lock` / `__malloc_unlock` |
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
| `Status complete_init()` | 初始化阶段结束：置运行位；有过 `init_error` 就返回 `NOT_SUPPORTED`、不置位 |
| `Status error(Status)` | 运行期错误：只记码，不动运行位，**向上传播**到父节点 |
| `Status init_error(Status)` | 初始化失败：记码 + 置粘滞失败位 + 清运行位，向上传播。**只在初始化期有效**：本实例已在运行时降级成 `error()`，返回 `NOT_SUPPORTED`（运行期的错误不能让系统停摆） |
| `Status clear_error(Status)` / `Status clear_errors()` | 清一个 / 全部错误码；只清本实例、**不传播**，不动失败位与运行位 |
| `bool is_ready()` / `bool running()` | 阶段查询 |
| `Status error_code()` | 未清的错误码里**枚举值最小**的那个（不是最近一次）；没有错误返回 `OK` |
| `uint32_t error_bits()` | 未清的错误码位图（`status_bit()` 掩码） |
| `void wait_running()` | 阻塞到运行位；**初始化失败时删除当前任务** |

父子关系只用于「错误向上传播」，**不聚合 running**：子系统 running 不代表父级 running。

错误的生命周期：`error()` / `init_error()` 记下的码一直保留（粘滞），直到维护者调 `clear_error()`；
清子节点不会清父节点（父节点上的同一个码可能还来自别的子节点）。初始化失败位永不清除。

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
回绕安全）刷新所有节点状态。遍历时挂起调度器、每个节点单独进出一次临界区，关中断的时间不随节点数增长。
对象只能在任务里构造 / 析构（析构从链表摘除；`update()` 挂着调度器，任务里的增删不会和遍历交叠），不能在 ISR 里析构。

---

## 5. CAN 收发节点

### 接收：`CanRxNodeT<CanT>`

```cpp
using Callback = Status (*)(void *context, const CanRxMsg &rx);

static CanRxNodeT *regist(CanT &can, uint32_t can_id, Callback cb, void *ctx = nullptr,
                          uint32_t id_type = FDCAN_STANDARD_ID, uint16_t dlc_mask = CAN_DLC_MASK_8);
static Status      unregist(CanT &can, CanRxNodeT *node);
static void        freeze();                       // 运行后冻结注册
static bool        dispatch(CanT &can, const CanRxMsg &rx);  // 有节点消费返回 true
```

- 在**初始化阶段**（`board_init()`）注册，同一 CAN + 帧类型 + ID 不允许重复
- `dispatch()` 由 `can_rx_task` 调用，回调里要短、不能阻塞，`rx` 只在回调期间有效
- `dlc_mask`：接受的数据长度位图（第 n 位 = DLC n），默认只收 8 字节帧，`CAN_DLC_MASK_ANY` 收 0~8；长度不符当作没人认领
- 返回 `false` 表示没人认领：计入 `diagnostics.rx_unclaimed`，有回退缓冲（`CAN_RX_RETURN_FRAMES > 0`）就放进去，可通过 `BspCanT::receive()` 自己取

### 发送：`CanTxNodeT<CanT>`

```cpp
static CanTxNodeT *regist(CanT &can, uint32_t can_id, uint8_t division = 1, uint8_t slot = 0,
                          uint32_t id_type = FDCAN_STANDARD_ID);
Status filldata(uint8_t data[], uint8_t slot = 0);  // 往聚合缓冲的某个槽位填 8 / division 字节
Stats  get_stats() const;                          // sent / partial_sends / heartbeat_sends / expired
```

`can_tx_task` 每 1 ms 检查一次，满足任一条件就把这一帧发出：
- **凑齐即发**：所有“有效”槽位都重新填过（过期的、注册了却从没填过的槽位不算）；
- **凑不齐时限**：有槽位填了新数据，等了 `CAN_TX_GATHER_MS` 还没凑齐（计入 `partial_sends`，大于 0 说明同帧设备的刷新节拍不一致）；
- **保底心跳**：`CAN_TX_HEARTBEAT_MS` 内没发过（计入 `heartbeat_sends`）。

槽位 `CAN_CMD_EXPIRE_MS` 没被刷新就清零并标为过期；槽位全部注销的节点不再发送。
发送走 `send_latest()`：同 ID 的上一帧还没发出去时这一帧不入队、下个周期发最新内容，所以总线被占满时一个节点最多在驱动里占一帧。

---

## 6. fault_record：停机现场

```cpp
enum class FaultKind : uint32_t { NONE, ASSERT, STACK_OVERFLOW, MALLOC_FAILED, HARD_FAULT, MEM_MANAGE, BUS_FAULT, USAGE_FAULT };
struct alignas(8) FaultRecord { /* 种类、文件、行号、任务名、计数、tick、IPSR；故障异常另有 CFSR / HFSR / MMFAR / BFAR / PC / LR / xPSR / EXC_RETURN；boot_rsr */ };

void fault_on_assert(const char *file, int line);
void fault_on_stack_overflow(const char *task_name);
void fault_on_malloc_failed(void);
void fault_on_exception(uint32_t kind, uint32_t exc_return);   // HardFault / MemManage / BusFault / UsageFault

#define ICBSP_CHECK(expr)   // 求值并检查：失败走 fault_on_assert()；不会随 configASSERT 一起被编译掉
```

- 记录区放在 `.noinit` 段，**复位后不清零**，下次上电还能读上次的停机现场；上电（POR / BOR）时整体清零（AXI SRAM 带 ECC，
  上电后内容随机，评审 A8），本次启动的复位原因记在 `boot_rsr`（`RCC->RSR`，读完已清）
- `configASSERT` 被 `integrate.py` 接到 `fault_on_assert()`（`FreeRTOSConfig.h` 的 USER CODE 1，`do { } while (0)` 形式）
- 栈溢出与分配失败钩子在 `freertos.c` 的 USER CODE 4 / 5 里接好
- 故障异常在 `stm32h7xx_it.c` 四个处理函数的 USER CODE 段里转调 `fault_on_exception()`（片段 `templates/stm32h7xx_it_user_code.c`，
  `integrate.py` 自动写入，评审 A5）：任务里出错时从 PSP 解出出错指令地址 `pc`，用 `arm-none-eabi-addr2line -e <elf> <pc>` 找到源码行
- **有副作用的初始化检查用 `ICBSP_CHECK(x.init() == Status::OK)`**，不要写进 `configASSERT(...)`（评审 A1，见编码规范）

---

## 7. 常见坑

| 现象 | 原因 |
| --- | --- |
| 业务任务一直不跑 | 前面某步 `init_error`，`system_start()` 断言停机；或忘了调 `wait_running()` 前的 `system_start()` |
| `regist()` 返回 `nullptr` | 同一 CAN + ID 重复注册，或注册发生在 `freeze()` 之后（运行态之后注册只返回失败，不影响系统运行） |
| 设备一直判离线 | 回调里没调 `refresh_task()` / `refresh_isr()`，或 `timeout_gap` 比实际帧间隔小 |
| 复位后读不到上次的故障 | 链接脚本缺 `.noinit`，或该段被 `NOLOAD` 之外的方式初始化了 |
| 错误码一直 OK 但系统没起来 | 子系统 `set_parent()` 前就 `error()`，父节点收不到 |
