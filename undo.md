# 待办（v1.0.0 路线）

> 2026-11-03 整理，2026-11-04 按对比评估第 3 节的回答定稿。编号说明：F / A / I / D 见宿主根目录 `stm32_bsp_review.md`；C（差异条目）/ N（fork 引入的新问题）/ Q（问题）见宿主根目录 `stm32_framework_对比评估.md`（定稿方案在其 3.1 节和第 4 节）。
> **主线 = fork**（`cubot-refactor-code-plan/stm32-framework`，阶段 2 改名 **ICBSP**，发布方 INVC & CUBOT）。阶段 1~4 的库改动都在 fork 上做；`bsp:` 位置（本仓库）是回填时的对照来源。本文件在 1.0 搬进 fork 仓库。
> Q1~Q19 已全部回答（Q11 更正为编码器进核心库，与 Rh-i 协商的结果）。
> 2026-11-04 在 fork @ `7ef6bfa` 上做了独立复核：新增 N27~N37（对比评估 2.8 节），更正 F11（fork 只改了字段类型，赋值时仍截成 float）。下面的条目已经同步。
> 每个阶段结束时都要满足：Debug 与 -O2（RelWithDebInfo）编译 0 warning。

---

## 阶段 1：修复已有问题

### 1.0 切换主线

- [x] 宿主 MC02_with_submodule 新开分支，submodule 指向 fork；宿主改 C++20，按 fork 模板补 `board_instances.hpp` / `hardware_config.h` / `board_cfg.hpp`，`app/` 适配；0 warning 作为基线（此时 UART / CAN 带着 N1~N23，不上板跑通信测试）
  - 2026-11-04 完成：宿主分支 `icbsp-migration`，submodule 路径暂不改（阶段 2 改名时一起改成 `third_party/icbsp`）；Debug 与 RelWithDebInfo 干净重编 0 warning。
  - 遗留到 1.1：U10 原来是 256 字节缓冲，库只有 `<dma, 128, 128>` 的显式实例化，暂改 128（I1）；`app/test/` 测试代码还是旧接口（开关为 0 时整段不编译），1.1 / 1.2 回填后逐组适配；RS485 应答的 `dma_repair_cnt` 暂时填 0（N7）。
- [x] 本文件搬进 fork 仓库；fork 的 `docs/bsp_test/` 注明“对应 STM32_BSP @ 43a34cf 的旧实现”

### 1.1 UART 回填（保留 fork 接口，内部换成本仓库的收发状态机）

- [x] 修 N1~N12：`_tx_write_lock` + 中断不写流缓冲区、FIFO、RX 流缓冲 2 倍、IDLE 重算、按进度判卡死、先中止再结算、DMA 句柄修复 + NVIC 保护、运行中 re-init、`ms_to_ticks()`、DMA / 可达性检查、RS485 软件 DE、在途长度时序（对比评估 C12 表格有两边的位置）
  - 2026-11-04 完成（N11 按“硬件设计没问题就不该有软件 DE”的决定**不恢复**，RS485 只支持硬件 DE；N4 在循环 DMA 下不再需要）。改动逐项见宿主 `docs/bsp_test/uart_backfill_2026-11-04.md`。
