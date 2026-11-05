/**
 * @file fault_record.hpp
 * @author ChoseB
 * @brief 故障现场记录 —— 断言失败、栈溢出、内存分配失败时留下现场再停机
 * @version 0.1
 * @date 2026-10-14
 *
 * @copyright Copyright (c) 2026
 *
 * @details 故障异常（HardFault / MemManage / BusFault / UsageFault）用 fault_on_exception() 记录（stm32h7xx_it.c 的 USER CODE 段）。
 *          上电（POR / BOR）时 fault_record 整体清零（AXI SRAM 带 ECC，上电后的内容和校验位都是随机的），复位原因记进 boot_rsr。
 *          还提供 ICBSP_CHECK(expr)：有副作用的初始化检查（init()、xTaskCreate()）用它，不会随断言一起被编译掉（评审 A1）。
 *          三个入口（C 链接，内核与 freertos.c 也能调）：
 *            - fault_on_assert()         —— configASSERT 失败（定义见 FreeRTOSConfig.h 的 USER CODE 1）
 *            - fault_on_stack_overflow() —— vApplicationStackOverflowHook（freertos.c 的 USER CODE 4）
 *            - fault_on_malloc_failed()  —— vApplicationMallocFailedHook（freertos.c 的 USER CODE 5）
 *          都先关中断、把现场写进 fault_record，再停机：接着调试器时执行 BKPT 直接停在现场，
 *          没接调试器就死循环（与原来的 configASSERT 行为一致）。
 *
 * @note fault_record 放在 .noinit 段（链接脚本里 RAM_D1 上的 NOLOAD 段）：启动代码不清零，
 *       看门狗 / 复位键 / 调试器复位之后仍能读到上一次的现场；掉电后内容随机，靠 magic 判断是否有效。
 *       用调试器查看：`print fault_record`。
 *
 * @note 记录本身不调用任何可能再次断言的 RTOS 接口；重入（记录过程中又触发故障）直接停机。
 *
 * @note RAM_D1（AXI SRAM）带 ECC：不足 64 位的写入先留在控制器的缓冲里，等写到另一个 64 位字时才落盘，
 *       缓冲里的那一笔遇到复位就丢（实测：最后写的行号 / 任务名复位后变回旧值）。所以停机前要往
 *       单独的 flush 字再写一次，把前面的写入挤出去。
 */

#ifndef __SERVICE_FAULT_RECORD_HPP__
#define __SERVICE_FAULT_RECORD_HPP__

#include <stdint.h>


/** @brief 故障种类 */
enum class FaultKind : uint32_t
{
  NONE           = 0, ///< 没有记录
  ASSERT         = 1, ///< configASSERT 失败
  STACK_OVERFLOW = 2, ///< 任务栈溢出
  MALLOC_FAILED  = 3, ///< pvPortMalloc 失败
  HARD_FAULT     = 4, ///< HardFault（评审 A5）
  MEM_MANAGE     = 5, ///< MemManage（MPU / 执行权限）
  BUS_FAULT      = 6, ///< BusFault（访问不存在的地址、ECC 错误等）
  USAGE_FAULT    = 7, ///< UsageFault（未定义指令、非对齐、除零等）
};

/** @brief 故障现场（布局固定，调试器直接读；按 8 字节对齐，见文件头 ECC 说明） */
struct alignas(8) FaultRecord
{
  static constexpr uint32_t MAGIC = 0x46415554U; ///< "FAUT"：magic 等于它才说明内容有效

