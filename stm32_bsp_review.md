# stm32_bsp 代码审查：潜藏问题清单

- 审查对象：`third_party/stm32_bsp`，submodule 提交 `43a34cf`（`[feat] Matrix by RoboWalker & RLS`）
- 范围：`bsp/`、`service/`、`device/`、`algorithm/`、`templates/`、`tools/integrate.py`、`CMakeLists.txt`、`README.md` / `docs/编码规范.md`。`examples/` 不在范围内；`vqf.*`（第三方移植）只看了接入方式，没有审算法本身。
- 方法：只读代码（对照宿主工程的 `.ioc`、`FreeRTOSConfig.h`、`freertos.c`），**没有编译、没有上板**。
- 可信度：
  - **确认**：从代码就能直接推出结论；
  - **待验证**：推理成立，但触发条件或实际后果要上板或写测试才能确定。
- 严重度：**高** = 容易触发，后果是失控、卡死或数据静默出错；**中** = 有明确触发条件，或者是移植 / 使用时一定会碰到的坑；**低** = 代码质量问题、诊断盲区、文档不一致。
- **2026-11-04 补充**：主线已经改成 fork（`stm32-framework` @ `7ef6bfa`，见 `stm32_framework_对比评估.md` 3.1 节）。第 6 节是在 fork 代码上的独立复核：列出本文各条在 fork 里的位置和现状，供回填时对照。fork 自己引入的新问题不记在本文，统一编为 N1~N37，见对比评估的 2.2 节与 2.8 节。

---

## 0. 摘要

| 编号 | 级别 | 模块 | 问题 | 可信度 |
| --- | --- | --- | --- | --- |
| F1 | 高 | quaternion / BMI088 / PID | 初始 `ImuData::quaternion` 全 0，喂给 `error_to()` / `QuaternionPID` 会算出 NaN，而 NaN 会永久留在 PID 积分里 | 确认 |
| A1 | 中 | system / 模板 | 初始化动作写在 `configASSERT(...)` 里面，断言被编译掉时整个系统都不会初始化 | 确认 |
| A2 | 中 | system | 库任务优先级（+6~+8）低于 CMSIS-RTOS2 常用优先级（`osPriorityNormal` = 24），注释说的“最高”不成立 | 确认 |
| A3 | 中 | BMI088 / HAL 时基 | 调度器启动前 `HAL_Delay()` 要求 HAL 时基优先级 < 5，这条依赖没写出来，而且和 `undo.md` 的回归计划冲突 | 确认 |
| F2 | 中 | bsp_uart | 任务侧启动 DMA 后才置 `_tx_chunk_active`，中间若发送完成中断先到，同一段数据会**再发一遍** | 待验证 |
| F3 | 中 | can_tx_task | 共帧槽位里只要有一个电机不刷新，整帧就从“凑齐即发”降到 5 ms 心跳（1 kHz → 200 Hz） | 确认 |
| F4 | 中 | PID | 链式配置时 `i_max = 0` 不跟随 `out_max`，积分没有上限；两种配置方式语义不同 | 确认 |
| F5 | 中 | DjiMotor | `data()` 返回实时引用，互斥锁是私有的，读者没法加锁，必然撕裂读；互斥锁形同虚设 | 确认 |
| A4 | 中 | bsp_uart / bsp_spi | `dma_reachable()` 把 AXI SRAM 当作所有 DMA 都可达，SPI6 / LPUART1（走 BDMA）会误判通过 | 确认 |
| A5 | 中 | fault_record | 不记录 HardFault / MemManage / BusFault，最常见的崩溃类型没有现场 | 确认 |
| I1 | 中 | bsp_uart | 模板只显式实例化了 128 / 256，用其他尺寸会链接失败，只能改库源码 | 确认 |
| I2 | 中 | CAN 节点 | 发送只支持标准帧；接收节点只分发 DLC = 8 的帧（扩展帧电机、短帧设备接不进节点框架） | 确认 |
| D1 | 中 | README / integrate.py | CubeMX 前提缺 FDCAN `MessageRAMOffset` 等项；`integrate.py` 不检查 HAL 时基优先级 | 确认 |
| 其余 | 低 | — | 见第 1~4 节（共 20 余条） | — |

---

## 1. 架构层面

### A1【中｜确认】初始化写在 `configASSERT` 参数里，断言一关整个系统就没了

- 位置：`service/system.cpp:19-27`（`configASSERT(SysFlagInit() == ...)`、`configASSERT(xTaskCreate(...) == pdPASS)`），`templates/board_cfg.cpp`、`templates/user/user_main.cpp`，以及编码规范第 8 节要求的“每条 `init()` 单独一行 `configASSERT(x.init() == Status::OK)`”。
- 问题：`configASSERT` 是可配置的宏。FreeRTOS 在没有定义它时默认为空（`#define configASSERT(x)`），Release 构建里关掉断言也很常见。一旦为空，`SysFlagInit()`、库任务创建、所有外设 `init()`、业务任务创建都**不会执行**。编译没有任何警告，系统静默什么都不做。
- 建议：先求值、再断言，例如 `const Status st = x.init(); configASSERT(st == Status::OK);`。也可以由库提供一个不会被编译掉的 `STM32BSP_CHECK(expr)` 宏（内部调用 `fault_on_assert`），把编码规范和模板一起改掉。另外，`configASSERT` 的定义 `if ((x) == 0) {...}` 没有包 `do { } while (0)`，接在 `if/else` 后面会有悬挂 else 问题；FreeRTOS 默认定义也是这个形式，但库模板可以顺手修掉。

