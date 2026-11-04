# stm32-framework 与 stm32_bsp 对比评估 & v1.0.0 路线

- 对比对象：
  - **本仓库**：`third_party/stm32_bsp` @ `43a34cf`，下文路径前缀记作 `bsp:`；
  - **fork**：`/home/choseb/Code/stm32-framework` @ `7ef6bfa`（远端 `cubot-refactor-code-plan/stm32-framework`，作者 Rh-i，2026-11-01 ~ 11-03，共 54 个提交），路径前缀记作 `fw:`。
- 问题编号 F / A / I / D 沿用 `stm32_bsp_review.md`；本文新编号：**C** = 差异条目，**N** = fork 引入的新问题，**Q** = 需要你回答的问题。
- 方法：逐文件 `diff`（去掉 CRLF 再比较），并对照 fork 的提交历史。**两边都没有重新编译，也没有上板**；fork 自己的 README 写明“实板验证尚未完成”，只有 EventState / Online 有实板记录（`fw:docs/service_test/event_state_online.md`，81/81 通过）。

---

## 0. 结论摘要

1. **fork 不是在我们的代码上逐步改进，而是“换芯”。** 首个提交 `ae8746f` 和我们的 `43a34cf` 几乎一样，只做了改名和 C++20 适配。真正的改动集中在 `87c8480` / `1e8997c` / `87a5a0e` / `0cd503b`（“Merge User BSP / Service / Device layer”）：用作者自己 `User/` 目录里的代码**整块替换**了 UART、CAN 驱动和 CAN 节点，同时去掉了实例登记表。被换进来的 UART / CAN 是 **2026-10-17 实机测试修复之前**的那一代，所以 `docs/bsp_test/changes.md` 第 3 节记录的修复大多又退回去了（见 N1~N12）。`fw:docs/bsp_test/` 是从我们这里原样带过去的，测的不是 fork 现在的代码。
2. **fork 解决了我们已有问题里的一小部分**：A7（借助可加载的 `.dma_buffer`）、F9 的重复 init 与 RF0L 计数、I3（删掉 `CanMode`）、I8（GPIO / Key 的空指针）、F11 的多圈角精度、I7 的一部分（PID 删掉 `print`、去掉 virtual）、F7 的一部分（RX 硬件错误计数），以及 Online 计时和 sys_task 周期解耦。**高、中级别的问题（F1、A1、A3、F3、F4、F5、A2、A4、A5、I1、I2、D1）一个都没有解决。**
3. **fork 真正的价值**在于：新功能（UART 中断传输、`UartDir`、RX / TX 尺寸分开、DWT、编码器、舵机、直流电机、R9DS、EmmV5、EventState 树、功能开关）、工具链（`integrate.py` 生成 `.vscode` / `Flash/` / `.clangd`，能识别更多型号）以及**明显更好懂的文档结构**（接入指南把每条要求的“为什么”讲清楚，六层代码文档配了架构图）。
4. ~~总体建议：以本仓库为底，挑着移植 fork 的功能和文档结构~~（2026-11-03 初稿的建议，已被第 5 条取代）。
5. **定稿（2026-11-04，按第 3 节的回答）**：Q1 / Q2 / Q3 / Q13 / Q14 / Q16 都选了 fork 的方案（X-macro、C++20、宿主必须提供完整配置、`code/` 布局、fork 的 `integrate.py`、fork 的文档结构），所以**改以 fork 为主线**（Q15 选 B）：fork 的架构、新功能、工具和文档整体保留，把本仓库的 UART / CAN 收发状态机和 10-17 实测修复**回填**进 fork，再修已有问题。库改名为 **ICBSP**，发布方署名 **INVC & CUBOT**。完成后在 GitHub 上归档 `STM32_BSP` 仓库（Q19）。详见 3.1 节和第 4 节。
6. **独立复核（2026-11-04，fork @ `7ef6bfa`，同样只读代码、没有编译和上板）**：先不看本文，从头审一遍 fork 的 `code/`、`templates/`、`CMakeLists.txt`、`tools/integrate.py`，再和本文对照。结果是：N1~N26 在当前代码里都还成立，下面三处判断要改：
   - **F11 在 fork 里其实没有解决**：`angle_multi_round` 的类型改成了 double，但 `fw:code/device/dji_motor.cpp:311` 赋值时用的是 `static_cast<float>(angle)`，精度仍然是 float（C20、附录 A 已改）；
   - **N21 的修法要分情况**：`CAN_RX_ENABLE = 0` 时没有 `can_rx_task`，`receive()` 如果只读回退缓冲，就永远读不到数据（N28）；
   - **N20 不只是多写者**：发送 MessageBuffer 同时也有两个读者，任务和 ISR 都会读（N29）。
   另外新记 11 条（**N27~N37**，见 2.8 节）。其中最要紧的三条：**N27**，CAN 的 `tx_recover()` 没有任何地方调用，TX-empty 动态开关（Q9 = B）失去了补救手段，阶段 3 的“丢唤醒统计”会一直显示 0；**N28**，关掉 CAN 接收路径后 `receive()` 会卡死；**N30**，库独占了 HAL 回调，库外的同类外设（宿主自己管的 UART / SPI / FDCAN）收不到回调。第 4 节的路线和附录已经同步更新。

---

## 1. 两个仓库的关系与差异总览

### 1.1 提交脉络（fork）

| 阶段 | 提交 | 内容 |
| --- | --- | --- |
| 起点 | `ae8746f` | 本仓库 `43a34cf` 的快照；改名 stm32_bsp → stm32_framework；加 `tasks/`、`CAN_RX/TX_ENABLE`；volatile `++` 改成 `x = x + 1U`（C++20 弃用 volatile 复合赋值） |
| 换芯 | `87c8480` `1e8997c` `87a5a0e` `0cd503b` `a7fa3cd` `b6d6076` `e0a1984` `a46604c` | 合入 `User/` 层的 BSP / Service / Device / Emm_V5；登记表改成 `board_instances.hpp` X-macro；inline 全局实例；`.dma_buffer` 改为可加载段 |
| 接口打磨 | `040df90`~`62e90a8` | `uart_mode::dma/it`、`UartDir`、指定初始化器；拆出 QuaternionPID；`hardware_config.h` 挪到 `user/board/` |
| 工具 / 文档 | `3cd0059`~`d0d7254` | `integrate.py` 生成 `.vscode` / `Flash/` / `.clangd`；`code/` 目录；`docs/code/` 六层文档 + 架构图；sys 优先级 +6 |
| 最近 | `75e6835` `7ef6bfa` | `bsp_dwt` 实例收进库，由 `system_init()` 初始化 |

### 1.2 文件对照

| 情况 | 文件 |
| --- | --- |
| 大改 | `bsp_uart.*`（约 1100 行不同）、`bsp_can.*`（约 900 行）、`status.*`（约 220 行）、`can_rx_node` / `can_tx_node`（.cpp 删掉，改成仅头文件的模板）、`system.cpp`、`sys_task` / `can_rx_task` / `can_tx_task`（挪到 `code/tasks/`）、`pid.*`、`bsp_gpio` / `bsp_key` / `bsp_pwm` / `bsp_spi` |
| 小改 | `dji_motor.*`（按 CanT 模板化、double 多圈角）、`online_check.*`（改成 ms 计时）、`device_bmi088.*` / `device_buzzer.*` / `matrix.hpp`（Config 改聚合体、`_data` 公开）、`motor_definition.hpp` |
| 只改了注释格式 | `vqf.*`、`quaternion.*`、`rls.hpp`、`fault_record.*`、`imu_definition.hpp` |
| fork 新增 | `bsp_dwt`、`bsp_encoder`、`device_motor`、`device_servo`、`device_r9ds`、`device_emmv5_group`、`protocol/emmv5_protocol`、`quaternion_pid`、`templates/user/board/board_instances.hpp`、`templates/{vscode,Flash,clang}`、`tools/draw_diagrams.py`、`docs/{接入指南,常用接口,编辑器配置}.md`、`docs/code/*`、`docs/service_test/*` |
| fork 删除 | `bsp_can_recovery.cpp`（并入 bsp_can）、`hardware_config_defaults.hpp`、`examples/dm_mc02/`、`templates/hardware_config.h`（挪到 `user/board/`）、`tools/integrate.py` 的旧结构 |

---

## 2. 逐项评估

每一条按这几个方面写：**做了什么 → 已有问题有没有解决 → 引入了什么新问题 → 新功能 / 接口语义的变化 → 处理意见**。处理意见只针对“修问题 / 引问题”的部分；新功能和接口变化交给第 3 节的问题。

> **2026-11-04 注**：本节的“处理意见”是按“以本仓库为底”写的。主线改成 fork 之后，“移植 X”读作“保留 fork 的 X”，“保持我们的实现”读作“把我们的实现回填进 fork”；判断（哪些是修复、哪些是回退）不变。C3 的第三方案（NOLOAD + 启动清零）因 Q2 选 C++20 而作废，采用 fork 的可加载方案。最终方案以 3.1 节和第 4 节为准。

### 2.1 架构

#### C1 实例登记表 → 宿主的 `board_instances.hpp`（X-macro）

- 做了什么：`BSP_CAN_INSTANCES(X)` / `BSP_UART_INSTANCES(X)` / `BSP_SPI_INSTANCES(X)` 由宿主定义；库里的 HAL 回调展开成一串 `else if (h == name.handle())`（`fw:code/bsp/bsp_uart.cpp:21-73`、`bsp_can.cpp:28-77`、`bsp_spi.cpp`），库任务直接按名字调用（`fw:code/tasks/*.cpp`）。
- 已有问题：无。
- **新问题**：
  - **N13** 实例漏写不会报错，只会“中断不分发、串口不巡检”（fork 自己在 `fw:templates/user/board/board_instances.hpp` 和 README 的“已知限制”里也承认）。我们的登记表在 `init()` 时自动登记，不可能漏掉。
  - **N14** 依赖倒置：库源码 `#include "board_instances.hpp"`（宿主文件），`sys_task` 还直接用宿主的 `print_uart` 宏（`fw:code/tasks/sys_task.cpp:62`）。库不能单独编译，也不能做主机侧单元测试；宿主少定义一个宏就编译失败。
  - SPI 丢掉了“两个实例绑同一句柄”的检测（我们在 `bsp:bsp/bsp_spi.cpp` 的 `register_self()` 里有）。
- 处理意见：**问 Q1**。我的倾向：保持登记表。登记表的代价只有十几个指针，加上一次线性查找，对 ISR 的影响可以忽略。fork 的“零开销”换来的是更容易犯的错误和依赖倒置。

#### C2 C++11 → C++20（inline constexpr 实例写在头文件、指定初始化器）

- 做了什么：`fw:CMakeLists.txt` 里 `CXX_STANDARD 20`；各 `Config` 改成带默认成员初始化的聚合体，用 `{.huart = ..., .mode = ...}` 写；`board_cfg.hpp` 里用 `inline` 定义实例；构造函数一律 `constexpr`，析构必须平凡（另外提供 `deinit()`）；volatile `++` 全部改成 `x = x + 1U`。
- 已有问题：附带缓解了 D3 的一点（指定初始化在 C++20 下是标准写法，不再是 GNU 扩展）。
- 新问题：宿主自己的目标也必须 ≥ C++20（`board_cfg.hpp` 用到了指定初始化）；`DeviceBmi088` 的构造函数不再允许省略 cfg（`fw:code/device/device_bmi088.hpp:183`）；volatile 写法的改动遍布全库。
- 处理意见：**问 Q2**。

