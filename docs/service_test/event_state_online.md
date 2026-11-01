# EventState 父子树 / Online 运行时链表 实板测试（2026-11-01）

对 `service/status`（`Status` + `EventState`）与 `service/online_check`（`Online`）在 DM-MC02 上做实板测试：
接口边界、错误传播、生命周期，以及 `Online` 的毫秒语义、多节点、运行期增删。

- 对应合并方案里的验收项：**P1 Service — `EventState` 树（`set_parent` / 传播 / `error_code`）、`Online` 毫秒语义**。
- 测试代码在宿主工程 `user/app/test/`（不在库里）；**库侧 0 改动**，本轮未发现缺陷。
- 实板结果：**81 / 81 判据通过**。

> 状态：本轮验证完成后，宿主工程里的测试代码（`user/app/test/` 六个文件、`app_main.cpp` 的挂载）**已删除**，
> 工程恢复到无测试的状态；本文件保留用例表、判据与原始实测结果，需要复跑按下方“怎么跑”重建那七个文件。

## 环境

| 项 | 取值 |
| --- | --- |
| 板 | DM-MC02（STM32H723VGT6），USB 供电 |
| 探针 | SEGGER J-Link V9（OpenOCD 0.12.0 的 `jlink` 适配器，SWD 1800 kHz） |
| 构建 | Debug（`-O0 -g3`），Flash 121 000 B / 11.54 %，DTCMRAM 76 072 B / 58.04 % |
| 调试串口 | USART1（PA9 / PA10，115200）；**本机没接 USB-TTL，结果走 RAM 表读取** |
| 内核 | FreeRTOS，tick 1 kHz，heap_4（48 KB） |

## 怎么跑

1. 开关在 `user/app/test/app_test.hpp`：`APP_SERVICE_TEST_ENABLED 1`。测完改回 0。
2. `app_init()` 里创建 `svc_test` 任务（优先级 4，栈 896 字）：先 `sys_state.wait_running()`，
   让出 100 ms 等 `sys_task` 的 `Online::update()` 跑起来，再依次跑 E、O 两组用例，最后置 `g_test_done`。
3. 结果两种读法：
   - 串口：每行 `<用例号>|<判据描述>|PASS 或 FAIL|v=<观测值>`，最后一行 `########## done pass=N fail=M ##########`。
   - 没有串口线时读 RAM 结果表（本轮用的这条）。符号地址从 ELF 取：
     ```bash
     arm-none-eabi-nm --print-size build/Debug/dm_mc02.elf | grep g_test_
     # 本次：g_test_log=0x2000ea2c(0x28a0) log_count=0x200112cc pass=0x200112d0 fail=0x200112d4 done=0x200112d8
     openocd -f Flash/jlink.cfg -c "init" -c "halt" \
       -c "dump_image /tmp/log.bin 0x2000ea2c 0x28a0" -c "shutdown"
     ```
     `g_test_log` 是 `char[200][52]`，按 52 字节一行切即可。
4. 用例时长：E 组 < 0.2 s，O 组约 2.6 s，复位后约 3 s 全部跑完。

## 用例与判据

### EventState（E01~E14）