### A2【中｜确认】库任务优先级与 CMSIS-RTOS2 优先级体系不匹配

- 位置：`service/hardware_config_defaults.hpp:39-51`（`SYS_TASK_PRIORITY = tskIDLE_PRIORITY + 7`、`CAN_TASK_PRIORITY = +8`，注释写“最高，1 kHz 节拍内必须跑完”）、`:157-159`（`BMI088_TASK_PRIORITY = +6`）。
- 问题：库要求 CMSIS_V2，CubeMX 下 `configMAX_PRIORITIES = 56`，`osPriorityNormal = 24`、`osPriorityLow = 8`。宿主工程 CubeMX 生成的 `defaultTask` 就是 `osPriorityNormal`（`Core/Src/freertos.c:56`）。用户只要按 CubeMX 习惯 `osThreadNew(..., osPriorityNormal)` 建业务任务，它的优先级就比 CAN 收发、`sys_task`、IMU 任务都高。这样一来：
  - 业务任务忙一会儿，CAN 发送的 1 ms 节拍、Bus-Off 恢复、串口巡检都会被推迟；
  - `Online::update()` 由 `sys_task` 推进，离线计时会跟着偏慢；
  - BMI088 刷新漏帧（`attitude_skip_count` 增加）。
- 建议：默认值改成相对 `configMAX_PRIORITIES` 取（例如 `configMAX_PRIORITIES - 2`），或者直接用 `osPriorityRealtime` 等 CMSIS 常量；在 `system.cpp` 的 `static_assert` 里把 BMI088 优先级也检查一遍；README 1.3 写明“业务任务必须低于这些值”。

### A3【中｜确认】BMI088 在调度器启动前 `HAL_Delay()`，隐含要求 HAL 时基优先级 < `configMAX_SYSCALL`（5）

- 位置：`device/device_bmi088.cpp:502-507`（`delay_ms()`：调度器没启动就调 `HAL_Delay()`），`init()` 里一共要等约 100 ms。
- 问题：`system_init()` 先调了 `xTaskCreate()`。FreeRTOS 在调度器启动前进出临界区时 `uxCriticalNesting` 从 `0xaaaaaaaa` 起算，退出临界区不会恢复 BASEPRI，于是数值 ≥ 5 的中断一直被屏蔽到 `vTaskStartScheduler()`（`bsp_spi.hpp:45-46` 的注释也提到了这一点）。如果 HAL 时基（TIM8）优先级 ≥ 5，比如 CubeMX 默认的 15，那么 `board_init()` 里的 `imu_bmi088.init()` 会**永远卡在 `HAL_Delay()`**。
  - 现在宿主工程的 TIM8 优先级是 4，所以碰巧没事。README 1.1 只拿 `HAL_DMA_Abort()` 竞争来解释为什么要设 4，没写这一条。
  - `third_party/stm32_bsp/undo.md` 的计划是“改完后把 TIM8 优先级改回 15 验证不再卡死”。照做的话，开机就会卡在 BMI088 初始化，并且容易被误判成 DMA 循环模式没改好。
  - `bsp_uart.cpp:137-140` 的 `stop_dma_stream()` 用 `HAL_GetTick()` 计时，在启动前重复调用 `init()` 的路径上也有同样的依赖。
- 建议：调度器启动前改用不依赖中断的忙等（DWT 周期计数器），或者把 BMI088 的芯片配置推迟到内部任务里第一次运行时做；README 1.1 补上这条理由；`undo.md` 记下“改回 15 之前，先解决启动前的延时”。

### A4【中｜确认】`dma_reachable()` 不区分 DMA 控制器，BDMA 外设会误判通过

- 位置：`bsp/bsp_uart.cpp:45-51`、`bsp/bsp_spi.cpp:33-39`（同一份代码复制了两次）；`hardware_config_defaults.hpp:113-116, 136-139` 的注释说登记表考虑了“LPUART1”和“SPI1~6”。
- 问题：H7 的 SPI6、LPUART1（以及 I2C4、SAI4 等）挂在 D3 域，由 **BDMA** 服务，BDMA 只能访问 SRAM4（0x3800_0000）。`.dma_buffer` 放在 RAM_D1（0x2400_0000），检查却只排除了 ITCM / DTCM，所以会返回“可达”，`init()` 返回 OK，运行时 DMA 出传输错误，或者静默收不到数据，正好是这条检查本来要防的情况。另外 `uart_irqn()` 也不认识 LPUART1。
- 建议：按 `hdma->Instance` 判断是 DMA1/2 还是 BDMA，BDMA 时要求地址落在 SRAM4；README 写明 BDMA 外设需要单独的段。把两份 `dma_reachable` 合并到一个公共头文件里。

### A5【中｜确认】`fault_record` 不覆盖 HardFault 等异常

- 位置：`service/fault_record.hpp:10-14`（只有 assert / 栈溢出 / malloc 失败三个入口）。
- 问题：空指针访问（例如默认构造的 `BspGpio` / `BspKey` 没有 `init()` 就调用，见 I8）、非法地址、未对齐访问，都会进 HardFault / MemManage / BusFault。宿主工程这些 Handler 是 CubeMX 默认的 `while(1)`，没有现场记录；复位后 `fault_record` 里还是上一次的断言记录，容易误导排查。
- 建议：库提供 `fault_on_exception()`（汇编入口取 MSP/PSP 栈帧，记录 PC / LR / xPSR / CFSR / HFSR / MMFAR / BFAR），模板里给出 `HardFault_Handler` 的 USER CODE 片段；`FaultKind` 增加 `HARD_FAULT` 等种类。