#### C3 `.dma_buffer` 从 NOLOAD 改为可加载段 + 启动时拷贝

- 做了什么：链接片段改成 `>RAM_D1 AT> FLASH` 并导出 `__dma_buffer_start/end/load` 符号（`fw:templates/linker_sections.ld`）；`fw:code/service/system.cpp` 里用 `__attribute__((constructor(101)))` 在全局构造之前 `memcpy`；段名必须写成 `.dma_buffer.<变量名>`（因为 inline 变量是 COMDAT，同名段会把重复副本也带进来）；CMake 配置阶段还会检查“不是 NOLOAD 且有 AT>”。
- 已有问题：**解决了 A7**（NOLOAD 不清零、静态初始化的值丢失）。
- 新问题：缓冲区的初值（基本都是 0）要占同样大小的 Flash（每个 UART 实例约 384 B，每个 SPI 实例约 512 B）；正确性依赖构造优先级和段名约定这两条隐性规则，写错不会报错。
- 处理意见：**第三方案**。如果 Q2 保持 C++11、实例不放在头文件里，那么不需要拷贝初值：段继续用 NOLOAD，在启动时（`constructor(101)` 或启动文件）把 `.dma_buffer` **清零**即可，和 `.bss` 的语义一致，不占 Flash，也不需要按变量命名段。如果 Q2 选 C++20 + inline constexpr 实例，就必须用 fork 的方案。

#### C4 `hardware_config.h` 改成必须提供的唯一配置源（删掉默认值头文件）

- 做了什么：库里直接 `#include "hardware_config.h"`；模板里写全所有值和 `static_assert`（`fw:templates/user/board/hardware_config.h`）；宿主不提供就编译失败。
- 新问题：
  - **N15** 好几个宏已经**没有任何代码引用**：`CAN_RX_QUEUE_FRAMES`、`CAN_RX_RETURN_FRAMES`、`CAN_TX_QUEUE_FRAMES`（队列深度改由 `BspCanT<RX, TX>` 的模板参数决定）、`CAN_BUSOFF_RETRY_MS`（`fw:code/bsp/bsp_can.cpp:352,367,385` 写死成 100）、`UART_ENABLE_FIFO`（对应的调用被注释掉了，`bsp_uart.cpp:288`）。用户改这些值不会有任何效果。
  - 范围检查放在宿主文件里，用户整段删掉也能编译通过；库升级新增一个宏时，所有宿主的配置文件都得跟着改，否则编译失败。
- 处理意见：**问 Q3**。我的倾向：保持“库内默认值 + `static_assert`，宿主可选覆盖”；把 C5 的功能开关加进默认值头文件。

#### C5 功能开关（`SYS_TASK_ENABLE` / `CAN_RX_ENABLE` / `CAN_TX_ENABLE` / `UART_PATROL_ENABLE`）

- 新功能：可以裁掉库任务和整个 CAN 收 / 发路径；对应的 HAL 模块没开时自动置 0。
- 注意：`DjiMotor` 要求两路都开（`fw:code/device/dji_motor.hpp:20`）；关掉 `SYS_TASK_ENABLE` 之后 Online 不再更新、Bus-Off 不再恢复，文档里没有提醒。
- 处理意见：**问 Q4**。

#### C6 目录布局：`code/`、`tasks/`；删掉 `examples/`；LICENSE 移到根目录

- 已有问题：部分解决 A10（submodule 不再带几十 MB 的示例 HAL；厂商 PDF 仍在 `docs/`）。
- 新问题：没有了库自带的回归工程，`docs/bsp_test/` 里的用例只能在外部宿主工程（比如本工作区）上复现。
- 处理意见：**问 Q13**。

#### C7 `EventState` 取代 `sysEvent` + `SysFlag*`

- 做了什么：`fw:code/service/status.hpp` 新增 `EventState` 类（原生 FreeRTOS 事件组、可以有多个实例、`set_parent()` 把错误向上传播、`error()` / `init_error()` / `complete_init()` / `wait_running()` / `error_code()`），还有 `static_assert` 检查状态码不能占用系统标志位；全局 `sys_state` 由 `system_init()` 初始化。有实板测试 81/81。
- 已有问题：部分解决 A6（位号冲突有了编译期检查；`status.hpp` 不再依赖 `cmsis_os2.h`，但仍然依赖 FreeRTOS，`algorithm/rls.hpp` 照样被带进 RTOS）。
- **新问题 N16（中）**：运行态之后再调 `CanTxNodeT::regist()`，`_frozen` 为真，会走 `sys_state.init_error(Status::BAD_ARG)`（`fw:code/service/can_tx_node.hpp:134`）。`init_error()` 会**清掉 RUNNING 位**，于是 `can_rx_task` 的 `if (sys_state.running())`（`fw:code/tasks/can_rx_task.cpp:48`）不再成立，**整个系统的 CAN 接收分发全部停止**。我们的实现在这条路径上只返回 nullptr，不记错误。（`DjiMotor::init()` 会先检查 running 再返回，所以这个坑主要在直接调用 `regist` 时出现。）
- 处理意见：功能本身见 **Q6**；如果采用，必须修掉 N16（运行期误用只用 `error()` 记一下，或者直接返回）。

#### C8 `sys_task` 节拍与行为

- 做了什么：周期从 1 ms 改成 10 ms（`fw:code/tasks/sys_task.cpp:17`），开机先等 1 s（`:29`），每 5 s 通过宿主的 `print_uart` 打印一次 uptime / 循环耗时（`:62`）；默认优先级 +7 → +6（`fw:templates/user/board/hardware_config.h:66`）。
- 新问题：
  - 开机后第一秒里 Bus-Off 恢复和串口巡检都不工作；
  - 依赖宿主的宏（见 N14）；
  - 优先级注释前后矛盾：`hardware_config.h` 写“低于业务任务（常用 idle+7）”，`app_main.cpp` 又写“业务 ≤ +5，让库的 CAN / sys 任务（+8 / +7）先跑”，而 BMI088 也是 +6，和 sys 同级，会时间片轮转。A2（相对 CMSIS 优先级偏低）也没解决。
- 处理意见：**问 Q5**。打印依赖建议舍弃（库不应该引用宿主的宏，要打印的话做成库提供的可选回调）。

#### C9 `Online` 改成毫秒计时

- 做了什么：每个节点记 `_last_refresh_tick`，`update()` 按 `now - last >= pdMS_TO_TICKS(gap)` 判断（`fw:code/service/online_check.cpp`）；`isOnline` → `is_online`。
- 已有问题：离线判定不再依赖 `update()` 的调用频率（sys_task 改成 10 ms 后这是必要的）；`is_online` 符合命名规范（I6 的一部分）。
- 代价：判定会多一个 update 周期的延迟（10 ms）。
- 处理意见：**移植**（改名见 Q7）。

#### C10 `can_tx_task` 只在有发送节点时创建

- 等价于我们“没有节点就自行退出”的写法，只是省了一份栈。处理意见：**移植**。

#### C11 `bsp_dwt`（库持有实例，`system_init()` 里初始化）

- 新功能：CYCCNT 计时、绝对时间（处理回绕）、忙等延时（`fw:code/bsp/bsp_dwt.*`）；CPU 频率从 RCC 推算。
- 可以顺带解决 **A3**：BMI088 在调度器启动前改用 `bsp_dwt.delay_ms()` 忙等，就不再依赖 HAL 时基中断的优先级。
- 注意：`get_time_s()` 会改 `_base_s` / `_last_cnt`，不是线程安全的；两次调用间隔超过 7.8 s（550 MHz 下）会漏掉一次回绕。
  - （2026-11-04 复核补充）两个任务同时调用时，后读 CYCCNT 的任务先写 `_last_cnt`，另一个任务回来后会看到 `now < _last_cnt`，误判成回绕，时间一下子**往前跳 7.8 s**。另外，回绕跟踪现在只靠 `sys_task` 每 5 s 调一次 `get_time_s()`（`fw:code/tasks/sys_task.cpp:58-60`），这个间隔小于 7.8 s 只是凑巧，而且 480 MHz 时一圈是 8.9 s。Q5 去掉打印时如果把这次调用也一起删了，绝对时间就不再跟踪回绕。修的时候要在 sys_task 里保留一次周期性的 `update_timeline()`（间隔 ≤ 回绕周期的一半），并给它加临界区。
- 处理意见：**问 Q11**（倾向移植，并用来修 A3）。

### 2.2 BSP

#### C12 `BspUartT<Transport, RX_SIZE, TX_SIZE>`（UART 整块替换）

**新功能 / 接口变化**：`uart_mode::dma` / `uart_mode::it` 两种传输方式（中断收发，适合没有 DMA 通道的串口）；`UartDir::RxOnly / TxOnly / TxRx`（取代 `transmit_enable`）；RX / TX 缓冲尺寸分开；`RxDiag::hw_error_cnt`（ORE / FE / NE / PE / RTO）；`check_format()`（核对 CubeMX 的帧格式）；`deinit()`；查询接口改成 `get_rx_health()` / `get_rx_diag()` / `get_tx_diag()`。

**已有问题**：
- F7 部分解决（RX 线路错误有了计数）；
- F2 的“同一段发两遍”在 fork 里不存在了：中断里总是重新从流缓冲区取数据，不会重发旧段。但代价见 N3。
- I1 没有解决：显式实例化只有 3 种组合（`fw:code/bsp/bsp_uart.cpp:12-14`），README 里也承认。

**新问题（与 10-17 实测修复相比全部是回退）**：

| 编号 | 回退 | fork 位置 | 我们的对应修复 |
| --- | --- | --- | --- |
| N1 | **发送流缓冲区有多个写者**：去掉了 `_tx_write_lock`；`send()` 直接 `xStreamBufferSend`；启动失败时任务（`:863`）和中断（`:644`）都会把数据写回流缓冲区。FreeRTOS 流缓冲区只允许一个写者，并发 `send()` 时会互相交错、越界；写回的数据会排到队尾，顺序打乱 | `bsp_uart.cpp:351,644,863` | `_tx_write_lock` + “中断侧从不写流缓冲区”（`bsp:bsp/bsp_uart.hpp:36-39`） |
| N2 | **硬件 FIFO 不再打开**（调用被注释掉），`UART_ENABLE_FIFO` 成了摆设 → 2 Mbps 以上连续接收会 ORE | `:288` | `bsp:bsp/bsp_uart.cpp:421-423` |
| N3 | RX 流缓冲区 = RX_SIZE（不再是 2 倍）→ 921600 下“缓冲满后紧接着的帧尾被丢”又会出现 | `:250` | `bsp:...:365`（2 倍） |
| N4 | 删掉了 IDLE 之后按剩余计数重算长度 → 4 Mbps 下又会静默丢 1 字节 | `on_idle_isr` | `bsp:...:793-800` |
| N5 | 卡死判定退回“上次启动之后过了多久”，不看传输是否在推进 → `Config::baudrate` 填错时会把正常发送中止（实测 U15：150 帧丢 97 帧） | `:495` | `bsp:...:675-687` |
| N6 | 先读剩余计数、再中止 → 续发时可能重复 | `:505-508` | `bsp:...:689-696`（先中止再结算） |
| N7 | 删掉了 DMA 句柄修复（`repair_dma_handle`）和 rx_recover 期间关 NVIC → 句柄卡在 `HAL_DMA_STATE_ERROR` 后永远恢复不了 | `rx_recover` | `bsp:...:105-120, 739-754` |
| N8 | 运行中重新 `init()` 不先停收发 → 返回 IO_ERROR、实例失效 | `init()` | `bsp:...:334-356` |
| N9 | `pdMS_TO_TICKS(portMAX_DELAY)` 溢出成约 71 分钟，“一直等”变成会超时 | `:351,457` | `ms_to_ticks()`（`bsp:...:32-35`） |
| N10 | 不检查 `hdmarx` / `hdmatx` 是否为空、实例在不在 DMA 可达的内存（注释和模板都说“会返回 BAD_ARG”，实际没有检查）→ 绑到没配 DMA 的串口或者忘了放进 `.dma_buffer` 时静默失效 | `init()` | `bsp:...:314-324` |
| N11 | 删掉了 RS485 软件 DE（`de_port` / `de_pin`） | — | `bsp:bsp/bsp_uart.hpp:156-175` |
| N12 | 在途长度 `_tx_inflight_len` 在任务侧启动之后才写，中断可能先完成并写入新值，然后被任务侧的旧值覆盖 → `tx_recover` 结算出错 | `start_transmission` | （我们的 F2 是同一类时序问题） |