- [x] **F2** 调用 HAL 之前先置 `_tx_chunk_active`，失败再改回（评审 F2）
- [x] **BspUart 接收改为 DMA 循环模式**：直接在回填后的实现上做（2026-10-17 记录，原文如下；文中路径、行为指 STM32_BSP 的旧实现）

  **为什么要改**：现在的接收是 DMA Normal 模式 + `HAL_UARTEx_ReceiveToIdle_DMA()`，每次 IDLE / 缓冲写满都由 HAL 停 DMA、库再重新启动。
  这条路径在实机测试（`docs/bsp_test/`）里暴露了一串问题，前三个已在库里打补丁，第四个只能靠配置缓解：

  1. 缓冲写满（TC）一次投递整块，流缓冲区与 DMA 缓冲一样大时紧接着的 IDLE 投递被丢（已改：接收流缓冲区 2 倍）。
  2. IDLE 后到重新启动 DMA 之间的空窗里字节溢出（已改：打开 UART FIFO）。
  3. HAL 先读 DMA 剩余计数、再停 DMA，中间到达的字节没算进 size（已改：停流后按剩余计数重算）。
  4. **HAL V1.11.6 `HAL_DMA_Abort()` 的读-改-写竞争**：先清 EN，紧接着 `CR &= ~(TC|HT|TE|DME)` 又读回 CR，
     流还没真正停下时 EN 仍读 1，被写回去 —— 流重新使能，HAL 再死等 EN 清零。HAL 在 IDLE 中断里调用它，
     HAL 时基（TIM8）优先级 15 低于串口中断 5，超时永远到不了 → 整机卡死。-O2 构建下 U14（双向、帧间 1 tick）9 s 内必现，-O0 下没出现。
     - 现在的缓解（宿主工程 CubeMX）：TIM8 时基中断优先级改为 4（高于串口 / CAN 的 5），HAL_DMA_Abort 5 ms 后超时返回，
       句柄进 ERROR，库的 `rx_recover()` 修复后重建接收。不再卡死，但每次命中丢一小段数据、中断里多停 5 ms。
     - 其他用本库的工程如果 HAL 时基优先级仍是 15，同样会卡死。

  **怎么改**：`init()` 里把 RX DMA 设成循环模式（`hdmarx->Init.Mode = DMA_CIRCULAR` 后 `HAL_DMA_Init()`，或要求 CubeMX 配成 Circular），
  HAL 的 IDLE / HT / TC 回调在循环模式下**不会停 DMA**，只报告当前写到的位置；库按“上次读到的位置 → 当前位置”把新数据投进流缓冲区（处理回绕）。
  这样 IDLE 路径不再调用 `HAL_DMA_Abort()`（根除第 4 条），也没有重新挂接收的空窗（第 1~3 条自然消失），`rx_recover()` 只在真故障时动作。

  **要注意**：

  - 回调里的 Size 是缓冲区内的绝对位置，不是本次长度；HT / TC / IDLE 都要处理，HT 不能再忽略。
  - 错误（ORE / FE / NE）时 HAL 在 DMA 模式下仍会中止接收，`rx_recover()` 的重建路径保留。
  - 改完在宿主工程（测试代码在宿主 `app/test/`）按 `docs/bsp_test/` 全部用例回归，**Debug 与 -O2（RelWithDebInfo）各跑一遍**，并把 TIM8 优先级改回 15 验证不再卡死。
  - （2026-11-03 补充）把 TIM8 改回 15 之前，先完成 1.4 的 A3：BMI088 在调度器启动前用 `HAL_Delay()`，时基优先级 ≥ 5 时会卡在 `board_init()`。

- [x] **I1** 改成 header-only；恢复 `HAL_UART_MODULE_ENABLED` 保护（N24）
- [x] 回填时核对 **N31**：`tx_recover()` 的中止 + 续发要持 `_tx_lock`
- [x] 回填完马上跑 `docs/bsp_test/` 的 UART / RS485 用例（Debug 与 -O2），确认 10-17 的修复都回来了
  - 2026-11-04：记录见宿主 `docs/bsp_test/uart_backfill_2026-11-04.md`。顺带修了测试时钟 `us_now()` 每 7.8 s 回绕一次的错误（R03 偶发“超时”与 10-17 记录里 -O2 下未定位的 1/1000 超时都是它）。
  - Debug 37 / 37、-O2 39 / 39（含 U14 / R06 各 5 分钟浸泡；U14 双向零丢失、零 DMA 错误，10-17 时这里丢 124 帧）。
  - TIM8 改回 15 的验证没做：要先完成 1.4 的 A3（见 1.4 新增的一条）。

### 1.2 CAN 回填（保留 fork 的 `BspCanT<RX, TX>`、re-init 复位回滚、诊断计数、TX-empty 动态开关、ErrorStatusCallback、删 `CanMode`、ms 超时、`const` data）