| 编号 | 测试项目 | 测试目的 | 测试方案 | 判据 |
| --- | --- | --- | --- | --- |
| E01 | 未 init 的安全返回 | 文档承诺「未 init 时除 init() 外全部安全」 | 对未 init 实例调全部接口 | `is_ready=false`、`running=false`、`error`/`init_error`/`complete_init` 返回 `NOT_INIT`、`error_code=NOT_INIT` |
| E02 | init() 幂等 | 二次 init 不得重建事件组（否则泄漏） | init 两次，前后比 `xPortGetFreeHeapSize()` | 两次都 `OK`，堆余量不变 |
| E03 | complete_init() | 置运行位、不动错误码 | init → complete_init | `running=true`、`error_code=OK` |
| E04 | error() 参数与运行位 | 运行期错误只记码 | `error(OK)`、`error(TIMEOUT)`，穿插查 running | `error(OK)=BAD_ARG`；`TIMEOUT` 可记录；running 始终为 true |
| E05 | 多错误码的可见结果 | 位号即枚举值，取最小者 | 依次记 `IO_ERROR(4)`、`FULL(3)`、`BUSY(1)` | `error_code=BUSY`；之后再 `complete_init()` 仍 `OK` |
| E06 | init_error() 粘滞 | 初始化失败后不得再进入运行态 | init → complete_init → `init_error(OK)` / `init_error(IO_ERROR)` → 再 complete_init，再 `init_error(TIMEOUT)` | `init_error(OK)=BAD_ARG`；running 被清；`error_code=IO_ERROR`；complete_init 返回 `NOT_SUPPORTED`；第二次 init_error 后 `error_code=TIMEOUT`，仍 `NOT_SUPPORTED` |
| E07 | set_parent() 边界 | 自己、父未 init、重复挂接都要拒 | 三种调用各一次 | `BAD_ARG` / `NOT_INIT` / 第二次 `BAD_ARG` |
| E08 | set_parent() 环检测 | 父子链不能成环 | A→B 成功后 B→A | `A→B=OK`、`B→A=BAD_ARG` |
| E09 | 三级链的初始化失败传播 | `init_error` 要一路传到祖父并停掉各级 | 孙/子/父三级挂链后 `leaf.init_error(IO_ERROR)` | 三级 `error_code=IO_ERROR`、三级 running 全为 false |
| E10 | 运行期错误的传播方向 | 只向上、不动运行位、不向下 | 三级链 `leaf.error(TIMEOUT)`；另一对父子 `parent.error(BUSY)` | 子/父/祖父都记到 `TIMEOUT` 且 running 全 true；父记 `BUSY` 后子仍是 `OK` |
| E11 | 析构归还事件组 | `~EventState` 释放句柄 | 作用域内 init + complete_init + error 后离开作用域 | 堆余量复原 |
| E12 | wait_running() 未 init | 文档：删除当前任务 | 探针任务调 `wait_running()` | 探针任务消失（任务数还原）、函数未返回 |
| E13 | wait_running() 初始化失败 | 同上，且不能永久阻塞 | 先 `init_error(IO_ERROR)`，探针任务调 `wait_running()` | 同上 |
| E14 | wait_running() 已运行 | 运行态应立刻返回 | complete_init 后探针任务调 `wait_running()` | 探针任务返回并自行退出 |

### Online（O01~O11）

| 编号 | 测试项目 | 测试目的 | 测试方案 | 判据 |
| --- | --- | --- | --- | --- |
| O01 | 构造即离线 | 未收到有效数据前必须是 `TIMEOUT` | 构造后直接查，再手动 `update()` 复查 | 两次都 `TIMEOUT` |
| O02 | refresh_task() | 刷新不等 update | 刷新后立即查 | 返回 `OK`，`is_online()=OK` |
| O03 | 超时由 `sys_task` 推进 | 判定不依赖调用方自己 update | gap=100，刷新后自己**不**调 update，睡 150 ms | 150 ms 后 `TIMEOUT` |
| O04 | 毫秒语义 | threshold 是毫秒，与 update 周期无关 | gap=1000；500 ms 查一次，再睡 800 ms 查一次 | 500 ms 在线、1300 ms 离线 |
| O05 | `timeout_gap=0` | 归一为 1 ms，不是「永不超时」 | 构造 gap=0 节点，刷新 + 睡 20 ms | 刷新后在线、20 ms 后离线 |
| O06 | refresh_isr() | ISR 版刷新接口可用 | 先超时，再 `refresh_isr()` | 返回 `OK`，状态回到在线 |
| O07 | 多节点独立计时 | 每个节点用各自的阈值 | 30 / 100 / 300 ms 三个节点同时刷新；150 ms 与 400 ms 各查一次 | 150 ms：30 与 100 离线、300 在线；400 ms：全部离线 |
| O08 | 刷新后立刻 update | 同一 tick 内不得误判 | 刷新后手动 `update()` | 仍在线 |
| O09 | 运行期摘除链头/尾/中间 | `_head` / `_tail` 维护正确 | 放置 new 手工控制构造/析构：删头后查其余两节点；删尾后再建新节点；删中间后查尾节点 | 剩余节点与新节点**都能从 OK 被判成 TIMEOUT**（说明 update 走到了它们） |
| O10 | 100 轮临时增删 | 反复构造/销毁不留残链 | 常驻节点存在时，100 次作用域内构造 + 刷新 | 常驻节点仍被推进（能超时） |
| O11 | 100 节点一次遍历 | 链表长度与遍历耗时 | 100 个函数内静态节点全部刷新，60 ms 后统计 | 100 个全部离线；顺带记一次 `update()` 耗时 |