**处理意见：两者结合**。以我们的实现为底，**舍弃** fork 的收发状态机，把 Transport 抽象（IT 传输）、`UartDir`、RX / TX 尺寸分开、`hw_error_cnt`、`check_format()`、`deinit()` 移植过来；F2 按我们评审里的方案修（调用 HAL 之前先置 `_tx_chunk_active`）。具体移植哪些看 **Q8**。

#### C13 `BspCanT<RX_DEPTH, TX_DEPTH>`（CAN 整块替换）

**新功能 / 接口变化**：队列深度变成模板参数；新增诊断 `rx_lost`（RF0L）、`err_passive`、`err_warning`、`tx_buf_full`、`tx_stall_recover`、`tx_it_fail`；库占用 `HAL_FDCAN_ErrorStatusCallback`；TX-FIFO-EMPTY 中断随软件缓冲有没有帧动态开关；`tx_recover()`；`read_raw()` / `return_frame()` 供节点任务使用；`receive()` 超时单位改为 ms；删掉 `CanMode`；`send()` 的 data 改为 `const`。

**已有问题**：解决 **F9**（运行中重新 init 前会先复位、失败回滚；RF0L 有计数）、**I3**（删掉了不起作用的 `CanMode`）、I6 的超时单位不统一。

**新问题（回退）**：

| 编号 | 回退 | fork 位置 | 我们的对应修复 |
| --- | --- | --- | --- |
| N17 | 全局过滤器又写回 `FDCAN_FILTER_REMOTE` → 远程帧重新进入回退缓冲 | `bsp_can.cpp:752` | `FDCAN_REJECT_REMOTE`（`bsp:bsp/bsp_can.cpp:255-259`） |
| N18 | 判断 TX 有没有空位又改回 `HAL_FDCAN_GetTxFifoFreeLevel` → TX 配成 Queue 模式时一帧都发不出 | `:299,489,617` | 看 `TXFQS.TFQF`（`bsp:...:297-300`） |
| N19 | 不管 DLC 多少都拷 8 字节 → DLC < 8 的帧带出栈上的旧数据 | `:459` | 按 DLC 拷贝（`bsp:...:397-403`） |
| N20 | `send()` 不再加临界区，中断里写入失败也会写回 → 发送 MessageBuffer 有多个写者（`can_tx_task`、用户任务、ISR）。读者同样不止一个，见 N29 | `:198,502,630` | `send()` 整段放在 `taskENTER_CRITICAL` 里（`bsp:...:216-224`） |
| N21 | `receive()` 在回退缓冲没有数据时会去读**原始缓冲**，和 `can_rx_task` 抢同一个 MessageBuffer（单读者约束被破坏） | `:236` | 只读回退缓冲（**仅限 `CAN_RX_ENABLE = 1`**；为 0 时要只读原始缓冲，见 N28） |
| N22 | Bus-Off 恢复间隔写死 100 ms（`CAN_BUSOFF_RETRY_MS` 不起作用）；`bus_off_events` 在 ISR（`:523`）和 `service_recovery`（`:350`）里**各计一次** | `:350-385,523` | 用宏，只计一次 |
| N23 | 只显式实例化了 `BspCanT<16,16>`，连带 `DjiMotor<…, BspCanT<16,16>>`（`fw:code/device/dji_motor.cpp:385-387`）→ 换个队列深度就链接失败（I1 的问题扩散到了 CAN）。串口设备也一样：`DeviceR9dsT` 只实例化了 `<it,128,32>` / `<dma,128,128>`（`fw:code/device/device_r9ds.cpp:10-11`），`DeviceEmmv5GroupT` 只实例化了 `<dma,128,128>`（`fw:code/device/device_emmv5_group.cpp:10`） | `bsp_can.cpp:13` | 队列深度是宏 |

**处理意见：两者结合**。保持我们的收发与恢复实现，移植：重新 init 的复位与回滚、`rx_lost` / `err_passive` / `err_warning` 计数、删除 `CanMode`、超时改成 ms、`const uint8_t *data`。TX-empty 中断动态开关和 ErrorStatusCallback 的归属看 **Q9**。

#### C14 SPI

- 只改了 Config 聚合体和 X-macro 分发（见 C1 / C2）；A4（BDMA 外设的可达性误判）没有解决；`HAL_SPI_MODULE_ENABLED` 保护还在。
- 处理意见：跟随 Q1 / Q2，其余保持。

#### C15 PWM

- 改动：constexpr 构造；查询接口改名为 `get_duty_pct()` / `get_counter_clk_hz()` / `get_period_cnt()` / `get_freq_hz()`。
- **新问题 N24**：删掉了 `#ifdef HAL_TIM_MODULE_ENABLED`（`fw:code/bsp/bsp_pwm.*`），新增的 `bsp_encoder`、`device_servo`、`device_motor` 也都没有这个保护；UART 同样如此，`fw:code/bsp/bsp_uart.*` 删掉了 `HAL_UART_MODULE_ENABLED` 保护，`device_r9ds`、`device_emmv5_group` 也没有。结果是宿主没开 TIM / UART 时会编译失败，破坏了“没开的外设不参与编译”的约定。
- F10（ARR 不预装载、32 位定时器溢出）没有解决。
- 处理意见：保护宏**保持我们的**，改名看 **Q10**。

#### C16 GPIO

- 改动：删掉默认构造和 `init()`，只能 `constexpr BspGpio(cfg)`；`write/read` → `set_level/get_level`，`get_port/get_pin` → `port/pin`。
- 已有问题：解决 I8 的 GPIO 部分（不能再默认构造出空指针对象）。
- 处理意见：**移植“删掉默认构造”**；改名看 **Q10**。

#### C17 Key

- 改动：`poll()` → `update()`，`Event::LONG_PRESS` → `LONG`；`init()` 不再带参数（参数在构造时给）；未初始化时安全返回；`debounce_cnt` / `long_press_cnt` 为 0 时返回 BAD_ARG；`_hold_cnt` 饱和，不会回绕。
- 已有问题：解决 I8 的 Key 部分，并修掉了长按计数溢出（我们的评审里没有记这条）。
- 处理意见：**修复部分移植**；改名看 **Q10**。

#### C18 新增 `BspEncoder`

- 新功能：定时器编码器模式，16 位回绕处理、累计位置、方向取反。没有 `HAL_TIM_MODULE_ENABLED` 保护（N24）；不是线程安全的（文档有写）。
- 处理意见：**问 Q11**。

### 2.3 Service

#### C19 CAN 节点改成 `CanRxNodeT<CanT>` / `CanTxNodeT<CanT>`（仅头文件模板）

- 语义和我们一样：F3（共帧降频）没解决，F6（回退缓冲没人消费）没解决；节点上限改成按 CAN 类型分别计数；发送节点的互斥量从 CMSIS `osMutex` 换成 `xSemaphoreCreateMutex`。
- 新问题：N16（见 C7）。
- 处理意见：跟随 Q1 / Q2；N16 要修。

### 2.4 Device

#### C20 DjiMotor

- 改动：`DjiMotor<type, CanT>` 模板；`angle_multi_round` 改为 double（`fw:code/service/motor_definition.hpp:32`）；状态判断改用 `sys_state`。
- 已有问题：~~解决 F11 的精度部分~~ **（2026-11-04 更正）F11 没有解决**：字段类型改成了 double，但 `DataUnpack()` 赋值时写的是 `static_cast<float>(angle)`（`fw:code/device/dji_motor.cpp:311`），存进去的仍然是 float 精度。F5（撕裂读）、I4（离线超时不能配置）、I6（`DataUnpack` / `FillData` / `lvboData` 的命名）都没有解决。
- 新问题：N23（只有 `<16,16>` 的实例化）。另外，`DataUnpack()` 的 try-lock 失败时返回 `BUSY`（`:280`），`dispatch()` 把它当成“没人认领”（`fw:code/service/can_rx_node.hpp:142`），这一帧就被放进回退缓冲。这和 F6 是同一类问题：回调返回值把“不是我的帧”和“处理失败”混成了一种情况。
- 处理意见：**保留 double，并去掉那次 float 转换**；F5 / I4 按评审里的方案做；F5 去掉互斥锁之后，这条 BUSY 路径也就不存在了。

#### C21 BMI088 / Buzzer

- 只把 Config 改成聚合体（C2），BMI088 构造时 cfg 不能再省略。F1（零四元数）、A3（启动前 `HAL_Delay`）、F8 都没有解决。
- 处理意见：跟随 Q2。

#### C22 新设备：`DeviceMotor`、`DeviceServo`、`DeviceR9dsT`、`DeviceEmmv5GroupT` + `emmv5_protocol`

| 设备 | 功能 | 备注 |
| --- | --- | --- |
| `DeviceMotor` | 有刷直流电机：H 桥双 PWM + 编码器 + 速度环 PID，cm/s 换算，按时长运行，`pid_frame()` 生成 VOFA JustFloat 帧 | 机械参数是“轮径 / 线数 / 减速比”，偏小车场景；PID 是每拍量纲（文档已写） |
| `DeviceServo` | PWM 位置舵机：角度 ↔ 脉宽，限速，梯形加速规划，trim | 依赖调用方周期调 `update()` |
| `DeviceR9dsT<Uart>` | 乐迪 R9DS（SBUS，100 kbps 8E2 反相）：切帧、16 通道、摇杆 / 开关语义、自带在线判定 | **不是 RM 标准的 DT7 / DR16（DBUS）**；查询接口返回实时引用（和 F5 一样会撕裂读） |
| `DeviceEmmv5GroupT<Uart>` + `emmv5_protocol` | 张大头 Emm_V5 步进闭环驱动：单串口多机、MMCL 多机命令、扫描器、到位位图、每台电机一个 Online | 体量最大（约 1900 行）；串口缓冲尺寸写死 128，和 `BspUartT` 的模板参数耦合 |

- 共同点：都没有上板测试记录；都没有 HAL 模块保护（N24）；接口风格（聚合体 Config、`DeviceXxxT<Uart>`）跟随 C2。
- 处理意见：**问 Q12**。

### 2.5 Algorithm

#### C23 PID / QuaternionPID / Matrix