- [x] 修 N17~N22：`FDCAN_REJECT_REMOTE`、`TXFQS.TFQF`、按 DLC 拷贝、`send()` 临界区、`receive()` 只读回退缓冲（仅 `CAN_RX_ENABLE = 1`，见 N28）、`CAN_BUSOFF_RETRY_MS` 接回且 Bus-Off 只计一次
- [x] **N27** sys_task 加 `tx_recover()`（现在没有任何地方调用它）；`send()` 返回 FULL 前也 `_start_transmission(0)` 踢一次
- [x] **N28** `receive()` 按 `CAN_RX_ENABLE` 分开：1 → 只读回退缓冲；0 → 不建回退缓冲，只读原始缓冲
- [x] **N29** 发送 MessageBuffer 单读者（任务侧取帧放进临界区，或者只让 ISR 取帧），和 N20 一起改
- [x] **N35** 槽位全部注销的发送节点从链表摘除；`service_recovery()` 检查 `xMessageBufferReset` 的返回值
- [x] **N23** header-only（连带 DjiMotor、DeviceR9dsT、DeviceEmmv5GroupT 的显式实例化）
- [x] **I2** 扩展帧发送、接收节点声明 DLC；**F3** 凑齐即发加时限 + 降频计数；**F6** 回退缓冲可选 / 只计数；F9 其余（`StdFiltersNbr`）
- [x] 回填完马上跑 CAN 用例
  - 2026-11-04：记录见宿主 `docs/bsp_test/can_backfill_2026-11-04.md`。Debug：14 / 16（C06c / C06d 为设计限制）；-O2 三组全量 + 5 分钟浸泡：功能用例全部通过，C09 双向 30 万帧零丢失。N35 的 Reset 改为逐帧读空发送缓冲、不再清接收缓冲；顺带修了本次发现的“TX 队列模式下同 ID 乱序”（C06e）。
- [x] 测试固件堆不够 UART + RS485 + CAN 三组一起跑（2026-11-04 -O2 回归最低空闲堆 160 字节，系统检查 S-* 失败；功能用例全部通过）：CubeMX 把 `configTOTAL_HEAP_SIZE` 从 48 KB 加到 64 KB（需用户确认 CubeMX 生成），或者分组烧录
  - 2026-11-04 用户确认后加到 64 KB（CubeMX 命令行生成，只改了 `FreeRTOSConfig.h` 一行）
- [x] 发送节点对同一 ID 只保留最新值（替换而不排队），解决 C06c / C06d 的队头阻塞（10-17 起的设计限制）
  - 2026-11-04：新增 `BspCanT::send_latest()`，发送节点改用它。队列模式 C06d 20 / 20（0.8 ~ 1.2 ms）；FIFO 模式 C06c 仍 0 / 20（硬件按入队顺序发的固有限制），改为只记录，文档写明“有更高优先级流量时用 Queue 模式”

### 1.3 fork 引入的架构问题

2026-11-04 完成，Debug 与 -O2 全组回归通过（含 SPI / BMI088 与新增服务层组），记录 `docs/bsp_test/arch_1_3_2026-11-04.md`。

- [x] **N13** BSP `init()` 校验自己在 X-macro 列表里（不在就 `BAD_ARG`）；SPI 同句柄重复检测
  - UART（1.1）、CAN（1.2）、SPI（1.3，`bsp_spi_is_listed()`）都做完；清单里两个实例绑同一句柄也是 `BAD_ARG`
- [x] **N14** sys_task 去掉 `print_uart`（换成 Q5 的计数 + 回调）
  - `sys_task_stats`（cycles / last_us / avg_us / max_us）+ `system_set_report_hook(hook, period_ms)`；模板 `app_main.cpp` 每 5 s 打到调试串口
- [x] **N15** 清理摆设宏：`CAN_BUSOFF_RETRY_MS` 接回、`UART_ENABLE_FIFO` 恢复作用、删 `CAN_RX_QUEUE_FRAMES` / `CAN_RX_RETURN_FRAMES` / `CAN_TX_QUEUE_FRAMES`
  - 前两项在 1.2 / 1.1 做完；删了 `CAN_RX_QUEUE_FRAMES` / `CAN_TX_QUEUE_FRAMES`；`CAN_RX_RETURN_FRAMES` 在 1.2 已接回代码（回退缓冲深度，0 = 不建），保留；`CAN_BUSOFF_RETRY_MS` 检查改成 ≥ 10（Q5）