### A6【低｜确认】`status.hpp` 把纯状态码和 RTOS 事件组绑在一起

- 位置：`service/status.hpp:18-24`（`#include "cmsis_os2.h"`、`extern osEventFlagsId_t sysEvent`）。
- 问题：`algorithm/rls.hpp` 号称“纯计算，不依赖硬件”，但为了拿到 `Status` 就间接依赖了 CMSIS-RTOS2 头文件，主机侧单元测试要先打桩 `cmsis_os2.h`。`Status` 也被系统事件位复用为位号（`1U << Status`），以后加枚举值时容易和 `SYS_FLAG_*_BIT` 冲突，而且没有 `static_assert` 保护。
- 建议：拆成 `status.hpp`（只放 `enum class Status`，不包含其他头文件）和 `sys_flag.hpp`（事件组相关）。

### A7【低｜确认】`.dma_buffer` 是 NOLOAD，放进去的对象得不到 C++ 的零初始化保证

- 位置：`templates/linker_sections.ld`（`.dma_buffer (NOLOAD)`）。
- 问题：标准规定静态存储期对象先零初始化、再动态初始化，NOLOAD 段不会被启动代码清零。现在 `BspUart` / `BspSpi` 的每个成员都有默认初始化或者在构造函数里赋值，所以没出问题。但以后只要给类加一个没写初始化的成员，或者用户往 `.dma_buffer` 里放“以为是 0”的普通数组，就会读到随机值。另外，编译器把动态初始化折叠成静态初始化时（标准允许；clang / armclang 的 GlobalOpt 会这样做），初值会被 NOLOAD 整个丢掉。
- 建议：把 `.dma_buffer` 改成启动时清零的普通段（在启动文件里再清一段，或者复用 `.bss` 那套 `_s/_e` 符号）；或者在 README 里明确写出“NOLOAD、不清零”，并在类的注释里要求所有成员必须显式初始化。

### A8【低｜待验证】`fault_record` 冷启动时读 ECC SRAM 里没初始化过的内容

- 位置：`service/fault_record.cpp:75-77`（先读 `magic` / `count` 再写）。
- 问题：H7 的 AXI SRAM 带 ECC，上电后数据和校验位都是随机的，读到双比特错误时有可能产生总线错误；不足 64 位的写入还要先读后改。冷启动后的第一次断言就落在这条路径上。现在“复位后保留”已经实测过，但“上电后第一次故障”有没有测过不清楚。
- 建议：上电（`RCC_RSR` 判断是 POR / BOR）时整字清零 `fault_record`（这个阶段没有有效内容可保留），之后再按 magic 判断。

### A9【低｜待验证】`BspUart::printf` 浮点格式化的线程安全依赖宿主的 newlib 配置

- 位置：`bsp/bsp_uart.cpp:554`（`vsnprintf`）。`_printf_lock` 只按实例加锁。
- 问题：newlib(-nano) 的 `%f` 走 `_dtoa_r` → `Balloc` → `malloc`。宿主工程 `configUSE_NEWLIB_REENTRANT = 1`，但 `Core/Src/` 里没有 newlib 锁胶水（`newlib_lock_glue.c` 或 `__malloc_lock` 实现）。两个任务同时对**不同串口**用浮点 `printf` 时，可能并发进入 `malloc`。库里的 `new (std::nothrow)`（`CanRxNode` / `CanTxNode` / VQF）也走 newlib 堆，只不过都在初始化阶段，风险小。
- 建议：README 1.1 写明要求（CubeMX 里打开 FreeRTOS 的 newlib 线程安全锁策略），或者库里提供 `__malloc_lock` / `__malloc_unlock`；另外说明库用到两个堆（FreeRTOS heap 与 newlib heap），`xPortGetFreeHeapSize()` 统计不到后者。

### A10【低｜确认】合规与体积

- `algorithm/matrix.*` 不适用 MIT，授权依据只有一句口头许可（文件头已写明）。它被 `GLOB_RECURSE` 无条件编进 OBJECT 库，所有宿主都会带上这部分代码。建议做成可选编译（CMake 选项），并保存一份书面授权。
- `docs/` 里有 DJI / 达妙的说明书 PDF，`examples/` 带了整套 HAL / FreeRTOS（README 3.1 已说明有几十 MB）。把厂商文档随 MIT 仓库再分发可能有版权问题，体积也拖慢 submodule 克隆。可以考虑移到独立的文档仓库，或者改成只放链接。

---

## 2. 功能实现层面

### F1【高｜确认】全 0 四元数 → NaN → 永久污染姿态 PID

- 位置：
  - `device/device_bmi088.hpp:357`：`_data = ImuData()`，四元数初值是 `{0,0,0,0}`；手动模式、`attitude = false`、宿主没有转调 EXTI 时一直保持 0（`imu_definition.hpp:11` 明确说保持 0）；
  - `algorithm/quaternion.cpp:149`：`rotation_vector()` 在 `s < 1e-9` 时用 `k = 2.0 / qw`，`qw = 0` 时得到 `inf`，`0 × inf = NaN`；
  - `algorithm/pid.cpp:76-81`：`i_term += ki * NaN`，`clamp_symmetric` 遇到 NaN 时比较全为假，原样返回 NaN，积分从此一直是 NaN，直到 `reset()`。