- PID：删掉 `print()` 桩函数、去掉 `calc_input` 的 virtual（解决 I7 里“有虚函数却没有虚析构”）；新增 `get()` 返回 `{target, feedback, output}`。F4（链式配置时积分没有上限）、F1 的 NaN 防护都没有解决。
- QuaternionPID 拆到 `quaternion_pid.{hpp,cpp}`，实现没变。
- **Matrix 的 `_data` 改成 public**（`fw:code/algorithm/matrix.hpp:282-283`），只是为了去掉 `friend` 声明，失去了封装（I6 风格问题以外又多了一条）。
- 处理意见：**移植** PID 的前两项和 `get()`，以及 QuaternionPID 拆文件；**舍弃** `_data` 公开（保留 `friend`）；F1 / F4 按评审里的方案做。

### 2.6 模板与工具

#### C24 `integrate.py`（670 → 1260 行）与模板

- 新功能：生成 / 合并 `.vscode/{settings,tasks,launch}.json`（保留 JSONC 注释）；复制 `Flash/` 烧录脚本（OpenOCD / J-Link / DAPLink）；生成 `.clang-format` 和 `.clangd`（屏蔽 `Drivers/`、FreeRTOS、`Core/Inc` 的诊断）；按 `.ioc` 的 `Mcu.Name` 识别 H723 / H743 / H750 / H745，并挑选 SVD；`--board-dir`；链接脚本插入点改为“离 `.data` 最近的段尾”；检查 `.dma_buffer` 相关符号。
- 已有问题：D1 没有解决（仍然不检查 `TICK_INT_PRIORITY`、FDCAN Message RAM、`MasterKeepIOState`、DMA 模式）。
- **新问题 N25**：`fw:templates/user/app/app_main.cpp:46` 定义了强符号 `StartDefaultTask`，用来覆盖 `freertos.c` 里的实现。只有 CubeMX 把 defaultTask 的 “Code Generation Option” 设成 **As weak** 时才成立；默认设置下会重复定义、链接失败。
- 模板里的 `board_instances.hpp` 示例写着“`BSP_SPI_INSTANCES(X)` 打开 SPI 时改成……”，示例本身少了续行符。
- 处理意见：**问 Q14**（和第 5 阶段的 Web UI 一起考虑）。

### 2.7 文档

#### C25 文档结构

- **更好懂的地方**（建议采用）：
  - `docs/接入指南.md`：每条硬性要求都写了“为什么”，CubeMX 逐项设置，自动 / 手动接入步骤，自查清单，常见问题；
  - `docs/code/`：六层各一份文档，有分层依赖图、启动顺序图、依赖矩阵（PNG + SVG，由 `tools/draw_diagrams.py` 生成）；
  - `docs/常用接口.md`：UART / CAN / 电机 / PID / 系统状态的速查；
  - `docs/编辑器配置.md`：`.vscode` 每一项的作用；
  - 板级模板的文件头写得像教程（四个文件分工、初始化时序、三条硬规矩、加一个外设的完整步骤）；
  - `docs/service_test/`：有用例表、判据和原始结果的实板测试记录，格式值得照着用。
- **问题**：
  - **N26** 代码注释里引用了 10 个不存在的文档：`bsp_uart.md`、`bsp_can.md`、`bsp_pwm.md`、`bsp_key.md`、`bsp_dwt.md`、`device_servo.md`、`device_r9ds.md`、`device_emmv5_group.md`、`emmv5_protocol.md`、`status.md`；
  - 有错误说法：接入指南写“任务、队列、事件组都走 cmsis_os2.h”（EventState 已经改用原生事件组）；“时基中断优先级”的理由写成“被长中断挡住会让超时失真”，真正的原因（`HAL_DMA_Abort` 竞争、A3 的启动前延时）都没写；常用接口写 `section(".dma_buffer")`，模板却要求 `.dma_buffer.<变量名>`；
  - （2026-11-04 复核补充）代码和模板里还有这些不一致：
    - README 写 sys_task “每 2 s 打印一次”（`fw:README.md:90`），代码是 5 s；
    - `templates/user/board/board_cfg.cpp:21` 还写着“DWT 第一个”，实际 DWT 已经由 `system_init()` 初始化；`:47-48` 的示例把 `DeviceServo`（PWM 舵机）当成挂在 CAN 上的设备，`damiao` 又对应到 `DeviceEmmv5Group`（Emm_V5 是张大头的串口步进驱动，不是达妙）；
    - `demo_task.cpp` 和 `app_task.hpp`、`app_main.cpp:75` 都说 monitor “每秒”打印，代码是每 10 s（`demo_task.cpp:67`）；
    - `bsp_spi.cpp:21` 说“见 bsp_uart.cpp 同名函数”，但 `bsp_uart.cpp` 里并没有 `ms_to_ticks()`，这正好是 N9 的成因；
    - `bsp_spi.hpp:22` 的用法示例写的是非 inline 实例加 `section(".dma_buffer")`，和模板规则冲突；
    - `integrate.py:1251` 的收尾提示让用户“在 `StartDefaultTask()` 里 xTaskCreate”，而模板约定在 `app_init()` 里创建；
    - `hardware_config.h` 写“`CAN_RX_NODE_MAX` ≤ 255，计数为 uint8_t”，实际计数类型是 `uint32_t`，上限也是按 `CanT` 类型分别计的，不是“全部总线合计”；
  - sys 任务优先级的说明前后矛盾（见 C8）；
  - `docs/bsp_test/` 测的是我们的旧代码，fork 现在的 UART / CAN 恰恰把这些修复退回去了，读者会误以为“已经实测”；
  - 厂商 PDF 仍然随仓库分发（A10）。
- 处理意见：**采用 fork 的文档结构**，内容按最终代码重写（**Q16**）。

### 2.8 独立复核新增的问题（N27~N37，2026-11-04）

方法和前文一样：只读 fork @ `7ef6bfa` 的代码，没有编译、没有上板。级别和可信度的写法沿用 `stm32_bsp_review.md`。这些问题都是 fork 自己的代码里的，和 `bsp:` 无关；凡是会被阶段 1.1 / 1.2 回填覆盖掉的，都注明了“回填后核对”。

| 编号 | 级别｜可信度 | 位置（`fw:`） | 问题 | 处理 |
| --- | --- | --- | --- | --- |
| **N27** | 中｜确认（后果待验证） | `code/tasks/sys_task.cpp:40`；`code/bsp/bsp_can.hpp:20`；`bsp_can.cpp:207` | **CAN `tx_recover()` 没有任何调用者。** 头文件写的是“`tx_recover()` / `service_recovery()` 由 sys_task 每 10 ms 调用”，但 sys_task 只展开了 `service_recovery()`。TX-empty 中断按需开关（Q9 = B）要靠 `tx_recover()` 补救丢掉的唤醒；没有它，一旦中断没开起来，软件缓冲又积到满，`send()` 每次都返回 `FULL`。而 `send()` 只在入队成功后才调 `_start_transmission()`，所以总线会**一直沉默**，电机全部收不到指令。`tx_stall_recover` 计数也会永远是 0，阶段 3 的“丢唤醒统计”看到 0 并不能说明没有丢唤醒 | sys_task 的 X-macro 加上 `(void)name.tx_recover();`；`send()` 返回 `FULL` 前也用 `_start_transmission(0)` 踢一次；阶段 3 人为关一次 TX-empty 中断，确认计数会增加、总线能恢复 |
| **N28** | 中｜确认 | `code/bsp/bsp_can.cpp:126, 213-238` | **`CAN_RX_ENABLE = 0` 时 `receive()` 用不了。** `_rx_return_buffer` 总是会创建，`receive()` 先在它上面等满 `timeout`，然后才不等待地读一次原始缓冲。没有 `can_rx_task` 时回退缓冲永远是空的，所以默认的 `portMAX_DELAY` 会**永久阻塞**；给了有限超时，每次调用也要先空等一个超时，才能拿到一帧。按 N21 的原方案“只读回退缓冲”去改，这条路径就彻底断了 | 编译期分开处理：`CAN_RX_ENABLE = 1` 时只读回退缓冲（N21）；为 0 时不创建回退缓冲，只读原始缓冲。编译矩阵（阶段 3）加一个 `CAN_RX_ENABLE = 0` + `receive()` 的用例 |
| **N29** | 中｜待验证 | `bsp_can.cpp:492`（ISR）与 `_start_transmission()`（任务） | **发送 MessageBuffer 有两个读者。** `_tx_lock` 只能挡住其他任务，挡不住 TX-empty ISR：任务正在 `xMessageBufferReceive` 时，上一帧发完、FIFO 变空就会进 ISR，ISR 也去读同一个缓冲。FreeRTOS 要求单读者，这里可能读出错帧或者破坏长度字段 | 和 N20 一起修：任务侧“判空位 → 取帧 → 写 FIFO”放进临界区（或者屏蔽本路 FDCAN 中断），或者干脆只让 ISR 取帧，任务侧只负责触发 |
| **N30** | 中｜确认 | `code/bsp/bsp_uart.cpp:26-78`、`bsp_can.cpp:28-77`、`bsp_spi.cpp:63-97` | **库外的同类外设收不到 HAL 回调。** 库对 `HAL_UART_TxCpltCallback` / `HAL_UARTEx_RxEventCallback` / `HAL_UART_ErrorCallback` / `HAL_SPI_TxRxCpltCallback` / `HAL_SPI_ErrorCallback` / `HAL_FDCAN_*` 都给了强定义，`else if` 链后面没有 `else`。宿主自己用 HAL 管的串口（比如厂商模块代码里的 `HAL_UART_Transmit_IT`）回调会被**静默吞掉**，宿主自己又不能再定义这些回调（重复定义）。阶段 4 只写了“宿主不能定义的回调清单”，没有给出替代办法 | 每个回调末尾加 `else { icbsp_unhandled_xxx(h); }`，默认是空的弱函数，由宿主覆盖；接入指南写明这一点，也说明可以改用 `USE_HAL_*_REGISTER_CALLBACKS` 的方式 |
| **N31** | 低｜待验证（回填后核对） | `code/bsp/bsp_uart.cpp:475-530` | UART `tx_recover()` 的“中止 + 续发”**不持 `_tx_lock`**：sys_task 中止并续发 `_tx_buffer + sent` 的时候，另一个任务的 `send()` 可能正在 `start_transmission()` 里改写 `_tx_buffer` 和 `_tx_inflight_len`。和 N12 是同一类时序问题 | 回填 `bsp:` 的状态机时一并核对，恢复路径要拿锁（取不到就下个周期再试） |
| **N32** | 低｜确认 | `code/bsp/bsp_spi.cpp:105-128` | `BspSpi` 没有遵守 C2 的实例约定：构造函数不是 `constexpr`，又自定义了析构函数。按模板写成 `inline` 实例时，会变成动态初始化，每个 TU 一份初始化代码，外加 atexit 注册。`init()` 第二次调用直接返回 `BAD_ARG`，CAN / UART 却可以重入。和其他 BSP 不一致 | 改成 `constexpr` 构造 + 平凡析构 + `deinit()`；`init()` 的重入语义和 CAN / UART 统一 |
| **N33** | 低｜确认 | `code/device/device_emmv5_group.cpp:131, 173, 296, 400, 557` | Emm_V5 的接口细节：① `get_position()` / `get_status()` **读完就清标志**，和 Q10 定下的“`get_` 是没有副作用的查询”冲突；② `_send_for()` 等三处把 `pdMS_TO_TICKS(TX_TIMEOUT_MS)` 当毫秒传给 `send()`，换算了两次（1 kHz tick 下碰巧数值不变）；③ `lock_bus()` 直接用 `pdMS_TO_TICKS(timeout_ms)`，和 N9 一样，`portMAX_DELAY` 会溢出；④ 只支持固定 `0x6B` 校验，Emm_V5 的 XOR / CRC-8 模式不能用，RS485 上有噪声时只能靠帧格式匹配来防错 | ① 改名为 `take_position()` / `take_status()`；②③ 统一用 `ms_to_ticks()`；④ 校验方式做成 `Config` 的一项（阶段 2 并入核心库时一起做） |
| **N34** | 低｜确认 | `code/device/device_r9ds.hpp:139-140` | R9DS 的 `Diag::byte_cnt` / `frame_cnt` 是 `float`，到 2^24 之后再 `+1` 就不变了。SBUS 每秒约 1.8 kB，`byte_cnt` 大约 2.6 h 后就停止增长，诊断失真（`is_online()` 只判断 `frame_cnt > 0`，功能本身不受影响） | 改成 `uint32_t` |
| **N35** | 低｜确认 | `code/service/can_tx_node.hpp:248`；`code/bsp/bsp_can.cpp:354-358` | CAN 节点与恢复的边角：① 发送节点的槽位全部 `unregist` 之后，节点还留在链表里，`due` 也不看 `bufferRegister`，每 5 ms 照样发一帧全 0；② `service_recovery()` 在临界区里 `xMessageBufferReset()`，如果有任务阻塞在这个缓冲上（例如用户 `receive()` 在等回退缓冲），reset 会失败，而且没有任何提示，积压的帧也清不掉 | ① 槽位清空后从链表摘除，或者 `due` 加上 `bufferRegister != 0`；② 检查 reset 的返回值并计数，或者改成逐帧读空 |
| **N36** | 低｜确认 | `code/service/status.cpp:140`；`code/service/online_check.cpp:127` | Service 的边角：① `EventState` 没有清除错误的接口，OK 位一旦被清就再也回不来；`error_code()` 返回的是**编号最小**的错误，不是最近一次的错误。Q6 要默认建树，这两点会直接影响树上错误的含义；② `Online::update()` 在**一个**临界区里遍历整条链表，节点数随电机数量增长（每台 DjiMotor、每台 Emm_V5 各一个节点），关中断的时间也跟着线性变长 | ① 建树时定下错误的生命周期（粘滞还是可清），补上 `clear_error()`，`error_code()` 的含义写进文档；② 每个节点单独进出临界区，或者在 tick 回调里只做比较、不做遍历 |
| **N37** | 低｜确认 | `code/bsp/bsp_encoder.cpp:44`；`code/device/device_servo.hpp:184`；`code/algorithm/pid.cpp:112` | 其余接口细节：① `BspEncoder::read_delta()` 按 16 位回绕计算，只有 CubeMX 里 ARR = 0xFFFF（32 位定时器则是 0xFFFFFFFF）时才对，文档没有写这条要求；② `DeviceServo` 的跨任务变量 `_target_deg` / `_enabled_req` 是普通 `float` / `bool`，“单条 32 位存储天然原子”的前提是编译器既不重排也不缓存，标准不保证这一点；③ `PID::calc(target, feedback, df_dt)` 的 `df_dt` 实际是“每拍增量”（QuaternionPID 也是这样传的），名字看起来像每秒的导数，用户直接传 rad/s 时微分项的量级会差一个控制频率的倍数 | ① `init()` 检查 ARR，不符合就返回 `BAD_ARG`；② 改用 `std::atomic<float>` / `std::atomic<bool>`（Cortex-M7 上是无锁的）；③ 参数改名为 `d_feedback_per_step`，或者在 Config 里加 `dt` 后统一成按秒计（和 F4 的“ki / kd 按每周期计”一起定） |