- [x] **N16** 运行期 `regist()` 不调 `init_error()`；EventState 传播不清 RUNNING
  - 2026-11-04 在 1.2 里做完：`CanTxNodeT::regist()` 冻结后只返回 `nullptr`（`CanRxNodeT` 本来就是这样）；1.3 里 `init_error()` 改成只在初始化期有效（运行后降级成 `error()`，不清 RUNNING，传播到父节点时同样按父节点是否在运行决定）
- [x] **N24** encoder / servo / motor / r9ds / emmv5 / pwm 补 HAL 模块保护（uart 已在 1.1 恢复）
  - 编译矩阵（全开 / 四个模块全关 / 各关一个）库目标 0 error、0 warning；对照：去掉 pwm 的保护、关 TIM 时 36 个 error
- [x] **N25** `integrate.py` 不再生成强定义 `StartDefaultTask`（检查 As weak，否则写进 USER CODE 段转调）
  - 模板改成 `app_default_task()`；脚本把转调写进 USER CODE StartDefaultTask 段（不论 As weak 与否都成立），As external 时提示。只在本工程上 `--dry-run` 验证过
- [x] `.dma_buffer.<变量名>` 约定封装成宏（`ICBSP_DMA_BUFFER(name)`，`code/bsp/bsp_dma_buffer.hpp`）
- [x] **N30** 库占用的 HAL 回调末尾转调弱函数 `icbsp_unhandled_*()`，让宿主自己的同类外设也能收到回调
  - 8 个：UART rx_event / tx_cplt / error、SPI txrx_cplt / error、FDCAN rx_fifo0 / tx_fifo_empty / error_status；实测 Y05
- [x] **N32** `BspSpi` 改成 constexpr 构造 + 平凡析构 + `deinit()`，`init()` 的重入语义和 CAN / UART 一致
  - 弹跳缓冲要 `= {}` 才是常量初始化（测试里的 `constinit` 探针查出来的）；本工程 `bsp_spi2` 改成 `board_cfg.hpp` 的 inline 实例
- [x] **N36** 阶段 2 默认建树之前，先定 EventState 错误的生命周期（`clear_error()`、`error_code()` 的含义）；`Online::update()` 缩短临界区
  - 错误码粘滞到 `clear_error()` / `clear_errors()`（只清本实例、不传播、不动失败位）；`error_code()` = 未清错误里枚举值最小的，另有 `error_bits()`；`Online::update()` 挂起调度器遍历、每节点单独临界区

### 1.4 评审里的已有问题（fork 里同样存在）

2026-11-04 完成，TIM8 改回 15 的 -O2 验证 2026-11-05 补完，记录 `docs/bsp_test/review_1_4_2026-11-04.md`。

- [x] **F1** 四元数初值改为单位四元数，加 `attitude_ready()`；`rotation_vector()` / `normalized()` 防零模长；PID 遇到非有限输入时丢弃并计数（评审 F1）
  - `ImuData` 四元数初值单位四元数、`attitude_ready()`、四元数零模长 / NaN 防护、PID `nonfinite_count()`；实测 Y06 / Y07
- [x] **A1** 初始化动作挪出 `configASSERT`（`system.cpp`、模板、编码规范）；`configASSERT` 改成 `do { } while (0)` 形式（评审 A1）
  - `ICBSP_CHECK(expr)`（`fault_record.hpp`），库 / 模板 / 宿主 / 编码规范全部改用；实测 Y11
- [x] **F4** PID 积分上限在计算时取实际值（`i_max == 0` 时跟随 `out_max`），链式配置和 Config 配置语义一致（评审 F4）
  - 实测 Y06：链式配置与 Config 配置积分都限在 out_max
- [x] **F5** DjiMotor 提供 `snapshot()`，去掉没用的互斥锁；R9DS 同样处理（评审 F5）
  - DjiMotor 写回改短临界区（不再 try-lock 丢帧）；R9DS 同样加 `snapshot()`，顺带做了 N34（诊断计数 `uint32_t`）
- [x] **A3** BMI088 调度器启动前改用 DWT 忙等（fork 已有 `bsp_dwt`），不再依赖 HAL 时基中断优先级。**1.1 的 DMA 循环模式要把 TIM8 改回 15 验证，必须先完成这一项。**（评审 A3）
  - BMI088 用 `bsp_dwt.delay_ms()`；另外 UART 停 DMA 流的两处 `HAL_GetTick()` 等待也改成 CYCCNT + 循环上限