- 触发：上电后控制任务和 IMU 任务同时在运行位被放行，控制任务第一次 `snapshot()` 很可能早于第一次姿态解算；用户没有开 drdy 中断时更是一直为 0。`Quaternion::from_array(d.quaternion)` 得到零四元数，`error_to()` / `QuaternionPID::calc()` 输出 NaN，再转成 `int16_t` 交给 `FillData()`（浮点 NaN 转整数在 C++ 里是未定义行为）。结果是云台 / 姿态环一上电就失效，而且不会自己恢复。
- 建议：
  1. `ImuData` 的四元数初值设为单位四元数 `{1,0,0,0}`（`DeviceBmi088` 构造时写入），并提供“姿态是否有效”的标志 / 计数（例如 `attitude_ready()`）；
  2. `rotation_vector()` / `normalized()` 对零模长和非有限值做防护（返回零向量）；
  3. `PID::calc()` 遇到非有限输入时丢弃本次输入、保持状态，并计数（`Rls` 已经这样做了，可以照抄）。

### F2【中｜待验证】UART 发送：同一段数据可能被发两次

- 位置：`bsp/bsp_uart.cpp:1086-1097`（`start_chunk()`：先 `HAL_UART_Transmit_DMA()`，成功后才执行 `_tx_chunk_active = true`），`:840-847`（`start_transmission_from_isr()`：`_tx_chunk_active` 为假时不清 `_tx_chunk_len`，接着 `start_chunk(true)` 又把这一段原样启动一次）。
- 触发：任务侧（`send()` / `tx_recover()`）启动 DMA 后、执行那两条赋值之前被抢占（tick 切到更高优先级任务，或者一个稍长的中断），而这一段又很短（高波特率下 1~几个字节只要几 µs），TX 完成中断先到。中断看到 `active == false`，就当成“启动失败、等待重发”，于是把同一段再发一遍。接收方会收到重复字节；RS485 下 DE 时序也会被拉长。
- 建议：调用 HAL **之前**先置 `_tx_chunk_active = true`（失败再改回 false），或者把任务侧的“启动 + 记账”放进屏蔽本串口中断的临界区。修完后用短帧（1~4 字节）、4 Mbps、多个高优先级任务干扰的条件做回归。

### F3【中｜确认】共帧槽位：一个电机不刷新，整帧降到心跳频率

- 位置：`service/can_tx_task.cpp:56-62`（`ready = bufferUnsend == bufferRegister`，否则只能等 `CAN_TX_HEARTBEAT_MS` = 5 ms 的心跳）。
- 触发：
  - 0x200 帧注册了 4 个 M3508，其中 1 个只 `init()`、从不 `FillData()`（调试时常见），或者它的控制任务卡住 / 被删除；
  - 同一帧里的电机由不同频率的任务控制（例如一个 1 kHz、一个 500 Hz）。
  这时整帧发送频率由最慢的那个决定，最差是 200 Hz，其余电机的闭环带宽被悄悄降下来，库没有任何诊断。
- 建议：“凑齐即发”加一个时限，例如距上次发送 ≥ 1 ms 且有任意槽位更新就发（已经过期的槽位不参与“凑齐”判断）；或者改成固定 1 kHz 周期发送；并增加 `heartbeat_send_count` 之类的计数，便于发现降频。README 1.4 写明这条行为。

### F4【中｜确认】PID 链式配置时积分没有上限

- 位置：`algorithm/pid.cpp:4-10`（“`i_max == 0` 跟随 `out_max`”只在 `PID(const Config&)` 里处理），`pid.hpp:179-201`（`limit_output()` / `limit_i()` 只写字段），`pid.cpp:81`（`clamp_symmetric(i, 0)` 表示不限幅）。
- 问题：头文件示例 `PID motor1; motor1.kp(0.8).ki(0)...limit_output(10000)` 这种写法，`i_max` 保持 0，积分不受限，饱和后会严重积分饱和（windup）。先用 Config 构造、再用链式改 `out_max` 时，`i_max` 还停在旧值。文档“`i_max` 写 0 表示跟随 `out_max`”只对其中一种写法成立。
- 建议：在 `apply_limits_and_output()` 里取实际生效的上限：`i_limit = (_config.i_max == 0) ? _config.out_max : _config.i_max`，不再改 `_config`。另外：
  - 积分分离是“清零”而不是“冻结”（`pid.cpp:75`），误差越过阈值时输出会跳变，建议提供“冻结”选项；
  - 输出饱和时没有条件积分（clamping anti-windup），只靠 `i_max` 兜底；
  - `ki` / `kd` 按“每周期”计（没有 dt），换控制频率就等于换参数，建议在头文件里写清楚。

### F5【中｜确认】`DjiMotor::data()` 撕裂读，互斥锁不起作用

- 位置：`device/dji_motor.hpp:125-132`（返回内部引用，注释要求“调用方须保证读取期间不并发执行 DataUnpack()”），`dji_motor.cpp:504`（`DataUnpack` 对私有互斥锁 `try-lock`）。
- 问题：`DataUnpack` 由库的 `can_rx_task`（优先级 +8）调用，用户的控制任务没办法知道它什么时候运行；锁是私有的，读者拿不到，所以 `try-lock` 永远成功，只是白白消耗开销。控制任务读 `angle_multi_round` 和 `velocity` 时，可能分别来自相邻两帧。`DeviceBmi088` 已经提供了 `snapshot()`，两个设备的接口不一致。
- 建议：仿照 BMI088 提供 `snapshot(MotorData&, DjiMotorRawData* = nullptr)`（短临界区拷贝），去掉互斥锁或者让它真正保护读路径；`data()` 保留但标注“仅调试用”。

