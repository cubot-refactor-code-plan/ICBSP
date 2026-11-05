/*
 * 片段（不要编译这个文件）：分别复制到 CubeMX 生成的 Core/Src/stm32h7xx_it.c 对应的 USER CODE 段（评审 A5）。
 *
 * 作用：HardFault / MemManage / BusFault / UsageFault 进来时，把故障状态寄存器（CFSR / HFSR / MMFAR / BFAR）、
 * 出错指令地址（任务里出错时从 PSP 解出 PC / LR / xPSR）、任务名、tick 记进 fault_record（.noinit，复位后保留），
 * 再停机：接着调试器时停在 BKPT，没接就死循环。复位后用调试器 `print/x fault_record` 查看，
 * 用 `arm-none-eabi-addr2line -e <elf> <pc>` 找到出错的源码行。实现在库的 service/fault_record.cpp。
 *
 * stm32h7xx_it.c 是 C 文件、看不到库的 C++ 头文件，所以原型直接写在 USER CODE 0 里。
 * MemManage / BusFault / UsageFault 默认没有打开（SCB->SHCSR），这时它们都升级成 HardFault（HFSR.FORCED = 1，CFSR 里仍有原因）。
 */

/* USER CODE BEGIN 0 */
void fault_on_exception(uint32_t kind, uint32_t exc_return); /* 库：service/fault_record.cpp，不返回 */
/* USER CODE END 0 */

/* HardFault_Handler() 里： */
  /* USER CODE BEGIN HardFault_IRQn 0 */
  fault_on_exception(4U, (uint32_t)__builtin_return_address(0)); /* 4 = HARD_FAULT；返回地址就是 EXC_RETURN */
  /* USER CODE END HardFault_IRQn 0 */

/* MemManage_Handler() 里： */
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */
  fault_on_exception(5U, (uint32_t)__builtin_return_address(0)); /* 5 = MEM_MANAGE */
  /* USER CODE END MemoryManagement_IRQn 0 */

/* BusFault_Handler() 里： */
  /* USER CODE BEGIN BusFault_IRQn 0 */
  fault_on_exception(6U, (uint32_t)__builtin_return_address(0)); /* 6 = BUS_FAULT */
  /* USER CODE END BusFault_IRQn 0 */

/* UsageFault_Handler() 里： */
  /* USER CODE BEGIN UsageFault_IRQn 0 */
  fault_on_exception(7U, (uint32_t)__builtin_return_address(0)); /* 7 = USAGE_FAULT */
  /* USER CODE END UsageFault_IRQn 0 */