---

## 3. 待你回答的问题

每条给出选项和我的倾向，可以直接回“Q1 选 A”这样的格式，也可以写自己的方案。第 2 阶段（加入新功能）的条目按这些答案决定。

**Q1 实例怎么分发给 HAL 回调和库任务？**（C1、C14、C19）
- A. 保持登记表（`init()` 时自动登记，库不依赖宿主头文件）——**我的倾向**；
- B. 改用 fork 的 X-macro `board_instances.hpp`（零 RAM，但漏写会静默失效，库依赖宿主头文件）；
- C. 登记表 + 编译期上限检查（保留 A 的安全性，登记表容量由配置决定）。
- 说明：如果第 5 阶段由 Web UI 根据 `.ioc` 自动生成 `board_instances.hpp`，B 的“漏写”风险会小很多；但依赖倒置（库不能单独编译 / 测试）仍然存在。
- **User Answer:**B

**Q2 C++ 标准？**（C2、C3）
- A. 保持 C++11——宿主门槛最低；
- B. C++17（inline 变量可以用，指定初始化不行）；
- C. C++20（采用 fork 的 inline constexpr 实例 + 指定初始化，`.dma_buffer` 必须用 fork 的可加载方案）。
- 我的倾向：A 或 B；如果你很看重 `{.huart = ...}` 这种写法的可读性，就选 C。
- **User Answer:**C

**Q3 配置方式？**（C4）
- A. 库内默认值 + `static_assert`，宿主可选覆盖——**我的倾向**；
- B. fork 的“宿主必须提供完整配置”（N15 的摆设宏要清理）。
- **User Answer:**B

**Q4 要不要功能开关**（`SYS_TASK_ENABLE` / `CAN_RX_ENABLE` / `CAN_TX_ENABLE` / `UART_PATROL_ENABLE`）？（C5）
- A. 要，放进默认值头文件，并写清楚关掉之后会失去什么——**我的倾向**；
- B. 不要。
- **User Answer:**A.且后续web ui将能够通过生成相关宏定义来功能裁切

**Q5 sys_task 的节拍与行为？**（C8）
- 周期：A. 保持 1 ms / B. 改成 10 ms / C. 做成可配置的宏（默认 1 ms 还是 10 ms？）；**User Answer:**B
- 开机先等 1 s：要 / 不要（我倾向不要）；**User Answer:**No
- 周期打印循环耗时：A. 不要 / B. 改成库提供的可选回调或计数变量（倾向 B，不依赖宿主的宏）。**User Answer:**B

**Q6 用 `EventState` 取代 `sysEvent` / `SysFlag*`？**（C7）
- A. 采用（接口全部改名：`SysFlagWaitRunning()` → `sys_state.wait_running()` 等），同时修掉 N16——**我的倾向**；
- B. 保持现有的 C 风格接口；
- 如果选 A：子系统的父子树（例如每条 CAN 一个 EventState）要不要默认建起来？
- **User Answer:**A. 默认建立 

**Q7 Online 改成毫秒语义、`isOnline` 改名为 `is_online`？**（C9）
- A. 两者都采用——**我的倾向**；
- B. 只采用毫秒语义，保留旧名字（或者保留一个过渡别名）。
- **User Answer:**A

**Q8 UART 从 fork 移植哪些能力？**（C12，可多选）
- [x] 中断收发 Transport（`uart_mode::it`）；
- [x] `UartDir` 取代 `transmit_enable`；
- [x] RX / TX 缓冲尺寸分开（顺便解决 I1：显式实例化改成 header-only，或者由配置列表生成）；
- [x] `hw_error_cnt`；
- [x] `check_format()`；
- [x] `deinit()` + 平凡析构；
- [x] 查询接口加 `get_` 前缀。
- 不管选哪些，底层收发状态机都保持我们的实现。

**Q9 CAN 的接口取舍？**（C13）
- 队列深度：A. 保持宏 / B. 改成模板参数 `BspCanT<RX, TX>`；**User Answer:**B
- TX-FIFO-EMPTY 中断：A. 保持常开 / B. 随软件缓冲动态开关（fork 的做法，需要实测验证没有丢唤醒）；**User Answer:**B
- 库是否占用 `HAL_FDCAN_ErrorStatusCallback`（用来计错误被动 / 警告）：要 / 不要（要的话宿主不能再定义这个回调）。**User Answer:**yes

**Q10 GPIO / Key / PWM 改名？**（C15~C17）
- `write/read` → `set_level/get_level`；`poll` → `update`；`LONG_PRESS` → `LONG`；PWM 查询接口加 `get_` 前缀。
- A. 全部采用，并在编码规范里统一“查询一律 `get_`”；
- B. 保持现有名字——v1.0.0 之前改名成本最低，所以我倾向在两者中尽早定下一个。
- **User Answer:**A

**Q11 要不要 DWT 和编码器？**（C11、C18）
- DWT：A. 要（由库持有，`system_init()` 初始化，并用于修 A3）——**我的倾向**；B. 不要；**User Answer:**A
- 编码器：A. 要（补上 HAL 保护）；B. 不要；C. 放进可选扩展目录。**User Answer:**A**

**Q12 四个新设备怎么处理？**（C22，每个单独回答：核心库 / 可选扩展目录 / 不要）
- `DeviceMotor`（有刷直流 + 编码器）：——**User Answer:**要
- `DeviceServo`（PWM 舵机）：——**User Answer:**要
- `DeviceR9dsT`（乐迪 SBUS）：—— （另外：要不要补 RM 常用的 DT7 / DR16 DBUS 接收机？）**User Answer:**要。DR16和ET08接收机放undo里面
- `DeviceEmmv5GroupT`（张大头步进）：——**User Answer:**要
- 补充：你在 Web UI 的说明里提到了 **dm_motor（达妙电机）**，两个库里都还没有。要不要作为新设备排进第 2 阶段？**User Answer:**Yes

**Q13 目录布局？**（C6）
- 源码目录：A. 保持 `bsp/ service/ device/ algorithm/` 在仓库根 / B. fork 的 `code/` + 单独的 `tasks/`；**User Answer:**B.另外，device和service中的目录可以有子目录的细分，例如`device/motor`,`device/sensor`
- `examples/dm_mc02`：A. 保留（回归工程，但 submodule 体积大）/ B. 删掉，以 MC02_with_submodule 作为回归宿主 / C. 移到单独的仓库。**User Answer:**B. 不提供examples, 最多只在README中给出example的仓库链接

**Q14 接入工具今后的形态？**（C24、第 5 阶段）
- A. 先把 fork 的 `integrate.py` 扩展（`.vscode` / `Flash/` / `.clangd` / 型号识别）合进来，Web UI 调用这个脚本；
- B. Web UI 直接用 Node 重写全部接入逻辑，`integrate.py` 只作为命令行备选，最终删掉；
- C. 两者共用一份机器可读的清单（manifest），Python 和 Node 各自实现——我倾向 A 起步、最终走 C。
- **User Answer:**A

**Q15 仓库名与两个仓库今后的关系？**
- 名字：`stm32_bsp` / `stm32_framework` / 其他？（CMake 目标名、文档前缀跟着变）**User Answer:**你在`stm32_bsp` / `stm32_framework`/`ICBSP`三个名字中权衡一个
- 关系：A. fork 的有用内容按本文合回 stm32_bsp，fork 停止维护；B. 以 fork 为主线，把本仓库的修复反向移植过去；C. 两个仓库各自维护。**User Answer:**A和B都行，以你怎样方便完成任务来选择。完成后未维护的那个库将删除
- 我的倾向：A。fork 的 UART / CAN 回退太多，以它为主线需要把 N1~N23 全部重修一遍。另外请确认你和 Rh-i 之间对 fork 的分工（这会影响第 7 阶段的发布方）。**User Answer:**该仓库的发布方署名: INVC & CUBOT (Reason: Rh-i为INVC组织的负责人， ChoseB为CUBOT组织的技术负责人)