> O 组的链表正确性都是**间接**验证：`_head` / `_tail` 是私有静态成员，没有计数或遍历接口，
> 因此判据统一写成「节点是否还能被 update 判成 TIMEOUT」——只有 update 走到了它才会变。

## 实测结果

| 组 | 判据数 | 通过 | 失败 |
| --- | --- | --- | --- |
| EventState（E01~E14） | 56 | 56 | 0 |
| Online（O01~O11） | 25 | 25 | 0 |
| 合计 | **81** | **81** | **0** |

关键数据：

- `Online::update()` 遍历 **100 个节点 13.07 µs**（Debug `-O0`），占 10 ms 周期的 **0.13 %**。
- O09 三处删节点后，剩余节点与新节点全部能正常超时到 `TIMEOUT`：链表头、尾、中间的注销路径都正确。
- E12 / E13 中 `wait_running()` 确实没有返回、任务被删除；E14 中正常返回。

### 值得记住的行为细节

1. **`error_code()` 取的是枚举值最小的码，不是最后记的码**（E05、E06）。先记 `IO_ERROR(4)` 再记 `TIMEOUT(2)`，
   `error_code()` 返回 `TIMEOUT`；反过来先记 `BUSY(1)`，之后记什么都仍然是 `BUSY`。排查时不要以为是「错误码被覆盖」。
2. **运行期 `error()` 永不改运行位**（E04、E10），`init_error()` 才会清运行位并把失败位永久置上（E06）。
3. **错误只向上传播**（E10）：父节点记错误不会让子节点变成错误态。
4. **`wait_running()` 会删掉调用它的任务**（E12、E13）。它只能在「初始化失败就该退出的任务」里调用，
   不要在主任务或调试任务里试。
5. **`timeout_gap = 0` 等价于 1 ms**（O05），不是「关掉检测」。

## 已知限制与未覆盖

| 项 | 现状 | 建议 |
| --- | --- | --- |
| 父节点必须先于子节点析构 | `_parent` 是裸指针，父析构后子再调 `error()` / `init_error()` 会写已释放的事件组 → 未定义行为（本次**故意不构造**该场景，避免跑崩） | 文档已有约定；若要运行时保护，需要在析构里清理子节点的父指针，或加调试期的存活标记 |
| `Online` 链表不可观测 | 私有 `_head` / `_tail`，无计数、无遍历接口，只能间接验证 | 需要现场诊断时临时加 `static uint32_t count()` 之类的只读接口 |
| `update()` 在任务临界区里遍历整条链表 | 100 节点 13 µs 可接受；节点数上百且周期更短时会拉长中断延迟 | 节点规模变大时分批遍历，或改为只标记、由查询侧计算 |
| 未覆盖：tick 回绕 | 需要 49.7 天或修改 tick | 代码用无符号差值，构造时还依赖 `now - gap` 下溢（开机早期构造的节点就是下溢场景，已随 E/O 用例隐式跑过） |
| 未覆盖：`refresh_isr()` 在真正的 ISR 上下文 | 本轮在任务上下文调（`taskENTER_CRITICAL_FROM_ISR` 在两种上下文都成立） | 接一个真实中断源再跑一次 |
| 未覆盖：`-O2` 与压力竞态 | 只跑 Debug；未构造 refresh/update 的并发压力 | `build/Release` 复跑一次；并发用两个任务对打 |

## 改动清单（宿主工程，未改库）

> 下表列出的七个文件在做完本轮验证后已全部删除，工程恢复原状；保留此表作为复跑时的重建依据。

| 文件 | 内容 |
| --- | --- |
| `user/app/test/app_test.hpp` | 总开关 `APP_SERVICE_TEST_ENABLED`、任务栈/优先级、三个入口声明 |
| `user/app/test/test_report.hpp` | `check()` / `title()` / `note()`：串口打印 + RAM 结果表 |
| `user/app/test/test_log.cpp` | 结果表实体（`g_test_log` / `g_test_log_count` / `g_test_pass` / `g_test_fail` / `g_test_done`） |
| `user/app/test/test_event_state.cpp` | E01~E14 |
| `user/app/test/test_online.cpp` | O01~O11 |
| `user/app/test/test_runner.cpp` | `svc_test` 任务：等运行态 → 跑两组 → 打汇总 |
| `user/app/app_main.cpp` | `app_init()` 里按开关创建 `svc_test` 任务 |