- [x] A3 完成后：CubeMX 把 TIM8 时基优先级改回 15，Debug 与 -O2 跑 UART / RS485 全部用例 + U14 浸泡，确认不再卡死（1.1 的循环 DMA 已去掉串口中断里的 `HAL_DMA_Abort()`）
  - 2026-11-04 用户确认后 CubeMX 改成 15。Debug 与 -O2（各五组 + 5 分钟浸泡）都通过：正常启动、U14 / R06 / C09 / P05 无损（-O2 于 2026-11-05 补跑）
- [x] **A2** 库任务默认优先级按 `configMAX_PRIORITIES` 取，加 `static_assert`（含 BMI088；fork 的 sys 是 +6，与 BMI088 同级）（评审 A2）
  - CAN −2 / BMI088 −3 / sys −4（54 / 53 / 52）；实测 Y09
- [x] **A4** DMA 可达性按 DMA1/2 和 BDMA 分别判断；合并两份 `dma_reachable`（评审 A4）
  - `icbsp_dma_reachable()`（`bsp_dma_buffer.hpp`）；实测 Y10
- [x] **A5** 新增 `fault_on_exception()` 和 HardFault / MemManage / BusFault 的 USER CODE 片段（评审 A5）
  - 另有 UsageFault；`integrate.py` 自动写入 `stm32h7xx_it.c`。实测：故意的 BusFault 记下 PC = 出错函数里的地址，复位后 Y12 读到
- [x] **F11** 去掉 `dji_motor.cpp:311` 的 `static_cast<float>`（fork 只把字段改成了 double，存进去的仍是 float 精度）
  - 多圈角度直接存 double
- [x] 其余低级别条目：A6（`algorithm/` 不依赖 RTOS）、A8、A9、F7 其余部分、F8、F10、I4、I5、I6 / I7（和 Q10 一起）、D3、PWM 去掉默认构造（I8 剩余）、DWT `get_time_s()` 线程安全与回绕（C11：加临界区；sys_task 保留周期性的回绕跟踪，Q5 去掉打印时不能一起删掉）、N37（编码器 ARR 检查、舵机跨任务变量改 atomic、`PID::calc` 的 `df_dt` 改名或按秒计）
  - 做了：A6（`status_code.hpp`）、A8（上电清零 + `boot_rsr`）、A9（弱 `__malloc_lock`）、F7（RX 分类计数、单读者、`tx_recover` 波特率 0、`printf` 返回码）、F8（刷新锁、文档）、F10（ARPE、64 位周期、`set_freq` 返回 `Status`）、I4、D3（`-Wall -Wextra -Wshadow`、`CXX_EXTENSIONS OFF`）、PWM 去默认构造、C11（64 位时间轴 + 关中断区，sys_task 每 10 ms 跟踪）、N37（编码器 ARR、舵机 atomic、`delta_feedback`）
  - **推迟到阶段 2 的 Q10 命名统一**：I5、I6、I7（都是改名 / 返回值约定，避免同一批接口改两次）
- [x] 回填时**不能弄丢** fork 已修好的：F9（re-init / RF0L）、I3、I8（GPIO / Key）、Key 长按饱和、F11 的 double 字段类型、F7（`hw_error_cnt`）、PID 去 virtual / `print`、A7（可加载 `.dma_buffer`）
  - 核对过：F9（RF0L 计数、re-init）、I3（`CanMode` 已删）、I8（GPIO 无默认构造）、Key 长按饱和、F11 double 字段、F7 `hw_error_cnt`、PID 无 virtual / `print`、A7 可加载 `.dma_buffer` 都在
- [x] 1.1~1.4 完成后按对比评估附录 B 逐条核对 N1~N37
  - 核对结果见 `docs/bsp_test/review_1_4_2026-11-04.md` 末尾：除 N11（按决定不恢复软件 DE）外 1.1 ~ 1.4 都做了；N26（文档悬空引用）、N33（Emm_V5 接口）按原计划在后面的阶段

## 阶段 2：加入新功能