**Q16 文档采用 fork 的结构吗？**（C25）
- A. 采用（接入指南 / 常用接口 / 编辑器配置 / `docs/code/` 分层文档 + 架构图 / 实测记录模板），内容按最终代码重写——**我的倾向**；
- B. 只挑接入指南和常用接口；
- C. 保持现有的 README 单文件结构。
- **User Answer:**A

---

### 3.1 回答汇总与由此确定的方案（2026-11-04）

| 问题 | 你的回答 | 落地方式 |
| --- | --- | --- |
| Q1 | B（X-macro） | 宿主提供 `board_instances.hpp`（由 Web UI / `integrate.py` 生成）。另加两道防线：① 每个 BSP 的 `init()` 遍历 X-macro 列表，自己不在表里就返回 `BAD_ARG`，N13 从“静默失效”变成“初始化时断言停机”；② SPI 在 `init()` 里查同一句柄是否重复绑定（补回 C1 丢掉的检测）。N14：sys_task 不再用 `print_uart`（见 Q5），库对宿主只依赖 `board_instances.hpp` 和 `hardware_config.h` 两个文件，接入指南里写明 |
| Q2 | C（C++20） | 采用 inline constexpr 实例、指定初始化和 fork 的可加载 `.dma_buffer`（A7 由此解决，C3 的第三方案作废）。`.dma_buffer.<变量名>` 的段名约定封装成宏（`ICBSP_DMA_BUFFER(name)` → `section(".dma_buffer." #name)`），不再手写。宿主 CMake 必须设 `CXX_STANDARD 20`（本机 arm-none-eabi-gcc 15.3 支持）。回填进来的我们的代码要按 C++20 改掉 volatile 复合赋值 |
| Q3 | B（宿主必须提供完整配置） | 为了抵消 C4 记下的两个缺点：范围检查的 `static_assert` 搬进库（`code/service/system/config_check.hpp`），宿主删掉也照样检查；每个必需的宏缺失时 `#error "缺少 XXX，见 templates/…"`，库升级新增宏时，报错直接告诉用户要补什么。清理 N15：`CAN_BUSOFF_RETRY_MS` 接回代码；`UART_ENABLE_FIFO` 随 UART 回填恢复作用；`CAN_RX_QUEUE_FRAMES` / `CAN_RX_RETURN_FRAMES` / `CAN_TX_QUEUE_FRAMES` 已被模板参数取代，删掉 |
| Q4 | A，后续由 Web UI 生成宏来裁剪 | Q3 选了 B，没有默认值头文件了，开关放进宿主必须提供的 `hardware_config.h`；对应的 HAL 模块没开时，库内强制置 0。模板注释写清关掉的后果：`SYS_TASK_ENABLE = 0` → Online 不更新、Bus-Off 不恢复、串口不巡检；`DjiMotor` / dm_motor 要求 CAN 收发都开（`static_assert`）。阶段 5 由 Web UI 按勾选生成这些宏 |
| Q5 | 10 ms；不等 1 s；B | 循环耗时做成库内计数（`sys_task_stats`：最大 / 平均耗时，用 DWT 计时），另提供可选回调（例如 `system_set_report_hook()`，默认为空），库不引用宿主的宏。连带：`CAN_BUSOFF_RETRY_MS` 和串口巡检阈值要按 10 ms 粒度检查（`static_assert` ≥ 10） |
| Q6 | A，默认建树 | `sys_state` 为根；每个 BSP 实例（CAN / UART / SPI）一个 `EventState` 挂在根下；设备挂在它所用的总线实例下（共三层）。传播规则：子节点出错只置父节点的错误位，**不清 RUNNING**（运行期错误不能让整个系统停摆，和 N16 是同一个教训）；`init_error()` 只在初始化期有效 |
| Q7 | A | fork 已经是毫秒语义 + `is_online`，保留 |
| Q8 | 全选 | 这些接口 fork 里都已经有了：保留接口，底层收发状态机换成我们的（阶段 1.1 回填） |
| Q9 | B / B / 要 | 保留 `BspCanT<RX, TX>`；保留 TX-empty 中断动态开关，阶段 3 专门测丢唤醒；库占用 `HAL_FDCAN_ErrorStatusCallback`，把它加进“宿主不能再定义的回调”清单（接入指南、宿主 CLAUDE.md）。N23（只实例化了 `<16,16>`）和 I1 一起改成 header-only |
| Q10 | A | 全部改名，编码规范加一条：**查询一律用 `get_`，布尔判断用 `is_` / `has_`**。按这条规则，fork 把 GPIO 的 `get_port/get_pin` 改成 `port/pin` 要**退回 `get_port/get_pin`**；`DjiMotor::data()`、PWM 等其余查询接口也按规则过一遍 |
| Q11 | DWT：A；编码器：A（更正，原为 C） | DWT 保留（fork 里已经由库持有），修掉 `get_time_s()` 的线程安全和 7.8 s 回绕问题（C11），并用它修 A3。编码器直接进核心库 `bsp/`，补上 `HAL_TIM_MODULE_ENABLED` 保护（没开 TIM 就不编译）。**不设可选扩展目录** |
| Q12 | 四个都要；DR16 / ET08 放进 undo；dm_motor 要 | DeviceServo / DeviceR9dsT / DeviceEmmv5GroupT 进核心库（放进 `device/` 的子目录）；DeviceMotor 也进核心库（`device/motor/`，Q17）；dm_motor 排进阶段 2；DR16 / ET08 接收机记进 undo 的“待排期”，不进 v1.0.0 |
| Q13 | B，`device` / `service` 可分子目录；删 `examples/`，README 给示例仓库链接 | 目录草案见下；示例仓库链接先空着（Q18） |
| Q14 | A | fork 版 `integrate.py` 是唯一的接入引擎；Web UI 是它的图形前端，通过 `--json` 输入输出调用它。清单 `tools/manifest.json` 由 Python 读取。修 N25 |
| Q15 | 名字三选一由我定；A / B 由我定；署名 INVC & CUBOT | 名字 **ICBSP**，主线 **B（以 fork 为主线）**，理由见下 |
| Q16 | A | 采用 fork 的文档结构，内容按最终代码重写 |

**Q15 的权衡**

- **主线选 B。** Q1 / Q2 / Q3 / Q13 / Q14 / Q16 都选了 fork 的方案。以 fork 为底，要回填的只有 UART / CAN 的内部实现和一批修复，改动集中在两个驱动里，而且有 `bsp:` 的对照位置可循。反过来以本仓库为底，就得把 X-macro、C++20、聚合体 Config、可加载 `.dma_buffer`、`code/` 布局、新版 `integrate.py` 和整套文档全部移植一遍，改动面要大得多。代价是：阶段 1.1 / 1.2 完成之前，fork 的 UART / CAN 还带着 N1~N12、N17~N23，不能上车用。
- **名字选 ICBSP。**

| 候选 | 优点 | 缺点 |
| --- | --- | --- |
| `stm32_bsp` | 现在的名字 | 和 STM32Cube 自带的 `Drivers/BSP`（ST 官方板级驱动）撞名，读者容易混淆；库里实际还有 service / device / algorithm / tasks，不只是 BSP；GitHub 上同名仓库很多，搜不到 |
| `stm32_framework` | fork 现在的名字，主线选 B 时不用改名；表达了“不只是 BSP” | 同样太泛，搜不到；没有给宏和 CMake 目标提供独有的前缀 |
| `ICBSP` | 对应署名 INVC & CUBOT，名字唯一、好搜；提供天然的前缀 `ICBSP_`。Q3 选 B、Q4 选 A 之后，宿主的 `hardware_config.h` 里会有一批不带前缀的宏（`SYS_TASK_ENABLE`、`CAN_RX_ENABLE`、`UART_ENABLE_FIFO` 等），容易和 HAL 或用户代码的宏撞名 | 要改一次名；字面意思不直观，README 第一行要解释（INVC & CUBOT Board Support Package） |

  v1.0.0 之前本来就要做一轮破坏性改名（Q10），一起改成本最低；Web UI 生成配置之后，宏名变长对用户也没有负担。落地：仓库名 `ICBSP`，CMake 目标 `icbsp`，配置宏 / 功能开关 / 版本宏统一加 `ICBSP_` 前缀，`LICENSE` 的版权人写 INVC & CUBOT。

**目录草案（Q13）**

```
code/
├── bsp/                  bsp_can  bsp_uart  bsp_spi  bsp_pwm  bsp_gpio  bsp_key  bsp_dwt  bsp_encoder
├── service/
│   ├── system/           system  status (EventState)  fault_record  config_check
│   ├── comm/             can_rx_node  can_tx_node
│   ├── monitor/          online_check
│   └── definition/       motor_definition  imu_definition
├── device/
│   ├── motor/            dji_motor  dm_motor  device_motor  device_emmv5_group  emmv5_protocol
│   ├── sensor/           device_bmi088
│   ├── remote/           device_r9ds  （以后加 dr16、et08）
│   └── actuator/         device_servo  device_buzzer
├── algorithm/            pid  quaternion  quaternion_pid  matrix  rls  vqf
└── tasks/                sys_task  can_rx_task  can_tx_task
```

- 包含路径只加 `code/`，一律写成 `#include "device/motor/dji_motor.hpp"`：不怕重名，分层关系从包含语句就看得出来（“下层不出现上层路径”可以做成 CI 检查）。
- fork 的 `code/protocol/` 只有 emmv5 一个协议，并进 `device/motor/`；以后协议多了再单独成层。

**由上面的答案引出、还需要你回答的问题**

**Q17 DeviceMotor 放在哪里？** 它依赖 `BspEncoder`，而编码器按 Q11 放进了可选扩展目录，核心库不能依赖扩展。
- A. DeviceMotor 也放进 `code/ext/`，跟着编码器开关走（它偏小车场景，RM 车上少用）——**我的倾向**；
- B. 编码器改进核心库 `bsp/`（推翻 Q11 的 C），DeviceMotor 留在核心；
- C. DeviceMotor 留在核心，编码器部分做成可选（没开编码器扩展时只能开环）。
- **User Answer:** Q11 更正后不存在冲突：编码器和 DeviceMotor 都直接放进核心库（`bsp/bsp_encoder`、`device/motor/device_motor`），不设扩展目录。

**Q18 README 里的示例仓库链接指向哪里？** 删掉 `examples/` 之后要给一个示例仓库，而 MC02_with_submodule 现在没有远端。
- A. 把 MC02_with_submodule 推到 `cubot-refactor-code-plan` 组织下当示例仓库——**我的倾向**；
- B. 另建一个干净的示例仓库（不带 `app/test/` 测试代码）；
- C. 不给链接。
- **User Answer:** 先空着。Web UI 完成后，阶段 6 开一个新工程做回归测试，通过回归的那个测试宿主工程就作为新的示例，到时再把链接写进 README。

**Q19 `STM32_BSP` 删除还是归档？** 本工作区历史提交里的 submodule 指针都指向 `STM32_BSP`，删掉之后旧提交就检出不了库代码（`docs/bsp_test/` 的实测记录对应的代码也就找不到了）。
- A. 在 GitHub 上归档（只读，保留历史），不删——**我的倾向**；
- B. 删除，接受旧提交不能完整检出；删除前把 10-17 修复的提交记录整理进 `CHANGELOG.md`。
- **User Answer:** A，归档。

---

## 4. 七阶段路线（主线 = fork，已合并 `undo.md` 的现有待办；2026-11-04 定稿）