### F6【低～中｜确认】CAN 回退缓冲没人消费时，一直丢帧并置 FULL

- 位置：`service/can_rx_task.cpp:53-58`。
- 问题：没有被节点认领的帧都要进回退缓冲，而大多数用户根本不调 `BspCan::receive()`。总线上只要有未注册的 ID（别的设备、调试工具、同一总线上没建对象的电机），16 帧之后就每帧都 `++rx_return_dropped` 并 `SysFlagSet(Status::FULL)`。这个计数和全局 FULL 标志因此失去诊断价值，`SysFlagSet` 还会清掉 OK 位（`status.cpp:29-31`，而且清掉后再也不会重新置位）。回调返回非 OK（例如编码器越界）的帧也会进这里。
- 建议：回退通道做成可选（`init()` 参数或 `hardware_config.h` 开关，默认关），或者在没有消费者时直接丢弃、只计数；满了丢最旧的帧。`SysFlagSet` 的语义（谁读、什么时候清）要补文档，或者删掉。

### F7【低｜确认】UART 接收侧的诊断与并发

- `bsp/bsp_uart.cpp:856-868`：`on_error_isr()` 只统计 TX 侧。接收的帧错误 / 噪声错误（FE / NE / PE）在 DMA 模式下 HAL 不会中止接收，坏字节照常投进流缓冲区，`RxDiag` 里看不到。建议在回调里按 `huart->ErrorCode` 分别计数。
- `receive()` 没有读锁。FreeRTOS 流缓冲区要求单读者，两个任务同时 `receive()` 同一个串口是未定义行为；头文件只写了“多写安全”，应补“单读者”的说明（`BspCan::receive()` 已经写了）。
- `rx_recover()` / 重复 `init()` 只关了 UART 的 NVIC（`:739-754`、`:334-356`），RX DMA 流中断仍然开着（HAL 在 DMA TC 里也会回调 `RxEventCallback`），保护不完整（待验证）。
- `tx_recover()` 在 `_baudrate == 0` 时直接返回（`:629`），卡死恢复被整个关掉，文档没写。
- `printf()` 取锁超时返回 `IO_ERROR`（`:547-550`），应返回 `TIMEOUT` / `BUSY`；`printf("")` 返回 `BAD_ARG`。

### F8【低｜确认】BMI088 的几个边角

- 运行中再 `init()`（`device_bmi088.cpp:77, 213-216`）：只靠 `_init = false` 让内部任务跳过。如果任务已经越过检查、正在 `solve_attitude()`，`_vqf->resetState()` 会和它并发写 VQF 状态（没有锁）；软复位期间读到的那一帧也可能写进 `_data`。建议重新初始化时用任务通知 / 标志让内部任务先停下并确认，再动芯片和 VQF。
- 自动模式下仍然配置了加速度计 INT1 数据就绪（`:169-180`），但 `on_acc_drdy_isr()` 只计数（`:467-473`）。默认 1600 Hz 的 EXTI 中断白白消耗。建议自动模式不映射 INT1，或者文档说明可以不转调。
- 加速度、角速度、温度分三次片选读取，不是同一采样时刻（两颗芯片本来也不同步）。“`snapshot()` 拿到同一帧”的“同一帧”指同一次刷新，建议在文档里说明这个口径。

### F9【低｜确认】CAN 驱动的边角

- `BspCan::init()` 不能重复调用（`bsp_can.cpp:156-201`）：第二次调用会重新创建三个 MessageBuffer，旧的泄漏，ISR 可能正在用旧指针。中途失败时实例已经登记、缓冲区没释放（初始化失败会断言停机，影响小）。UART 已经修了同类问题，CAN 没有跟上。
- `config_filter()` 配的标准帧过滤器（`bsp_can.cpp:239-250`）和全局过滤器“不匹配也收进 FIFO0”功能重复。CubeMX 默认 `StdFiltersNbr = 0` 时，HAL 的 `assert_param` 拦不住（`0 - 1` 下溢），这个过滤器元素会被写到 RX FIFO0 的起始位置，过滤器本身不生效。现在不会出错，但属于没有意义的写入。
- 硬件 RX FIFO 溢出（RF0L）没有开中断、没有计数，`rx_dropped` 只统计软件队列满的情况。
- `send()` / `trigger_tx()` 用的是任务级临界区，不能在 ISR 里调用，接口注释只写了“非阻塞”。

### F10【低｜确认】PWM

- `set_freq()` 直接改 ARR（`bsp_pwm.cpp:101`）。CubeMX 默认 `AutoReloadPreload = DISABLE`，新 ARR 小于当前 CNT 时，计数器要一直数到溢出，会产生一个最长 65536 计数的异常周期（蜂鸣器会有爆音，舵机会收到一个超长脉冲）。建议 `init()` 里打开 ARPE（`__HAL_TIM_ENABLE_ARR_PRELOAD`），或者在文档里写明要求。
- 32 位定时器（TIM2 / TIM5 / TIM23 / TIM24）ARR 取 `0xFFFFFFFF` 时，`_period + 1U` 溢出成 0（`:128`、`:157`、`:166`），`freq_hz()` 会除以 0（Cortex-M 默认除零结果为 0，不触发异常，但返回值是错的）。
- `set_freq()` 限幅后的实际频率和请求值不同，但函数返回 `void`，调用方无从得知。