- [x] 改名 **ICBSP**（Q15）：仓库名、CMake 目标 `icbsp`、配置宏 / 开关加 `ICBSP_` 前缀、文档前缀、`LICENSE` 版权人 INVC & CUBOT
  - 2026-11-06：CMake 目标 `icbsp`；宿主提供的配置宏、功能开关、实例清单（`ICBSP_CAN/UART/SPI_INSTANCES`）和系列宏（`ICBSP_SERIES_H7/F4`、`ICBSP_CAN_MODULE_ENABLED`、`ICBSP_UART_HAS_FIFO`）加前缀；README / 文档 / integrate.py / 模板改名，`LICENSE` 版权人 INVC & CUBOT。类名（`BspUartT` 等）与头文件保护宏不改。历史记录（`docs/bsp_test/`、对比评估、评审、F4 移植计划）保持原文。**GitHub 仓库改名（stm32-framework → ICBSP）要在网页上做**，做完宿主 `.gitmodules` 的 url 跟着改
- [x] 目录重排（Q13）：`code/{bsp, service/{system,comm,monitor,definition}, device/{motor,sensor,remote,actuator}, algorithm, tasks}`，包含路径只加 `code/`（草案见对比评估 3.1）
  - 2026-11-06（分支 `phase2`）：按草案 `git mv`，`protocol/emmv5_protocol` 并进 `device/motor/`；包含语句全部写成 `"bsp/bsp_uart.hpp"` 形式（库、模板、宿主 `app/`）。bsp 改为只包含 `service/system/status_code.hpp`（原来经 `status.hpp` 间接带进 FreeRTOS 事件组），它是唯一允许各层包含的公共头；新增 `tools/check_layers.py` 检查分层。文档只改了目录描述，按新目录重写留到阶段 4
- [x] `examples/dm_mc02/Flash/README.md` 的经验（OpenOCD 0.12、不注册 OCTOSPI2 bank）补进 `templates/Flash` 和文档；README 的示例仓库链接先空着，阶段 6 再补（Q18）
  - 2026-11-06：`daplink.cfg` / `jlink.cfg` 注明不注册外置 Flash bank、HID 后端（注释掉，按探针打开）；四个脚本支持 `OPENOCD` 环境变量，`.sh` 检测到 0.11 及以下时提示；`templates/Flash/README.md` 加实测经验（含烧录 CRC 造成的假 HardFault），接入指南常见问题加三条。README 没有示例仓库链接（保持空）
- [x] 配置检查（Q3）：库内 `config_check.hpp`（范围 `static_assert` + 缺宏 `#error`），模板同步
  - 2026-11-06：`code/service/system/config_check.hpp` 是库读配置的唯一入口（库内不再直接包含 `hardware_config.h`），依次做缺宏 `#error`（21 个宏）→ HAL 模块没开时强制关开关 → 开关依赖 → 取值范围；`system.cpp` / `device_bmi088.cpp` 里零散的检查、`bsp_can` / `bsp_fdcan` / `can_tx_node` 里的缺宏检查并进来。模板删掉了检查段，宿主 `Core/Inc/hardware_config.h` 按模板重新同步。反向测试：缺宏、非法值、依赖不满足、关 CAN 发送后包含 DjiMotor 都会报错
- [x] 功能开关（Q4）：放进 `hardware_config.h`，注释写清后果，依赖用 `static_assert`
  - 2026-11-06：模板里四个开关逐个写明关掉之后失去什么；依赖：`ICBSP_UART_PATROL_ENABLE` 需要 `ICBSP_SYS_TASK_ENABLE`（static_assert），DjiMotor 需要 CAN 收发都开（开了 CAN 外设却关了任一路径时 `#error`，没开 CAN 外设仍是空头文件）
- [x] sys_task（Q5）：10 ms；去掉开机 1 s 等待；`sys_task_stats`（DWT 计时）+ 可选回调；`CAN_BUSOFF_RETRY_MS` 等按 10 ms 粒度检查
  - 10 ms、`sys_task_stats` + `system_set_report_hook()`、`ICBSP_CAN_BUSOFF_RETRY_MS ≥ 10` 在 1.3 / 1.4 已做；2026-11-06 去掉开机 1 s 等待；串口发送卡死阈值（一包耗时 × 2）加下限 10 ms（= 巡检周期；高波特率下阈值只有 1 tick 时，刚开始的传输可能被误判卡死并中止）。Online 的离线判定本来就按 ms，巡检周期只决定判定延迟，不用改
