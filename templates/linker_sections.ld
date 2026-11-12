/*
 * icbsp 需要的链接段：加进宿主工程的链接脚本（CubeMX 生成的 STM32H7xxxx_FLASH.ld 或
 * STM32F4xxxx_FLASH.ld），放在 SECTIONS { ... } 里、.fini_array 之后、.data 之前即可。
 *
 * CubeMX 新建工程的链接脚本没有这些段。icbsp 的 CMake 在配置阶段检查，缺了直接报错
 * （链接脚本一变就会重新检查）。CubeMX 6.18.1 重新生成时保留已有链接脚本的内容（实测只改了几行行尾），
 * 其他版本未验证，重新生成后留意配置阶段的检查结果即可。
 *
 * 按系列各取所需：
 *   - H7：两段都要（.dma_buffer + .noinit）
 *   - F4：**只要 .noinit**。主 SRAM 本身就能被 DMA 访问，ICBSP_DMA_BUFFER 在 F4 上展开为空，
 *         不会生成 .dma_buffer；CMake 在 F4 上也不检查这一段。
 *
 * - .dma_buffer（H7）：BspUart / BspSpi 实例（内含 DMA 收发缓冲）。H7 的 DMA1 / DMA2 访问不到 DTCM，
 *   所以实例要加 __attribute__((section(".dma_buffer.<变量名>"))) 并链接到 RAM_D1（AXI
 *   SRAM，或 RAM_D2 / RAM_D3）；段名按变量唯一，理由见 templates/user/board/board_cfg.hpp。
 *   这些实例是 constexpr 常量初始化的，**必须可加载**（AT> FLASH），由框架在全局构造前
 *   把初值拷进 RAM_D1；写成 NOLOAD 会让句柄 / 波特率等配置上电后变成随机值。
 *   BspUartT::init() 发现实例在 DTCM / ITCM 会返回 BAD_ARG。
 *   假定 D-Cache 关闭；开 D-Cache 时这块要用 MPU 设成非缓存。
 * - .noinit（H7 / F4 都要）：fault_record（断言 / 栈溢出 / 分配失败现场）。NOLOAD 且不在 .bss 范围内，
 *   启动代码不清零，复位后仍保留上一次的内容（掉电丢失）。
 *
 * 内存区名以宿主链接脚本 MEMORY { } 里的为准：
 *   - H7：AXI SRAM 都从 0x24000000 开始，但 CubeMX 的叫法因型号而异（H723 是 RAM_D1，H743 / H750 是 RAM）。
 *   - F4：主 SRAM 从 0x20000000 开始，CubeMX 里叫 RAM；0x10000000 的 CCM RAM 不能 DMA，别往那儿放。
 * 下面的 >RAM_D1 只是占位：tools/integrate.py 按 ORIGIN（H7 0x24000000，F4 0x20000000）找到这个区
 * 并替掉区名（--ram-region 可手动指定，但拒绝 ITCM / DTCM / CCM）；手动接入时自己改。
 */

  .dma_buffer :
  {
    . = ALIGN(32);
    __dma_buffer_start = .;
    *(.dma_buffer)
    *(.dma_buffer*)
    . = ALIGN(32);
    __dma_buffer_end = .;
  } >RAM_D1 AT> FLASH
  __dma_buffer_load = LOADADDR(.dma_buffer) + (__dma_buffer_start - ADDR(.dma_buffer)); /* 段内 ALIGN(32) 会让起点后移，加载地址同样偏移 */

  .noinit (NOLOAD) :
  {
    . = ALIGN(4);
    *(.noinit)
    *(.noinit*)
    . = ALIGN(4);
  } >RAM_D1