## 原始结果

Debug 轮，`pass=81 fail=0 done=1`：

```text
# E01 not inited
E01|is_ready|PASS|0
E01|running|PASS|0
E01|error_code=NOT_INIT|PASS|6
E01|error=NOT_INIT|PASS|6
E01|init_error=NOT_INIT|PASS|6
E01|complete_init=NOT_INIT|PASS|6
# E02 init idempotent
E02|init=OK|PASS|0
E02|is_ready|PASS|1
E02|error_code=OK|PASS|0
E02|running=false|PASS|0
E02|second init=OK|PASS|0
E02|second init no alloc|PASS|0
# E03 complete_init
E03|complete_init=OK|PASS|0
E03|running=true|PASS|1
E03|error_code=OK|PASS|0
# E04 error runtime
E04|error(OK)=BAD_ARG|PASS|0
E04|running kept|PASS|1
E04|error(TIMEOUT)=OK|PASS|0
E04|error_code=TIMEOUT|PASS|2
E04|running kept after error|PASS|1
# E05 error minimal code
E05|min code=BUSY|PASS|1
E05|still running|PASS|1
# E06 init_error sticky
E06|init_error(OK)=BAD_ARG|PASS|5
E06|init_error=OK|PASS|0
E06|running cleared|PASS|0
E06|error_code=IO_ERROR|PASS|4
E06|complete_init=NOT_SUPPORTED|PASS|7
E06|running still false|PASS|0
E06|new min code=TIMEOUT|PASS|2
E06|sticky after 2nd|PASS|7
# E07 set_parent args
E07|self=BAD_ARG|PASS|5
E07|parent not init=NOT_INIT|PASS|6
E07|attach=OK|PASS|0
E07|attach twice=BAD_ARG|PASS|0
# E08 set_parent cycle
E08|a->b=OK|PASS|0
E08|b->a=BAD_ARG|PASS|5
# E09 init_error 3 levels
E09|mid->grand=OK|PASS|0
E09|leaf->mid=OK|PASS|0
E09|all running|PASS|7
E09|leaf init_error=OK|PASS|0
E09|leaf stopped|PASS|0
E09|mid code=IO_ERROR|PASS|4
E09|mid stopped|PASS|0
E09|grand code=IO_ERROR|PASS|4
E09|grand stopped|PASS|0
# E10 error propagation
E10|mid got TIMEOUT|PASS|2
E10|grand got TIMEOUT|PASS|2
E10|running kept|PASS|7
E10|parent error not downward|PASS|0
# E11 destructor frees
E11|heap restored|PASS|0
# E12 wait_running not inited
E12|probe task exited|PASS|0
E12|wait_running not return|PASS|0
# E13 wait_running init failed
E13|probe task exited|PASS|0
E13|wait_running not return|PASS|0
# E14 wait_running running
E14|probe task exited|PASS|0
E14|wait_running returned|PASS|1
# O01 constructed offline
O01|fresh=TIMEOUT|PASS|2
O01|still TIMEOUT after update|PASS|2
# O02 refresh_task
O02|refresh_task=OK|PASS|0
O02|online immediately|PASS|0
# O03 timeout by sys_task
O03|online after refresh|PASS|0
O03|offline at 150ms|PASS|2
# O04 millisecond semantics
O04|online at 500/1000ms|PASS|0
O04|offline at 1300/1000ms|PASS|2
# O05 gap=0 as 1ms
O05|online right after refresh|PASS|0
O05|offline at 20ms|PASS|2
# O06 refresh_isr
O06|offline before|PASS|2
O06|refresh_isr=OK|PASS|0
O06|online after isr refresh|PASS|0
# O07 independent nodes
O07|30ms node offline|PASS|2
O07|100ms node offline|PASS|2
O07|300ms node online|PASS|0
O07|300ms node offline|PASS|2
# O08 update right after refresh
O08|update=OK|PASS|0
O08|still online|PASS|0
# O09 unlink head tail middle
O09|after unlink head: b advanced|PASS|2
O09|after unlink head: c advanced|PASS|2
O09|after unlink tail: new d advanced|PASS|2
O09|after unlink middle: g advanced|PASS|2
# O10 100 rounds add/remove
O10|anchor still advanced|PASS|2
# O11 100 nodes traversal
O11|all 100 advanced|PASS|100
O11|update 100 nodes 13.07 us
```