- 阶段 1~4 的库改动都在 fork 仓库（`cubot-refactor-code-plan/stm32-framework`，阶段 2 改名为 ICBSP）上做。`bsp:` 位置是回填时的对照来源。本工作区新开分支，把 submodule 切过去，作为回归宿主。
- 每个阶段结束时都要满足：Debug 与 -O2（RelWithDebInfo）编译 0 warning。
- Q1~Q19 已全部回答，路线定稿（2026-11-04；Q11 按与 Rh-i 协商的结果更正为“编码器进核心库”）。

### 阶段 1：修复已有问题

**1.0 切换主线**

1. 本工作区新开分支，submodule 指向 fork；宿主改成 C++20，按 fork 模板补齐 `board_instances.hpp` / `hardware_config.h` / `board_cfg.hpp`，`app/` 适配新接口；编译 0 warning，作为基线。**这时 UART / CAN 还带着 N1~N23，不上板跑通信测试。**
2. `undo.md` 搬进 fork 仓库，`STM32_BSP` 里的这份不再更新；fork 的 `docs/bsp_test/` 注明“对应 STM32_BSP @ 43a34cf 的旧实现”。

**1.1 UART 回填**：保留 fork 的接口（Transport / `UartDir` / RX 与 TX 尺寸分开 / `hw_error_cnt` / `check_format()` / `deinit()` / `get_` 查询），内部收发状态机换成我们的。

1. 修 N1~N12：`_tx_write_lock` + 中断侧不写流缓冲区（N1）、打开 FIFO（N2）、RX 流缓冲区 2 倍（N3）、IDLE 后按剩余计数重算（N4）、按传输进度判断卡死（N5）、先中止再结算（N6）、DMA 句柄修复 + NVIC 保护（N7）、运行中重新 init（N8）、`ms_to_ticks()`（N9）、DMA 句柄与可达性检查（N10）、RS485 软件 DE（N11）、在途长度的时序（N12）。
2. **F2** 调用 HAL 之前先置 `_tx_chunk_active`。
3. **RX 改为 DMA 循环模式**（`undo.md` 保留原文）：直接在回填后的实现上做，不必先在旧仓库做一遍；IT Transport 的接收路径不经过 `HAL_DMA_Abort`，单独确认即可。**TIM8 改回 15 之前要先完成 1.4 的 A3。**
4. **I1** 改成 header-only；恢复 `HAL_UART_MODULE_ENABLED` 保护（N24）。
5. 回填完马上跑一遍 `docs/bsp_test/` 的 UART / RS485 用例（Debug 与 -O2），确认 10-17 的修复都回来了；完整回归放在阶段 3。

**1.2 CAN 回填**：保留 fork 的 `BspCanT<RX, TX>`、重新 init 前复位与失败回滚、`rx_lost` / `err_passive` / `err_warning` 等诊断、TX-empty 中断动态开关、ErrorStatusCallback、删掉的 `CanMode`、毫秒超时、`const uint8_t *data`；收发与恢复逻辑换成我们的。

1. 修 N17~N22：`FDCAN_REJECT_REMOTE`、按 `TXFQS.TFQF` 判断空位、按 DLC 拷贝、`send()` 放进临界区、`receive()` 只读回退缓冲、`CAN_BUSOFF_RETRY_MS` 接回代码且 Bus-Off 只计一次。
   - 复核新增：**N27** sys_task 调 `tx_recover()`，`send()` 返回 FULL 前也踢一次；**N28** `receive()` 按 `CAN_RX_ENABLE` 区分读哪个缓冲（N21 的修法以此为准）；**N29** 发送缓冲的单读者（和 N20 一起改）；**N35** 节点槽位清空后摘除、`xMessageBufferReset` 检查返回值。
2. **N23** 改成 header-only（连带 DjiMotor、DeviceR9dsT、DeviceEmmv5GroupT 的显式实例化）。
3. **I2** 扩展帧发送、接收节点声明可接受的 DLC；**F3** “凑齐即发”加时限和降频计数；**F6** 回退缓冲可选 / 只计数；F9 的其余部分（`StdFiltersNbr` 写入）。
4. 回填完马上跑一遍 CAN 用例。

**1.3 fork 引入的架构问题**

1. **N13** BSP 的 `init()` 校验自己在 X-macro 列表里；SPI 同句柄重复检测。
2. **N14** sys_task 去掉 `print_uart`（换成 Q5 的计数 + 回调）。
3. **N15** 清理摆设宏（见 3.1 的 Q3）。
4. **N16** 运行期 `regist()` 不再调 `init_error()`；EventState 的传播不清 RUNNING。
5. **N24** TIM / UART 相关的所有模块（含 encoder / servo / motor / r9ds / emmv5）补上 HAL 模块保护。
6. **N25** `integrate.py` 不再生成强定义的 `StartDefaultTask`：检查 `.ioc` 是否设成 As weak，否则改为写进 USER CODE 段转调。
7. `.dma_buffer.<变量名>` 的约定封装成宏（见 3.1 的 Q2）。
8. **N30** 库占用的每个 HAL 回调末尾都转调一个弱函数 `icbsp_unhandled_*()`，让宿主自己的同类外设也能收到回调。
9. **N32** `BspSpi` 改成 `constexpr` 构造、平凡析构、`deinit()`，`init()` 的重入语义和 CAN / UART 一致。
10. **N36** 定下 EventState 错误的生命周期（补 `clear_error()`，写清 `error_code()` 的含义），这一步要在阶段 2 默认建树之前做；`Online::update()` 缩短临界区。
11. 回填 UART 时核对 **N31**（`tx_recover()` 要持锁）。

**1.4 评审里的已有问题**（fork 里同样存在）

1. **F1** 四元数初值改为单位四元数，增加 `attitude_ready()`；`rotation_vector()` / `normalized()` 防零模长；PID 遇到非有限输入时丢弃并计数。
2. **A1** 初始化动作挪出 `configASSERT`（`system.cpp`、模板、编码规范），`configASSERT` 改成 `do { } while (0)`。
3. **F4** PID 积分上限在计算时取实际值（`i_max == 0` 时跟随 `out_max`）。
4. **F5** DjiMotor 提供 `snapshot()`，去掉没用的互斥锁；R9DS 的实时引用同样处理。
5. **A3** fork 里已经有 DWT：BMI088 在调度器启动前改用 DWT 忙等；完成后才能做 1.1-3 的 “TIM8 改回 15” 验证。
6. **A2** 库任务默认优先级按 `configMAX_PRIORITIES` 取并加 `static_assert`（fork 里 sys 是 +6，和 BMI088 同级）。
7. **A4** DMA 可达性按 DMA1/2 和 BDMA 分别判断，合并两份 `dma_reachable`。
8. **A5** `fault_on_exception()` + HardFault / MemManage / BusFault 的 USER CODE 片段。
9. 其余低级别条目：A6（拆开 `status.hpp`，让 `algorithm/` 不依赖 RTOS）、A8、A9、F7 的其余部分、F8、F10、I4、I5、I6 / I7（和 Q10 一起做）、D3、PWM 去掉默认构造（I8 的剩余部分）、DWT `get_time_s()` 的线程安全与回绕（C11：加临界区；sys_task 保留周期性的回绕跟踪，不随 Q5 去掉打印而消失）、N37（编码器 ARR 检查、舵机跨任务变量改 atomic、`PID::calc` 的 `df_dt` 改名或改成按秒计）。
10. **F11** 去掉 `dji_motor.cpp:311` 的 `static_cast<float>`，让 double 字段真正存 double（fork 只改了类型）。
11. fork 已经修好、回填时**不能弄丢**的：F9（重新 init / RF0L）、I3、I8（GPIO / Key）、Key 长按计数饱和、F11 的 double 字段类型（赋值见上一条）、F7（`hw_error_cnt`）、PID 去掉 virtual 和 `print`、A7（可加载 `.dma_buffer`）。
12. 1.1~1.4 做完后，按附录 B 把 N1~N37 逐条核对一遍。

### 阶段 2：加入新功能

1. **改名 ICBSP**（Q15）：仓库名、CMake 目标 `icbsp`、配置宏 / 开关加 `ICBSP_` 前缀、文档前缀、`LICENSE` 版权人改为 INVC & CUBOT。
2. **目录重排**（Q13）：按 3.1 的草案分子目录，包含路径只加 `code/`。`examples/` 在 fork 里已经删掉了；把 `bsp:examples/dm_mc02/Flash/README.md` 里的经验（OpenOCD 必须用 0.12、不要注册 OCTOSPI2 外置 Flash bank）补进 fork 的 `templates/Flash` 和文档；README 的示例仓库链接先空着，阶段 6 再补（Q18）。
3. **配置检查**（Q3）：库内的 `config_check.hpp`（范围 `static_assert` + 缺宏 `#error`），模板同步更新。
4. **功能开关**（Q4）：注释写清后果，依赖关系用 `static_assert` 表达。
5. **sys_task**（Q5）：10 ms；去掉开机 1 s 等待；`sys_task_stats` + 可选回调。
6. **EventState 默认树**（Q6）：根 → BSP 实例 → 设备，按 3.1 的传播规则；照搬 fork 的 81 条实板判据并补充建树相关的用例。
7. **命名统一**（Q10）：GPIO / Key / PWM 改名；“查询 `get_`、布尔 `is_` / `has_`” 写进编码规范并扫一遍所有接口（GPIO 改回 `get_port/get_pin`）。
8. **编码器进核心库**（Q11 更正）：`bsp/bsp_encoder`，补 `HAL_TIM_MODULE_ENABLED` 保护。
9. **设备并入核心**（Q12 / Q17）：DeviceMotor（`device/motor/`）、DeviceServo、DeviceR9dsT（加 `snapshot()`；诊断计数改成整数，N34）、DeviceEmmv5GroupT（去掉写死的 128 字节串口缓冲尺寸，跟随 `BspUartT` 的模板参数；N33：`take_` 改名、超时统一用 `ms_to_ticks()`、校验方式可配），放进 `device/` 的子目录并补 HAL 保护。
10. **dm_motor（达妙）新写**：CAN 电机；MIT 模式 / 位置速度模式 / 速度模式；使能、失能、清错误、设零点命令；反馈解包（ID、错误码、位置、速度、扭矩、温度）；`P_MAX` / `V_MAX` / `T_MAX` 映射可配置；Online、`snapshot()`；接口风格和 DjiMotor 对齐。
11. fork 里已有、只需保留的：UART 七项（Q8）、CAN 三项（Q9）、Online 毫秒语义与 `is_online`（Q7）、DWT（Q11）、`can_tx_task` 按需创建（C10）、QuaternionPID 拆文件、PID 的 `get()`（C23）。Matrix 的 `_data` 改回 private + `friend`（C23 的舍弃项）。
12. 每加一项都补 HAL 模块保护（N24），并对照附录 B 核对没有带进 N1~N37。

### 阶段 3：功能全面测试