### F11【低｜确认】DjiMotor 数值细节

- `angle_multi_round` 是 float（`motor_definition.hpp:33`）。累计 1e4 rad 后分辨率约 1e-3 rad，长时间单向转动（摩擦轮、底盘轮）的多圈角会逐渐失去精度。`_total_ecd` 本身是 int64，建议额外提供 double 版本，或者改用“圈数 + 单圈”的表示。
- `LuenbergerMotorData`（`dji_motor.hpp:100, 151`）从来没有计算过，接口却公开了，容易被误用为“观测值”。

---

## 3. 用户接口层面

### I1【中｜确认】`BspUart<N>` 只能用 128 / 256

- 位置：`bsp/bsp_uart.cpp:15-16`（显式实例化）。
- 问题：模板成员定义都在 `.cpp` 里，用户写 `BspUart<64>` 或 `BspUart<512>` 会直接**链接报错**，只能去改 submodule 源码，这和“submodule 只读接入”的目标矛盾。`hardware_config.h` 里也没有对应的开关。
- 建议：成员定义移到 `bsp_uart_impl.hpp`，由头文件包含（header-only）；或者在 `hardware_config.h` 里提供 `UART_EXTRA_BUFFER_SIZES` 列表，由库展开显式实例化。

### I2【中｜确认】CAN 节点框架只覆盖“标准帧发送 + 8 字节接收”

- 位置：`bsp/bsp_can.hpp:138`、`service/can_tx_node.cpp:41, 111`（发送只支持标准帧），`service/can_rx_task.cpp:39-41`（只把 DLC = 8 的帧分发给节点）。
- 问题：RM 常用的一些执行器用 29 位扩展帧（例如小米 CyberGear 一类协议）；有些设备（超级电容控制器、自研传感器）回报不足 8 字节。这些设备只能绕开节点框架，自己 `receive()` 回退缓冲（而且只允许一个消费者）。接收节点支持扩展 ID，但发送不支持，两边不对称。
- 建议：`CanTxMsg` 增加 `id_type` / `dlc`，`send()` 增加重载；接收节点在注册时声明可以接受的 DLC（默认 8），分发时按节点的要求过滤。

### I3【低｜确认】`BspCan::Config::mode` 没有任何作用

- 位置：`bsp/bsp_can.hpp:103-111`、`bsp_can.cpp:115`（`_work_mode` 只赋值，从来不读）。
- 问题：用户传 `CanMode::LOOPBACK` 时不会报错，也不生效，实际模式由 CubeMX 决定。
- 建议：删掉这个参数；或者在 `init()` 里和 `hfdcan->Init.Mode` 比对，不一致就返回 `BAD_ARG`。

### I4【低｜确认】DjiMotor 的离线超时不能配置

- 位置：`device/dji_motor.hpp:99`（`Online _online;` 固定 30 ms）。README 1.3 说“在线超时按对象在构造时给出”，但 DjiMotor 的构造函数没有这个参数。
- 建议：构造函数增加 `timeout_ms` 参数，或者改为 `Config` 结构体（顺带符合编码规范第 4.5 条“禁止裸参数构造函数”，DjiMotor 现在就是裸参数构造）。

### I5【低｜确认】`ImuData` 的轴命名有歧义

- 位置：`service/imu_definition.hpp:28-32`。
- 问题：`gyro.yaw / pitch / roll` 实际是芯片本体 Z / Y / X 轴的角速度，不是欧拉角速率；安装方向一旋转，名字就和含义对不上（BMI088 头文件注释已经要专门解释）。`euler_angle` 和 `euler_radian` 同时存两份；所有字段都是 double，跟 float 接口（PID、Matrix）配合时要到处转换。
- 建议：改成 `gyro.x / y / z`（rad/s，本体系）；欧拉角只保留弧度一份，或者由调用方从四元数现算。

### I6【低｜确认】命名与风格和自家编码规范不一致

- 方法名：`DataUnpack` / `FillData` / `lvboData` / `statu` / `isOnline`（规范要求小写蛇形）；成员名：`txBuffer` / `bufferMutex` / `data_mutex_headler`（还拼错了）。
- 公有成员却用 `_` 前缀：`PID::_input` / `_output`，`BspCan::_tx_message_buffer`。
- `BspCan` 的 `trigger_tx()` / `trigger_tx_from_isr()` / `process_fifo0_isr()` 是内部入口，却放在 public。
- 全局命名空间里有通用名字：`Vector3`、`Quaternion`、`PI_D`、`angle_wrap_pi`、无作用域枚举 `MotorType { Motor3508 ... }`，以及两处 `#define PI 3.1415926f`（`motor_definition.hpp:14`、`imu_definition.hpp:17`，精度低，而且和 `arm_math.h` 的 `PI` 冲突时只是碰巧靠 `#ifndef` 规避）。按编码规范 2.1，应当加前缀或者放进命名空间。
- 时间单位不统一：`BspCan::receive(timeout)` 用 tick，其他接口都用 ms。

### I7【低｜确认】返回值约定不统一

