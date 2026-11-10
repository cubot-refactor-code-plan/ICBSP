/* icbsp bootloader：片内 Flash 0x08000000 运行，数据和栈放 DTCM（不碰 AXI SRAM 的 .noinit，也不碰 RAM_D3 的状态字以外区域） */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH   (rx)  : ORIGIN = 0x08000000, LENGTH = 128K
  DTCMRAM (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
}

_estack = ORIGIN(DTCMRAM) + LENGTH(DTCMRAM);

SECTIONS
{
  .isr_vector :
  {
    KEEP(*(.isr_vector))
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text .text*)
    *(.rodata .rodata*)
    . = ALIGN(4);
  } >FLASH

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data .data*)
    . = ALIGN(4);
    _edata = .;
  } >DTCMRAM AT> FLASH
  _sidata = LOADADDR(.data);

  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    _sbss = .;
    *(.bss .bss* COMMON)
    . = ALIGN(4);
    _ebss = .;
  } >DTCMRAM

  /DISCARD/ :
  {
    *(.ARM.exidx* .ARM.extab*)
  }
}