1. **主机侧单元测试**（`algorithm/` 不依赖 `board_instances.hpp`，A6 拆开后可以在 CI 上用主机编译器跑）：PID（F4、NaN）、四元数（F1、零模长）、Matrix、RLS。
2. **编译矩阵**：功能开关逐个关、HAL 模块（FDCAN / UART / SPI / TIM）逐个关，每种组合都 0 warning（防止 N24 再出现）。`CAN_RX_ENABLE = 0` 的组合要实际跑一次 `receive()`（N28）。
3. **BSP 实板回归**：`docs/bsp_test/` 的 UART（U7 ⇄ U10）/ RS485（U2 ⇄ U3）/ CAN（CAN1 ⇄ CAN2）/ SPI + BMI088 全部用例，加上 UART IT 模式，Debug 与 -O2 各跑一遍，再加浸泡测试；DMA 循环模式专项（TIM8 = 15 不卡死）；F2 的短帧压力测试（1~4 字节、4 Mbps、多个高优先级任务干扰）。
4. **Service**：EventState 树（fork 的 81 条 + 默认树的传播 + 运行期错误不清 RUNNING）、Online 毫秒语义、N16 回归（运行期调 `regist` 后 CAN 接收照常分发）、N13 回归（漏写实例时 `init()` 返回 `BAD_ARG`）。
5. **新功能实板**：CAN TX-empty 动态开关（丢唤醒统计；先修 N27，再人为关一次中断，确认 `tx_stall_recover` 会增加、总线能恢复）、ErrorStatusCallback 计数、DWT（两个任务同时调 `get_time_s()`，跑满 10 圈回绕不跳变）、编码器、阶段 2 并入的设备（R9DS / 舵机 / EmmV5 / dm_motor / DeviceMotor）、N30 的转调钩子（库外 UART 用 HAL_IT 收发）。手上没有硬件的设备标“只编译验证”。
6. **全程不打开 BMI088 加热电阻**（PB1 / TIM3_CH4），测试固件保留只读监视。
7. 结果按 `docs/bsp_test/` 的格式存档（用例 / 目的 / 方案 / 现象 / 结论 + 原始数据）。

### 阶段 4：文档补全

1. 按 Q16 采用 fork 的结构（接入指南 / 常用接口 / 编辑器配置 / `docs/code/` 分层文档 + 架构图），用 ICBSP 的名字和新目录重写；`docs/code/` 跟着代码的子目录组织，架构图用 `tools/draw_diagrams.py` 重画。
2. 修掉评审 D2 和本文 N26 列出的过时、错误、悬空引用。
3. 接入指南补齐前提：C++20；宿主要提供的两个文件及各自职责；宿主不能定义的 HAL 回调清单（含 `HAL_FDCAN_ErrorStatusCallback`），以及库外同类外设怎么通过 `icbsp_unhandled_*()` 拿到回调（N30）；编码器的 ARR 要求（N37）；FDCAN `MessageRAMOffset` / `StdFiltersNbr`；时基优先级的两条真实理由（`HAL_DMA_Abort` 竞争、启动前延时）；newlib 锁；BDMA 外设限制。
4. 功能开关逐个写明“关掉之后会怎样”。
5. 编码规范更新：`get_` / `is_` 规则、包含路径规则、C++20 规则（volatile、聚合体 Config、平凡析构）、`.dma_buffer` 宏。
6. 每个模块如实标注“已实测 / 只编译验证”；`docs/bsp_test/` 注明对应的代码版本。
7. 许可证说明：matrix 的授权来源、VQF 的 SPDX、厂商 PDF 是否继续随仓库分发（A10）。

### 阶段 5：web_ui 接入帮助（Node.js 前端 + `integrate.py` 引擎，Q14 = A）

目标（按你的说明）：在图形界面里完成 CubeMX 配置校验；按 `.ioc` 里开了的外设，自动在宿主工程里生成对应的 BSP 对象（开了 can1、can2 就生成 `bsp_can1`、`bsp_can2`）；按已有的外设提供可选的设备初始化（有 can1 时可以勾选 dji_motor、dm_motor 等）。

1. **`integrate.py` 加机器接口**（合入 fork 的扩展后）：`--json` 输出检查结果、外设列表和可挂的设备；`--plan` / `--dry-run` 输出将要写入的 diff；`--apply <选择.json>` 按 UI 的选择写盘。
2. **库侧清单** `tools/manifest.json`（由 Python 读取，版本号随库发布）：外设类型 → BSP 类、模板参数、必需的 CubeMX 设置（DMA、中断、优先级、`.dma_buffer`）；设备目录 → 依赖的外设、构造参数（类型、范围、默认值、说明）、`init()` 顺序；校验规则 → 条件、错误等级、修改方法。
3. **`.ioc` 校验**：现有检查 + D1 缺的检查（`TICK_INT_PRIORITY < 5`、FDCAN Message RAM 不重叠、`StdFiltersNbr`、SPI `MasterKeepIOState`、DMA Normal / Byte、D-Cache、NVIC 是否打开、defaultTask 是否 As weak），UI 逐条显示通过 / 失败。
4. **生成**：`board_instances.hpp`（X-macro，Q1）、`board_cfg.hpp` 里每个外设一个实例（名字按外设编号；参数取自 `.ioc` 和时钟树；自动套 `.dma_buffer` 宏）、`init()` 行，以及 `hardware_config.h`（功能开关按勾选生成，Q4）。
5. **可选设备**：按已有外设列出可挂的设备，勾选并填参数后生成实例、`init()` 和必要的回调转调（例如 `HAL_GPIO_EXTI_Callback`）；设备只在它依赖的外设和功能开关都满足时出现（例如 DeviceMotor 需要 TIM 的 PWM + 编码器）；DR16 / ET08 等还没实现的设备在清单里标“计划中”。
6. **与宿主同步**：只写生成区 / USER CODE 段，可以重复运行；CubeMX 重新生成后能检测出差异；写盘前给出 diff 预览；遇到用户手改过的内容就停下，提示手动合并。
7. **Node 前端只负责展示和收集选择**，不复制接入逻辑；找不到 Python 或版本不够时给出明确提示。
8. **测试**：拿 MC02_with_submodule 的 `.ioc` 和一个全新 CubeMX 工程做生成快照测试，生成的代码必须 0 warning 编译。

### 阶段 6：宿主接入回归测试

1. 全新 CubeMX 工程：H723（DM-MC02），另外至少一个不同型号（H743 或 H750），分别用 `integrate.py` 命令行和 Web UI 接入，编译 0 warning（C++20），上板冒烟（运行位已置、库任务在跑、串口 / CAN 收发正常）。
   - **通过回归的 H723 测试宿主工程就是新的示例工程**（Q18）：整理（去掉临时测试代码或单独开关）后推到组织下，README 写上链接。
2. CubeMX 重新生成后再跑一遍接入，确认幂等（只有预期的行尾变化）。
3. 本工作区把 1.0 的分支合回 main，submodule 指向 ICBSP；更新本工作区 `CLAUDE.md`（库路径和目录、HAL 回调清单加上 ErrorStatusCallback、C++20、去掉 `examples/dm_mc02` 的引用、烧录参照改为 `templates/Flash`）；跑完阶段 3 的回归用例；clangd `--check` 0 诊断。
4. 记录每种型号的接入耗时和遇到的问题，回填到接入指南的“常见问题”。

### 阶段 7：发布 v1.0.0

1. 许可证审计：根目录 `LICENSE`（版权人 INVC & CUBOT）；matrix 的书面授权说明；VQF 的 SPDX；厂商 PDF 的去留（A10）。
2. 版本信息：`ICBSP_VERSION_MAJOR/MINOR/PATCH` 宏，清单和 Web UI 校验版本是否匹配。
3. `CHANGELOG.md`：从 `43a34cf` 起的变化，包括 `STM32_BSP` 那边 10-17 实测修复的来源提交（那个仓库之后不再维护）、fork 的提交，注明作者。
4. 打 tag `v1.0.0`，写 release notes（发布方 INVC & CUBOT；支持的型号、已实测的模块、已知限制）。
5. 本工作区更新 submodule 指针，按 `[feat]` 格式提交；README 写上阶段 6 产出的示例仓库链接；`STM32_BSP` 在 GitHub 上**归档**（只读，保留历史，Q19）。

### 待排期（不进 v1.0.0）

- DR16 接收机（DT7 遥控器，DBUS）设备驱动。
- ET08 接收机设备驱动。

---

## 附录 A：已有问题在 fork 中的状态

| 编号 | fork 中的状态 |
| --- | --- |
| F1 | 未解决 |
| F2 | 不再出现，但换成了 N1 / N12 |
| F3 / F4 / F5 / F6 / F8 / F10 | 未解决 |
| F7 | 部分解决（RX 硬件错误计数） |
| F9 | 解决（重新 init、RF0L）；StdFiltersNbr 写入仍在 |
| F11 | **未解决**（2026-11-04 更正）：字段改成了 double，但赋值时 `static_cast<float>`（`dji_motor.cpp:311`）；Luenberger 观测值仍然从未计算 |
| A1 / A2 / A3 / A4 / A5 / A8 / A9 | 未解决（A2：sys 反而降到 +6） |
| A6 | 部分解决（位号有编译期检查，仍依赖 RTOS） |
| A7 | 解决（可加载段 + 启动拷贝） |
| A10 | 部分解决（删掉了 examples；PDF 还在） |
| I1 | 未解决，并扩散到 CAN（N23） |
| I2 / I4 / I5 | 未解决 |
| I3 | 解决 |
| I6 | 部分解决（`is_online`、ms 超时） |
| I7 | 部分解决（PID 去掉 virtual 和 `print`） |
| I8 | GPIO / Key 解决；PWM（以及 Key、Encoder）仍允许默认构造，但所有接口都先检查 `_initialized` / `_port`，不会再碰空指针，剩下的只是接口整洁问题 |
| D1 | 未解决 |
| D2 | 结构上改善，又引入了 N26 |
| D3 | 部分解决（C++20 下指定初始化合法），仍然没有警告选项 |

## 附录 B：新问题汇总（N1~N37）

N1 UART 发送流缓冲区多写者 · N2 UART FIFO 不再打开 · N3 RX 流缓冲区不是 2 倍 · N4 IDLE 长度不再重算 · N5 卡死判定按时间 · N6 先读计数再中止 · N7 没有 DMA 句柄修复 / NVIC 保护 · N8 运行中重新 init 失效 · N9 `pdMS_TO_TICKS(portMAX_DELAY)` 溢出 · N10 不检查 DMA 和可达性 · N11 删掉 RS485 软件 DE · N12 在途长度竞争 · N13 X-macro 漏写静默失效 · N14 库依赖宿主的头文件和宏 · N15 摆设配置宏 · N16 运行期 regist 清掉 RUNNING 位 → CAN 接收全停 · N17 远程帧又被接收 · N18 Queue 模式发不出 · N19 DLC < 8 带出旧数据 · N20 CAN 发送多写者 · N21 `receive()` 和 `can_rx_task` 抢原始缓冲 · N22 恢复间隔写死 / Bus-Off 重复计数 · N23 CAN / DjiMotor 只实例化了 `<16,16>` · N24 TIM / UART 相关模块（含新增的）没有 HAL 模块保护 · N25 强定义 `StartDefaultTask` 可能重复定义 · N26 文档悬空引用与错误说法 · N27 CAN `tx_recover()` 无人调用 → 丢唤醒后总线沉默 · N28 `CAN_RX_ENABLE = 0` 时 `receive()` 卡死 · N29 CAN 发送缓冲两个读者（任务 + ISR） · N30 库独占 HAL 回调，库外同类外设的回调被吞 · N31 UART `tx_recover()` 不持锁 · N32 `BspSpi` 不符合 constexpr / 平凡析构约定 · N33 Emm_V5 的 `get_` 有副作用、超时换算两次、只支持 0x6B 校验 · N34 R9DS 诊断计数用 float · N35 CAN 空节点仍发 0 帧、恢复时 reset 可能失败 · N36 EventState 没有清错误接口 / `Online::update()` 临界区过长 · N37 编码器 ARR 假设、舵机跨任务变量非 atomic、PID `df_dt` 语义