- `status.hpp:10` 说“所有对外 API 一律返回 Status”，但 `BspPwm::set_*`、`DeviceBuzzer::*`、`PID::calc` 返回 `void` / `float`，`BspSpi::unlock()` 返回 `void`。
- 错误码有误用：`CanTxNode` 分配失败报 `BUSY`（`can_tx_node.cpp:144`，应为 `FULL`）；槽位冲突报 `FULL`（应为 `BUSY` / `BAD_ARG`）；`DjiMotor::init()` 的接收节点注册失败报 `IO_ERROR`。
- `PID` 有虚函数 `calc_input()` 却没有虚析构函数；`PID::print()` 是“暂未实现”的公有接口。

### I8【低｜确认】没有 `init()` 就使用 = 空指针

- `BspGpio()`、`BspKey()`、`BspPwm()` 允许默认构造，`set()` / `poll()` 等成员不检查 `_port` / `_initialized`，直接走 `HAL_GPIO_*(nullptr, ...)`，进 HardFault（而且 A5 说明这类故障没有现场记录）。建议删掉默认构造，或者在入口处判空并返回。

---

## 4. 文档与工程化

### D1【中｜确认】CubeMX 前提检查有缺口

- README 1.1 没写 FDCAN 的 **`MessageRAMOffset`**（多路 FDCAN 时 CubeMX 默认都是 0，几路会共用同一段 Message RAM 而互相覆盖；宿主工程已经手动设成 0 / 129 / 258）、`StdFiltersNbr`、TX FIFO 深度这些要求。
- `tools/integrate.py:185-233` 检查了 tick、钩子、CMSIS_V2、D-Cache，但**没有检查** `TICK_INT_PRIORITY < 5`（README 里最关键的一条，见 A3 和 README 3.2），也没有检查 UART / SPI 的 DMA 是不是 Normal 模式、SPI 的 `MasterKeepIOState`、FDCAN 的 Message RAM 有没有重叠。这些都能从 `.ioc` 或 `stm32h7xx_hal_conf.h`、`*_msp.c` 里静态读出来。
- 建议：给 `integrate.py` 补上这些检查，至少给出 warn；README 1.1 的表格同步补齐。

### D2【低｜确认】文档过时 / 互相矛盾

- README 第 5 行还写着“分支 `mini-bsp` … 剩阶段 E”，而阶段 E 已经完成（第 3.3 节就是阶段 E 的实测）。
- README 多处引用“见 3.6”“（3.7）”，实际章节是 3.3 / 3.4。
- README 1.3 第 6 步的配置表缺 `SPI_*`、`BMI088_*`、`UART_ENABLE_FIFO`；`templates/hardware_config.h` 也没有列 `UART_ENABLE_FIFO`。
- `hardware_config_defaults.hpp:126-129`：`UART_PRINTF_TIMEOUT_MS` 的 `@brief` 注释写在了 `UART_ENABLE_FIFO` 上面（错位）。
- `bsp_uart.cpp:54`：“本工程 8 路串口”与实际支持的列表不符。
- README 第 5 节、编码规范第 1、5、6 节还在用 `api_main.h` / `all_init()` / `api_xxx.h` 和 Protocol 层的说法；模板已经改成 `app_main.h` / `app_init()`，README 第 2 节的分层里也没有 Protocol 层。编码规范要求文件前缀 `alg_` / `device_`，实际是 `pid.hpp`、`dji_motor.hpp`。
- README 第 2 节说 `pid` “已测”，但 `QuaternionPID` 和四元数库还没有实测（头文件和提交说明里都写了）。

### D3【低｜确认】静态检查缺失

- 库 CMake 不加任何警告选项（`-Wall -Wextra -Wconversion -Wshadow` 等），“0 warning”取决于宿主的编译参数；`CXX_EXTENSIONS ON`（gnu++11），而 `status.cpp:4-6` 依赖 C++20 的指定初始化（GNU 扩展），换 `-std=c++11 -pedantic` 就会报错。
- 没有主机侧单元测试。`algorithm/` 是纯计算（拆掉 A6 的耦合后），PID、四元数、矩阵、RLS 很适合放进 CI 用主机编译器跑。上面 F1、F4 两个问题用最简单的单元测试就能抓到。

---

## 5. 建议的处理顺序

> 2026-11-04：本节是“以 stm32_bsp 为底”时的顺序，已经被 `stm32_framework_对比评估.md` 第 4 节的七阶段路线取代（主线 = fork）。下面保留原文，作为来源记录。

1. **F1**（零四元数 / NaN）：改动小，后果是上电后姿态环失效。先把 `ImuData` 四元数初值设为单位四元数，再给 `rotation_vector()` 和 PID 加有限值防护。
2. **A1**（断言里放副作用）：改模板、编码规范和 `system.cpp`，一次改完。
3. **F4**（PID 积分上限）、**F3**（共帧降频）、**F5**（电机数据快照）：都直接影响控制质量，改动集中。
4. **A3 + D1**：在 README / `undo.md` 写明 HAL 时基优先级的第二条理由，`integrate.py` 加上时基优先级和 FDCAN Message RAM 的检查，免得做 `undo.md` 那项时踩坑。
5. **F2**：先写一个短帧压力用例复现，再改赋值顺序，按 `docs/bsp_test/` 的流程回归。
6. **A2**（任务优先级）、**I1**（UART 模板尺寸）、**I2**（扩展帧 / 短帧）：接口层面的改动，影响下游用户，建议排进下一个版本一起做。
7. 其余低级别条目可以随手修，或者记进 `undo.md`。

---

## 6. 在 fork 上的复核（2026-11-04）