- [ ] EventState 默认树（Q6）：`sys_state` → BSP 实例 → 设备；子节点错误只置父错误位、不清 RUNNING；照搬 fork 81 条判据并补建树用例
- [ ] 命名统一（Q10）：GPIO / Key / PWM 改名；编码规范写入“查询 `get_`、布尔 `is_` / `has_`”并扫一遍全部接口（GPIO 改回 `get_port/get_pin`）
- [ ] 编码器进核心库 `bsp/bsp_encoder`（Q11 更正），补 `HAL_TIM_MODULE_ENABLED` 保护
- [ ] 设备并入核心（Q12 / Q17）：DeviceMotor（`device/motor/`）、DeviceServo、DeviceR9dsT（加 `snapshot()`；诊断计数改 `uint32_t`，N34）、DeviceEmmv5GroupT（去掉写死的 128 字节串口缓冲尺寸；N33：`get_position/get_status` 改名 `take_`、超时统一 `ms_to_ticks()`、校验方式可配）
- [ ] **dm_motor（达妙）新写**：MIT / 位置速度 / 速度模式；使能、失能、清错误、设零点；反馈解包（ID、错误码、位置、速度、扭矩、温度）；`P_MAX` / `V_MAX` / `T_MAX` 可配；Online、`snapshot()`；接口风格对齐 DjiMotor
- [ ] DR16 接收机（DT7 遥控器，DBUS）
- [ ] ET08 接收机
- [ ] 保留 fork 已有的：UART 七项（Q8）、CAN 三项（Q9）、Online ms + `is_online`（Q7）、DWT（Q11）、`can_tx_task` 按需创建（C10）、QuaternionPID 拆文件、PID `get()`；Matrix `_data` 改回 private + `friend`
- [ ] 每加一项都补 HAL 模块保护（N24），对照附录 B 核对 N1~N37

## 阶段 3：功能全面测试

- [ ] 主机侧单元测试：PID（F4、NaN）、四元数（F1、零模长）、Matrix、RLS，放进 CI
- [ ] 编译矩阵：功能开关 / HAL 模块（FDCAN、UART、SPI、TIM）逐个关，各组合 0 warning；`CAN_RX_ENABLE = 0` 时实际跑一次 `receive()`（N28）
- [ ] BSP 实板回归：`docs/bsp_test/` 的 UART / RS485 / CAN / SPI + BMI088 全部用例 + UART IT 模式，Debug 与 -O2 各一遍，加浸泡测试
- [ ] DMA 循环模式专项测试（TIM8 = 15 不卡死）；F2 短帧压力测试
- [ ] Service：EventState 树（81 条 + 默认树传播 + 不清 RUNNING）、Online 毫秒语义、N16 回归、N13 回归（漏写实例时 `init()` 返回 `BAD_ARG`）
- [ ] 新功能实板：CAN TX-empty 动态开关（丢唤醒统计；先修 N27，再人为关一次中断，确认 `tx_stall_recover` 增加、总线恢复）、ErrorStatusCallback 计数、DWT（两个任务并发调 `get_time_s()`，跨 10 圈回绕不跳变）、编码器、N30 的转调钩子、R9DS / 舵机 / EmmV5 / dm_motor / DeviceMotor（没硬件的标“只编译验证”）
- [ ] 全程不打开 BMI088 加热电阻（PB1 / TIM3_CH4）；结果按 `docs/bsp_test/` 的格式存档

## 阶段 4：文档补全