  uint32_t  magic;    ///< 等于 MAGIC 才有效（掉电后是随机值）
  FaultKind kind;     ///< 最近一次故障的种类
  uint32_t  count;    ///< 上电以来累计故障次数（复位不清零）
  uint32_t  line;     ///< 断言所在行号（其他种类为 0）
  uint32_t  tick;     ///< 故障时的系统 tick（调度器未启动时为 0）
  uint32_t  ipsr;     ///< 故障时的 IPSR：0 = 任务上下文，非 0 = 异常号（在中断里断言）
  char      file[48]; ///< 断言所在源文件名（只留文件名，不含目录）
  char      task[16]; ///< 溢出的任务名 / 故障时正在运行的任务名（调度器未启动时为空）
  // 以下只对 HARD_FAULT ~ USAGE_FAULT 有意义（其他种类为 0）：故障状态寄存器与被打断现场
  uint32_t cfsr;       ///< SCB->CFSR（MemManage / Bus / Usage 三组状态位）
  uint32_t hfsr;       ///< SCB->HFSR（FORCED = 由可配置故障升级而来）
  uint32_t mmfar;      ///< SCB->MMFAR（CFSR.MMARVALID 时有效）
  uint32_t bfar;       ///< SCB->BFAR（CFSR.BFARVALID 时有效）
  uint32_t pc;         ///< 出错指令地址（被打断现场的 PC；0 = 没能解码，见 exc_return）
  uint32_t lr;         ///< 被打断现场的 LR
  uint32_t xpsr;       ///< 被打断现场的 xPSR
  uint32_t exc_return; ///< 进异常时的 EXC_RETURN：bit2 = 1 说明现场在 PSP（任务里出错），PC / LR 从 PSP 解码；= 0 在 MSP（中断里或调度器启动前），不解码
  uint32_t boot_rsr;   ///< 本次启动时的 RCC->RSR（复位原因；读完已清，评审 A8）
  uint32_t reserved;   ///< 保留（让 flush 落在 64 位边界）
  uint32_t  flush[2]; ///< 独占一个 64 位字：停机前写它一次，把前面还在 ECC 缓冲里的写入挤到 SRAM（内容无意义）
};

/** @brief 最近一次故障现场（.noinit，复位后保留） */
extern FaultRecord fault_record;

#ifdef __cplusplus
extern "C"
{
#endif

  /** @brief configASSERT 失败：记下文件名与行号后停机（不返回） */
  void fault_on_assert(const char *file, int line);

  /** @brief 栈溢出钩子：记下溢出的任务名后停机（不返回） */
  void fault_on_stack_overflow(const char *task_name);

  /** @brief 内存分配失败钩子：记下当前任务名后停机（不返回） */
  void fault_on_malloc_failed(void);
  /**
   * @brief 故障异常：记下故障状态寄存器与被打断现场后停机（不返回，评审 A5）
   * @param kind       FaultKind 的值（4 = HardFault、5 = MemManage、6 = BusFault、7 = UsageFault）
   * @param exc_return 进异常时的 LR（EXC_RETURN），在处理函数里用 __builtin_return_address(0) 取
   * @note 在 stm32h7xx_it.c 对应处理函数的 USER CODE 段里调用，片段见 templates/stm32h7xx_it_user_code.c。
   */
  void fault_on_exception(uint32_t kind, uint32_t exc_return);

#ifdef __cplusplus
}
#endif

/**
 * @brief 求值并检查：表达式为假时记下文件名 / 行号后停机（与 configASSERT 失败同一个入口）
 * @note 和 configASSERT 的区别：表达式**总会求值**。configASSERT 可能被定义成空（FreeRTOS 默认、或者发布构建关掉断言），
 *       写在它里面的 init() / xTaskCreate() 会被整个删掉，系统静默什么都不做（评审 A1）。有副作用的检查一律用它：
 *         ICBSP_CHECK(bsp_can1.init() == Status::OK);
 *       纯粹的条件检查（没有副作用）照旧用 configASSERT。
 */
#define ICBSP_CHECK(expr)                  \
  do                                       \
  {                                        \
    if (!(expr))                           \
    {                                      \
      fault_on_assert(__FILE__, __LINE__); \
    }                                      \
  } while (0)

#endif // __SERVICE_FAULT_RECORD_HPP__