- 对象：`stm32-framework` @ `7ef6bfa`，路径前缀记作 `fw:`。方法同第 0 节：只读代码，没有编译、没有上板。
- 先不看本文和对比评估，从头审 fork 的 `code/`、`templates/`、`CMakeLists.txt`、`tools/integrate.py`，再和本文逐条对照。结论和 `stm32_framework_对比评估.md` 附录 A 基本一致；**F11 是唯一需要更正的**（附录 A 原来写“精度部分解决”）。
- 下表的“fork 位置”是回填和修复时要改的地方。

| 编号 | fork 里的现状 | fork 位置 |
| --- | --- | --- |
| F1 | 仍然存在：`ImuData::quaternion` 没有初值，`rotation_vector()` 的 `2.0 / qw`、PID 的 NaN 积分路径都没变 | `fw:code/service/imu_definition.hpp:58`、`code/algorithm/quaternion.cpp:149`、`code/algorithm/pid.cpp` |
| A1 | 仍然存在，而且范围更大：`system_init()` 里 DWT、`sys_state` 的 init 和两个 `xTaskCreate` 都写在 `configASSERT(...)` 里，`system_start()` 的 `complete_init()` 也一样；模板的 `board_cfg.cpp` 每条 `init()` 都是这种写法；`configASSERT` 模板仍然没有 `do { } while (0)` | `fw:code/service/system.cpp:86-117`、`templates/user/board/board_cfg.cpp`、`templates/FreeRTOSConfig_user_code.h:24` |
| A2 | 仍然存在：sys +6、CAN +8、BMI088 +6（sys 和 BMI088 同级），都低于 `osPriorityNormal`（24）；三处注释互相矛盾 | `fw:templates/user/board/hardware_config.h:66-73, 119`、`templates/user/app/app_main.cpp` |
| A3 | 仍然存在：调度器启动前用 `HAL_Delay()`；fork 已经有库持有的 `bsp_dwt`，可以直接拿来修 | `fw:code/device/device_bmi088.cpp:507` |
| A4 | 仍然存在：`dma_reachable()` 只排除 ITCM / DTCM | `fw:code/bsp/bsp_spi.cpp:34-40`（UART 侧连这条检查都没有，见对比评估 N10） |
| A5 | 仍然存在：只有 ASSERT / STACK_OVERFLOW / MALLOC_FAILED 三种 | `fw:code/service/fault_record.hpp` |
| F3 | 仍然存在：`ready = bufferUnsend == bufferRegister`，否则只能等 5 ms 心跳 | `fw:code/service/can_tx_node.hpp:247-249` |
| F4 | 仍然存在：`i_max == 0` 跟随 `out_max` 只在 `PID(const Config&)` 里处理；积分分离是“清零”；没有 dt | `fw:code/algorithm/pid.cpp:7, 76-78` |
| F5 | 仍然存在：`data()` 返回内部引用，私有互斥锁只有写入方在用。新发现：try-lock 失败时返回 `BUSY`，`dispatch()` 会把这一帧当成没人认领，放进回退缓冲（F6 的同类问题） | `fw:code/device/dji_motor.cpp:278-281`、`code/service/can_rx_node.hpp:142` |
| F6 | 仍然存在（不再置全局 FULL，只累加 `rx_return_dropped`） | `fw:code/tasks/can_rx_task.cpp` |
| F10 | 仍然存在：`set_freq()` 直接改 ARR，不打开 ARPE | `fw:code/bsp/bsp_pwm.cpp` |
| **F11** | **没有解决（更正）**：`angle_multi_round` 改成了 double，但赋值时写成 `static_cast<float>(angle)`，存进去的仍然是 float 精度；`LuenbergerMotorData` 仍然从来没有计算过 | `fw:code/service/motor_definition.hpp:32`、`code/device/dji_motor.cpp:311` |
| I1 | 仍然存在，并且扩散到了 CAN（N23）和两个串口设备：`DeviceR9dsT` 只实例化了 2 种，`DeviceEmmv5GroupT` 只实例化了 1 种 | `fw:code/bsp/bsp_uart.cpp:12-14`、`code/device/device_r9ds.cpp:10-11`、`code/device/device_emmv5_group.cpp:10` |
| I2 | 仍然存在：`CanTxMsg` 只有 `std_id`；`can_rx_task` 只把 DLC = 8 的帧交给节点 | `fw:code/bsp/bsp_can.hpp:58-62`、`code/tasks/can_rx_task.cpp` |
| I4 | 仍然存在：`Online _online` 用的是默认的 30 ms，构造函数没有超时参数 | `fw:code/device/dji_motor.hpp` |
| I8 | GPIO 没有默认构造了；Key / PWM / Encoder 还有，但所有接口都会先检查 `_port` / `_initialized`，不会再碰空指针 | `fw:code/bsp/bsp_{gpio,key,pwm,encoder}.*` |
| D1 | 仍然存在：`integrate.py` 不检查 `TICK_INT_PRIORITY`、FDCAN Message RAM、`MasterKeepIOState`、DMA 模式，也不检查 defaultTask 是否设成 As weak（N25） | `fw:tools/integrate.py:203-249` |
| D3 | 仍然存在：库的 CMake 不加任何警告选项 | `fw:CMakeLists.txt` |

本文以外、复核时新发现的 fork 问题（N27~N37）见 `stm32_framework_对比评估.md` 2.8 节。其中 N27（CAN `tx_recover()` 无人调用）、N28（`CAN_RX_ENABLE = 0` 时 `receive()` 卡死）、N30（库独占 HAL 回调）是中级别，排进了阶段 1。