- [ ] 采用 fork 的文档结构（Q16），以 ICBSP 名字和新目录重写；`docs/code/` 跟随代码子目录，架构图用 `tools/draw_diagrams.py` 重画
- [ ] 修掉 D2 和 N26
- [ ] 修掉复核补进 N26 的不一致：README“每 2 s 打印”（实际 5 s）、`board_cfg.cpp` 头注释与示例（DWT 已由库初始化、舵机不是 CAN 设备、Emm_V5 不是达妙）、monitor“每秒”（实际 10 s）、`bsp_spi.cpp` 引用不存在的 `ms_to_ticks()`、`bsp_spi.hpp` 示例段名、`integrate.py` 收尾提示、`CAN_RX_NODE_MAX` 注释
- [ ] 接入指南补齐：C++20；宿主两个文件的职责；宿主不能定义的 HAL 回调清单（含 `HAL_FDCAN_ErrorStatusCallback`）及 `icbsp_unhandled_*()` 转调（N30）；编码器 ARR 要求（N37）；FDCAN `MessageRAMOffset` / `StdFiltersNbr`；时基优先级的两条理由；newlib 锁；BDMA 外设限制
- [ ] 功能开关逐个写明关掉之后的后果
- [ ] 编码规范：`get_` / `is_` 规则、包含路径规则、C++20 规则（volatile、聚合体 Config、平凡析构）、`.dma_buffer` 宏
- [ ] 每个模块如实标注“已实测 / 只编译验证”；`docs/bsp_test/` 注明对应的代码版本
- [ ] 许可证说明（matrix 授权来源、VQF SPDX、厂商 PDF 去留）

## 阶段 5：web_ui 接入帮助（Node.js 前端 + `integrate.py` 引擎，Q14 = A）

- [ ] `integrate.py` 合入 fork 扩展后加机器接口：`--json`（检查结果、外设、可挂设备）、`--plan` / `--dry-run`（diff）、`--apply <选择.json>`
- [ ] 库侧清单 `tools/manifest.json`（Python 读取）：外设 → BSP 类、模板参数、必需的 CubeMX 设置；设备目录 → 依赖、参数、默认值、`init()` 顺序；校验规则
- [ ] `.ioc` 校验：现有检查 + D1 缺的检查（`TICK_INT_PRIORITY < 5`、Message RAM 重叠、`StdFiltersNbr`、`MasterKeepIOState`、DMA 模式、D-Cache、NVIC、defaultTask As weak）
- [ ] 生成 `board_instances.hpp` + `board_cfg.hpp` 实例（can1 / can2 → `bsp_can1` / `bsp_can2`，参数取自 `.ioc`，自动套 `.dma_buffer` 宏）+ `init()` + `hardware_config.h`（开关按勾选生成）
- [ ] 可选设备：按外设列出可挂设备，勾选后生成实例、`init()`、回调转调；设备只在依赖的外设和开关都满足时出现；DR16 / ET08 标“计划中”
- [ ] 与宿主同步：只写生成区 / USER CODE 段，可重复运行，写盘前给 diff 预览，遇到手改内容就停下
- [ ] Node 前端只做展示和收集选择，不复制接入逻辑；找不到 Python 时明确提示
- [ ] 生成快照测试：MC02_with_submodule 的 `.ioc` + 一个全新工程，生成的代码 0 warning 编译

## 阶段 6：宿主接入回归测试

- [ ] 全新 CubeMX 工程：H723 + 至少一个其他型号（H743 / H750），分别用 `integrate.py` 和 Web UI 接入，0 warning（C++20），上板冒烟
- [ ] 通过回归的 H723 测试宿主工程作为新的示例工程（Q18）：整理后推到组织下，README 写上链接
- [ ] CubeMX 重新生成后再接入一次，确认幂等
- [ ] MC02_with_submodule 把 1.0 的分支合回 main，submodule 指向 ICBSP；更新宿主 `CLAUDE.md`（库路径 / 目录、回调清单加 ErrorStatusCallback、C++20、去掉 `examples/dm_mc02` 引用、烧录参照改 `templates/Flash`）；跑阶段 3 的回归；clangd `--check` 0 诊断
- [ ] 把遇到的问题回填到接入指南的“常见问题”

## 阶段 7：发布 v1.0.0

- [ ] 许可证审计（A10）：根目录 LICENSE（版权人 INVC & CUBOT）、matrix 书面授权、VQF SPDX、厂商 PDF 与仓库体积
- [ ] 版本号宏 `ICBSP_VERSION_MAJOR/MINOR/PATCH`，清单 / Web UI 校验版本匹配
- [ ] `CHANGELOG.md`（自 `43a34cf` 起，含 STM32_BSP 10-17 实测修复的来源提交、fork 的提交与作者）
- [ ] 打 tag `v1.0.0` + release notes（发布方 INVC & CUBOT；支持型号、已实测模块、已知限制）
- [ ] 宿主更新 submodule 指针并提交；`STM32_BSP` 在 GitHub 上归档（Q19）

