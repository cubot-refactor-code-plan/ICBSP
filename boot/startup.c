/**
 * @file startup.c
 * @brief icbsp bootloader 的启动代码：只有内核异常向量，不开任何外设中断
 */
#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern int main(void);

void Reset_Handler(void);
void Default_Handler(void);

void Reset_Handler(void)
{
  // 启用 FPU（工具链是 -mfloat-abi=hard，编译器可能生成浮点寄存器访问）
  *(volatile uint32_t *)0xE000ED88U |= (0xFU << 20);
  __asm volatile("dsb\n isb" ::: "memory");

  for (uint32_t *src = &_sidata, *dst = &_sdata; dst < &_edata;)
  {
    *dst++ = *src++;
  }
  for (uint32_t *dst = &_sbss; dst < &_ebss;)
  {
    *dst++ = 0U;
  }
  (void)main();
  for (;;)
  {
  }
}

/** 异常：停在这里，用调试器看 ICBSP_BOOT_STATUS_ADDR 处的状态字和 SCB->CFSR */
void Default_Handler(void)
{
  for (;;)
  {
  }
}

__attribute__((section(".isr_vector"), used)) static void (*const vectors[16])(void) = {
    (void (*)(void))(&_estack),
    Reset_Handler,
    Default_Handler, // NMI
    Default_Handler, // HardFault
    Default_Handler, // MemManage
    Default_Handler, // BusFault
    Default_Handler, // UsageFault
    0,
    0,
    0,
    0,
    Default_Handler, // SVC
    Default_Handler, // DebugMon
    0,
    Default_Handler, // PendSV
    Default_Handler, // SysTick
};
