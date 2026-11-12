/**
 * @file main.c
 * @brief icbsp bootloader：外置 QSPI Flash（W25Q 系列）引导程序，支持方案 A（拷进 AXI SRAM）和方案 B（XIP）
 *
 * 流程：
 *   1. HSI 64 MHz 运行（不开 PLL：应用的 SystemClock_Config() 可以照常从复位状态配置时钟）
 *   2. 配 QSPI 引脚和 QUADSPI；W25Q 软复位、核对 JEDEC ID、确保四线使能位 QE、>16 MB 进 4 字节地址
 *   3. 内存映射（0xEB 四线读）；MPU 第 13 / 14 / 15 区：256 MB 窗口禁止访问、芯片容量只读可执行、
 *      芯片最后 4 KB 禁止访问（实测：内存映射读芯片最后一个字时 QUADSPI 预取越过芯片末尾，总线永久挂死，
 *      连调试器都停不下内核；有了第 15 区，误读只会进 MemManage）
 *   4. 校验 0x90000000 的镜像头和载荷 CRC（CRC 外设，zlib 算法）
 *   5. 方案 A：载荷拷进 AXI SRAM，退出内存映射、复位 QUADSPI、清 MPU 区，跳转
 *      方案 B：保持内存映射和 MPU 区（应用的 MPU_Config() 只改它自己的区号，第 13~15 区保留），跳转
 *   失败时把原因写进 ICBSP_BOOT_STATUS_ADDR 处的状态字，然后原地循环（调试器可以直接连上读）。
 *
 * QUADSPI 预分频按应用的最终 HCLK3 选（icbsp_flash_config.h 的 ICBSP_QSPI_PRESCALER）：
 * 方案 B 里应用把系统时钟拉到 480 MHz 后，QSPI 时钟 = 240 MHz / (预分频 + 1)，不能超过芯片上限。
 */
#include "icbsp_boot.h"

#include ICBSP_BOOT_CONFIG_HEADER

#include "stm32h7xx.h"

#include <stdbool.h>
#include <stddef.h>

#ifndef ICBSP_QSPI_FLASH_SIZE
#error "icbsp_flash_config.h 缺少 ICBSP_QSPI_FLASH_SIZE"
#endif
#ifndef ICBSP_QSPI_PINS
#error "icbsp_flash_config.h 缺少 ICBSP_QSPI_PINS"
#endif
#ifndef ICBSP_BOOT_STATUS_ADDR
#define ICBSP_BOOT_STATUS_ADDR 0x3800FFE0U
#endif
#ifndef ICBSP_QSPI_CS_HIGH_CYCLES
#define ICBSP_QSPI_CS_HIGH_CYCLES 6
#endif
#ifndef ICBSP_QSPI_SAMPLE_SHIFT
#define ICBSP_QSPI_SAMPLE_SHIFT 1
#endif

#define AXI_SRAM_BASE 0x24000000U
#define DTCM_BASE 0x20000000U
#define DTCM_SIZE 0x00020000U
#define MPU_REGION_GUARD 13U ///< 256 MB 窗口：禁止访问
#define MPU_REGION_FLASH 14U ///< 芯片容量：只读、可执行、写通 cache
#define MPU_REGION_TAIL 15U  ///< 芯片最后 4 KB：禁止访问（防 QUADSPI 预取越过芯片末尾挂死总线）
#define QSPI_TAIL_GUARD 0x1000U

_Static_assert((ICBSP_QSPI_FLASH_SIZE & (ICBSP_QSPI_FLASH_SIZE - 1U)) == 0U, "QSPI 容量必须是 2 的幂");
_Static_assert(ICBSP_QSPI_FLASH_SIZE >= 0x100000U && ICBSP_QSPI_FLASH_SIZE <= 0x10000000U, "QSPI 容量范围 1 MB~256 MB");
_Static_assert(ICBSP_QSPI_ADDR_BYTES == 3 || ICBSP_QSPI_ADDR_BYTES == 4, "地址字节数只能是 3 或 4");
_Static_assert(ICBSP_QSPI_ADDR_BYTES == 4 || ICBSP_QSPI_FLASH_SIZE <= 0x1000000U, ">16 MB 的芯片要用 4 字节地址");
_Static_assert(ICBSP_QSPI_PRESCALER >= 0 && ICBSP_QSPI_PRESCALER <= 255, "预分频 0~255");
_Static_assert(ICBSP_QSPI_CS_HIGH_CYCLES >= 1 && ICBSP_QSPI_CS_HIGH_CYCLES <= 8, "片选高电平 1~8 个周期");

static volatile icbsp_boot_status_t *const status = (volatile icbsp_boot_status_t *)ICBSP_BOOT_STATUS_ADDR;

static void set_state(uint32_t state, uint32_t detail)
{
  status->state = state;
  status->detail = detail;
  __DSB();
}

/* ============================== QUADSPI ============================== */

#define CCR_LINES_NONE 0U
#define CCR_LINES_1 1U
#define CCR_LINES_4 3U
#define FMODE_WRITE 0U
#define FMODE_READ 1U
#define FMODE_MMAP 3U
#define TIMEOUT_LOOPS 4000000U ///< 约 0.2 s（64 MHz）；W25Q 写状态寄存器最长 15 ms

static uint32_t ccr(uint8_t inst, uint32_t imode, uint32_t admode, uint32_t adsize, uint32_t dmode, uint32_t fmode,
                    uint32_t dummy)
{
  return ((uint32_t)inst << QUADSPI_CCR_INSTRUCTION_Pos) | (imode << QUADSPI_CCR_IMODE_Pos) |
         (admode << QUADSPI_CCR_ADMODE_Pos) | (adsize << QUADSPI_CCR_ADSIZE_Pos) | (dummy << QUADSPI_CCR_DCYC_Pos) |
         (dmode << QUADSPI_CCR_DMODE_Pos) | (fmode << QUADSPI_CCR_FMODE_Pos);
}

static bool wait_set(uint32_t flag)
{
  for (uint32_t n = 0; n < TIMEOUT_LOOPS; ++n)
  {
    if ((QUADSPI->SR & flag) != 0U)
    {
      return true;
    }
  }
  return false;
}

static bool wait_idle(void)
{
  for (uint32_t n = 0; n < TIMEOUT_LOOPS; ++n)
  {
    if ((QUADSPI->SR & QUADSPI_SR_BUSY) == 0U)
    {
      return true;
    }
  }
  return false;
}

/** 单线指令 + 可选单线数据（间接读 / 写）；len = 0 表示只有指令 */
static bool cmd(uint8_t inst, uint8_t *rx, const uint8_t *tx, uint32_t len)
{
  if (!wait_idle())
  {
    return false;
  }
  QUADSPI->FCR = QUADSPI_FCR_CTEF | QUADSPI_FCR_CTCF | QUADSPI_FCR_CSMF | QUADSPI_FCR_CTOF;
  if (len > 0U)
  {
    QUADSPI->DLR = len - 1U;
  }
  const uint32_t dmode = (len > 0U) ? CCR_LINES_1 : CCR_LINES_NONE;
  QUADSPI->CCR = ccr(inst, CCR_LINES_1, CCR_LINES_NONE, 0U, dmode, (rx != NULL) ? FMODE_READ : FMODE_WRITE, 0U);
  for (uint32_t i = 0; i < len; ++i)
  {
    if (!wait_set(QUADSPI_SR_FTF | QUADSPI_SR_TCF))
    {
      return false;
    }
    if (rx != NULL)
    {
      rx[i] = *(volatile uint8_t *)&QUADSPI->DR;
    }
    else
    {
      *(volatile uint8_t *)&QUADSPI->DR = tx[i];
    }
  }
  if (!wait_set(QUADSPI_SR_TCF))
  {
    return false;
  }
  QUADSPI->FCR = QUADSPI_FCR_CTCF;
  return true;
}

static void delay_loops(uint32_t n)
{
  for (volatile uint32_t i = 0; i < n; ++i)
  {
  }
}

static void gpio_af(GPIO_TypeDef *port, uint32_t index, uint32_t pin, uint32_t af)
{
  RCC->AHB4ENR |= (1UL << index);
  (void)RCC->AHB4ENR;
  port->MODER = (port->MODER & ~(3UL << (pin * 2U))) | (2UL << (pin * 2U));
  port->OSPEEDR |= (3UL << (pin * 2U));
  port->OTYPER &= ~(1UL << pin);
  port->PUPDR &= ~(3UL << (pin * 2U));
  volatile uint32_t *afr = &port->AFR[pin >> 3U];
  *afr = (*afr & ~(0xFUL << ((pin & 7U) * 4U))) | (af << ((pin & 7U) * 4U));
}

#define ICBSP_PORT_INDEX_A 0U
#define ICBSP_PORT_INDEX_B 1U
#define ICBSP_PORT_INDEX_C 2U
#define ICBSP_PORT_INDEX_D 3U
#define ICBSP_PORT_INDEX_E 4U
#define ICBSP_PORT_INDEX_F 5U
#define ICBSP_PORT_INDEX_G 6U
#define ICBSP_PORT_INDEX_H 7U
#define ICBSP_PIN_INIT(port, pin, af) gpio_af(GPIO##port, ICBSP_PORT_INDEX_##port, (pin), (af));

static uint32_t log2u(uint32_t v)
{
  return 31U - (uint32_t)__builtin_clz(v);
}

static void qspi_init(void)
{
  ICBSP_QSPI_PINS(ICBSP_PIN_INIT)

  RCC->AHB3ENR |= RCC_AHB3ENR_QSPIEN;
  (void)RCC->AHB3ENR;
  RCC->AHB3RSTR |= RCC_AHB3RSTR_QSPIRST;
  RCC->AHB3RSTR &= ~RCC_AHB3RSTR_QSPIRST;

  uint32_t cr = ((uint32_t)ICBSP_QSPI_PRESCALER << QUADSPI_CR_PRESCALER_Pos) | (3UL << QUADSPI_CR_FTHRES_Pos);
#if ICBSP_QSPI_SAMPLE_SHIFT
  cr |= QUADSPI_CR_SSHIFT;
#endif
#if ICBSP_QSPI_BANK == 2
  cr |= QUADSPI_CR_FSEL;
#endif
  QUADSPI->CR = cr;
  QUADSPI->DCR = ((log2u(ICBSP_QSPI_FLASH_SIZE) - 1U) << QUADSPI_DCR_FSIZE_Pos) |
                 ((uint32_t)(ICBSP_QSPI_CS_HIGH_CYCLES - 1) << QUADSPI_DCR_CSHT_Pos);
  QUADSPI->CR |= QUADSPI_CR_EN;
}

static void qspi_shutdown(void)
{
  QUADSPI->CR |= QUADSPI_CR_ABORT;
  for (uint32_t n = 0; n < TIMEOUT_LOOPS && (QUADSPI->CR & QUADSPI_CR_ABORT) != 0U; ++n)
  {
  }
  QUADSPI->CR = 0U;
  RCC->AHB3RSTR |= RCC_AHB3RSTR_QSPIRST;
  RCC->AHB3RSTR &= ~RCC_AHB3RSTR_QSPIRST;
  RCC->AHB3ENR &= ~RCC_AHB3ENR_QSPIEN;
}

/* ============================== W25Q ============================== */

#define W25Q_ENABLE_RESET 0x66U
#define W25Q_RESET 0x99U
#define W25Q_JEDEC_ID 0x9FU
#define W25Q_READ_SR1 0x05U
#define W25Q_READ_SR2 0x35U
#define W25Q_WRITE_SR2 0x31U
#define W25Q_WRITE_ENABLE 0x06U
#define W25Q_VOLATILE_SR_WE 0x50U
#define W25Q_ENTER_4BYTE 0xB7U
#define W25Q_FAST_READ_QUAD_IO 0xEBU
#define W25Q_SR1_BUSY 0x01U
#define W25Q_SR2_QE 0x02U

static bool w25q_wait_ready(void)
{
  for (uint32_t n = 0; n < 100000U; ++n)
  {
    uint8_t sr1 = 0xFFU;
    if (!cmd(W25Q_READ_SR1, &sr1, NULL, 1U))
    {
      return false;
    }
    if ((sr1 & W25Q_SR1_BUSY) == 0U)
    {
      return true;
    }
  }
  return false;
}

/** 返回 icbsp_boot_state_t（STARTED 表示成功） */
static uint32_t w25q_prepare(void)
{
  if (!cmd(W25Q_ENABLE_RESET, NULL, NULL, 0U) || !cmd(W25Q_RESET, NULL, NULL, 0U))
  {
    return ICBSP_BOOT_QSPI_TIMEOUT;
  }
  delay_loops(5000U); // tRST 最长 30 µs

  uint8_t id[3] = {0U, 0U, 0U};
  if (!cmd(W25Q_JEDEC_ID, id, NULL, 3U))
  {
    return ICBSP_BOOT_QSPI_TIMEOUT;
  }
  const uint32_t jedec = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
  status->jedec = jedec;
  // 只核对厂商和容量（同一容量有 0x40 / 0x60 / 0x70 等不同类型码）
  if (id[0] != ICBSP_QSPI_JEDEC_MANUFACTURER || id[2] != ICBSP_QSPI_JEDEC_CAPACITY)
  {
    set_state(ICBSP_BOOT_JEDEC_MISMATCH, jedec);
    return ICBSP_BOOT_JEDEC_MISMATCH;
  }

  uint8_t sr2 = 0U;
  if (!cmd(W25Q_READ_SR2, &sr2, NULL, 1U))
  {
    return ICBSP_BOOT_QSPI_TIMEOUT;
  }
  if ((sr2 & W25Q_SR2_QE) == 0U)
  {
    // 写非易失 QE：只在第一次（新芯片）发生，之后上电都保持
    const uint8_t v = (uint8_t)(sr2 | W25Q_SR2_QE);
    if (!cmd(W25Q_WRITE_ENABLE, NULL, NULL, 0U) || !cmd(W25Q_WRITE_SR2, NULL, &v, 1U) || !w25q_wait_ready())
    {
      return ICBSP_BOOT_QSPI_TIMEOUT;
    }
    if (!cmd(W25Q_READ_SR2, &sr2, NULL, 1U))
    {
      return ICBSP_BOOT_QSPI_TIMEOUT;
    }
    if ((sr2 & W25Q_SR2_QE) == 0U)
    {
      set_state(ICBSP_BOOT_QE_FAIL, sr2);
      return ICBSP_BOOT_QE_FAIL;
    }
  }
#if ICBSP_QSPI_ADDR_BYTES == 4
  if (!cmd(W25Q_ENTER_4BYTE, NULL, NULL, 0U))
  {
    return ICBSP_BOOT_QSPI_TIMEOUT;
  }
#endif
  return ICBSP_BOOT_STARTED;
}

/** 0xEB 四线读：指令单线，地址 / 模式字节 / 数据四线，模式字节 0xFF（不进连续读模式）+ 4 个空周期 */
static bool w25q_memory_mapped(void)
{
  if (!wait_idle())
  {
    return false;
  }
  QUADSPI->ABR = 0xFFU;
  QUADSPI->CCR = ccr(W25Q_FAST_READ_QUAD_IO, CCR_LINES_1, CCR_LINES_4, ICBSP_QSPI_ADDR_BYTES - 1U, CCR_LINES_4,
                     FMODE_MMAP, 4U) |
                 (CCR_LINES_4 << QUADSPI_CCR_ABMODE_Pos) | (0U << QUADSPI_CCR_ABSIZE_Pos);
  return true;
}

/* ============================== MPU / CRC ============================== */

static void mpu_region(uint32_t number, uint32_t base, uint32_t rasr)
{
  MPU->RNR = number;
  MPU->RBAR = base;
  MPU->RASR = rasr;
}

static void mpu_setup(void)
{
  MPU->CTRL = 0U;
  __DSB();
  __ISB();
  // 第 14 区：256 MB 窗口禁止访问（防 M7 推测访问到芯片以外的地址把总线卡死）
  mpu_region(MPU_REGION_GUARD, ICBSP_QSPI_BASE,
             MPU_RASR_XN_Msk | (0U << MPU_RASR_AP_Pos) | ((28U - 1U) << MPU_RASR_SIZE_Pos) | MPU_RASR_ENABLE_Msk);
  // 第 14 区：芯片容量，只读（AP=110）、可执行、Normal 写通（TEX=0 C=1 B=0）
  mpu_region(MPU_REGION_FLASH, ICBSP_QSPI_BASE,
             (6U << MPU_RASR_AP_Pos) | MPU_RASR_C_Msk | ((log2u(ICBSP_QSPI_FLASH_SIZE) - 1U) << MPU_RASR_SIZE_Pos) |
                 MPU_RASR_ENABLE_Msk);
  // 第 15 区：芯片最后 4 KB 禁止访问（区号最大，优先级最高）
  mpu_region(MPU_REGION_TAIL, ICBSP_QSPI_BASE + ICBSP_QSPI_FLASH_SIZE - QSPI_TAIL_GUARD,
             MPU_RASR_XN_Msk | (0U << MPU_RASR_AP_Pos) | ((log2u(QSPI_TAIL_GUARD) - 1U) << MPU_RASR_SIZE_Pos) |
                 MPU_RASR_ENABLE_Msk);
  MPU->CTRL = MPU_CTRL_PRIVDEFENA_Msk | MPU_CTRL_ENABLE_Msk;
  __DSB();
  __ISB();
}

static void mpu_clear(void)
{
  MPU->CTRL = 0U;
  mpu_region(MPU_REGION_GUARD, 0U, 0U);
  mpu_region(MPU_REGION_FLASH, 0U, 0U);
  mpu_region(MPU_REGION_TAIL, 0U, 0U);
  __DSB();
  __ISB();
}

/** CRC-32（zlib：多项式 0x04C11DB7，输入按字反转、输出反转，初值与结果异或 0xFFFFFFFF）；len 为 4 的倍数 */
static uint32_t crc32_words(const volatile uint32_t *p, uint32_t len)
{
  CRC->POL = 0x04C11DB7U;
  CRC->INIT = 0xFFFFFFFFU;
  CRC->CR = CRC_CR_REV_OUT | (3UL << CRC_CR_REV_IN_Pos) | CRC_CR_RESET;
  for (uint32_t i = 0; i < len / 4U; ++i)
  {
    CRC->DR = p[i];
  }
  return CRC->DR ^ 0xFFFFFFFFU;
}

/* ============================== 跳转 ============================== */

__attribute__((noreturn)) static void jump(uint32_t vtor)
{
  const uint32_t sp = ((const volatile uint32_t *)vtor)[0];
  const uint32_t pc = ((const volatile uint32_t *)vtor)[1];

  __disable_irq();
  SysTick->CTRL = 0U;
  SysTick->LOAD = 0U;
  SysTick->VAL = 0U;
  for (uint32_t i = 0; i < 8U; ++i)
  {
    NVIC->ICER[i] = 0xFFFFFFFFU;
    NVIC->ICPR[i] = 0xFFFFFFFFU;
  }
  SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
  SCB_DisableICache(); // 应用 main() 里 SCB_EnableICache() 会重新作废并打开
  SCB->VTOR = vtor;
  __DSB();
  __ISB();
  __enable_irq();
  __asm volatile("msr msp, %0\n"
                 "bx  %1\n" ::"r"(sp),
                 "r"(pc)
                 : "memory");
  __builtin_unreachable();
}

static bool vector_ok(uint32_t vtor, uint32_t size)
{
  const uint32_t sp = ((const volatile uint32_t *)vtor)[0];
  const uint32_t pc = ((const volatile uint32_t *)vtor)[1];
  const bool sp_ok = (sp > DTCM_BASE && sp <= DTCM_BASE + DTCM_SIZE) ||
                     (sp > AXI_SRAM_BASE && sp <= AXI_SRAM_BASE + ICBSP_AXI_SRAM_SIZE);
  const bool pc_ok = (pc & 1U) != 0U && (pc & ~1U) >= vtor && (pc & ~1U) < vtor + size;
  return sp_ok && pc_ok;
}

static __attribute__((noreturn)) void fail(uint32_t state, uint32_t detail)
{
  if (state != ICBSP_BOOT_STARTED)
  {
    set_state(state, detail);
  }
  for (;;)
  {
  }
}

int main(void)
{
  status->magic = ICBSP_BOOT_STATUS_MAGIC;
  status->version = ICBSP_BOOT_VERSION;
  status->jedec = 0U;
  status->layout = 0U;
  status->size = 0U;
  set_state(ICBSP_BOOT_STARTED, 0U);

  SCB_EnableICache();
  RCC->AHB4ENR |= RCC_AHB4ENR_CRCEN;
  (void)RCC->AHB4ENR;

  qspi_init();
  const uint32_t prep = w25q_prepare();
  if (prep != ICBSP_BOOT_STARTED)
  {
    fail(prep, status->detail);
  }
  if (!w25q_memory_mapped())
  {
    fail(ICBSP_BOOT_QSPI_TIMEOUT, 1U);
  }
  status->qspi_cr = QUADSPI->CR;
  mpu_setup();

  const volatile icbsp_image_header_t *h = (const volatile icbsp_image_header_t *)ICBSP_QSPI_BASE;
  if (h->magic != ICBSP_IMAGE_MAGIC || crc32_words((const volatile uint32_t *)h, 28U) != h->header_crc)
  {
    fail(ICBSP_BOOT_NO_IMAGE, h->magic);
  }
  const uint32_t layout = h->layout;
  const uint32_t size = h->size;
  const uint32_t load = h->load_addr;
  status->layout = layout;
  status->size = size;
  if (h->version != ICBSP_IMAGE_VERSION)
  {
    fail(ICBSP_BOOT_BAD_HEADER, 1U);
  }
  if (size == 0U || (size & 3U) != 0U || size > ICBSP_QSPI_FLASH_SIZE - ICBSP_IMAGE_HEADER_SIZE - QSPI_TAIL_GUARD)
  {
    fail(ICBSP_BOOT_BAD_HEADER, 5U);
  }
  if (h->vector_addr != load)
  {
    fail(ICBSP_BOOT_BAD_HEADER, 4U);
  }
  if (layout == ICBSP_IMAGE_LAYOUT_RAM)
  {
    if (load != AXI_SRAM_BASE || size > ICBSP_AXI_SRAM_SIZE)
    {
      fail(ICBSP_BOOT_BAD_HEADER, 3U);
    }
  }
  else if (layout == ICBSP_IMAGE_LAYOUT_XIP)
  {
    if (load != ICBSP_QSPI_BASE + ICBSP_IMAGE_HEADER_SIZE)
    {
      fail(ICBSP_BOOT_BAD_HEADER, 3U);
    }
  }
  else
  {
    fail(ICBSP_BOOT_BAD_HEADER, 2U);
  }

  const volatile uint32_t *payload = (const volatile uint32_t *)(ICBSP_QSPI_BASE + ICBSP_IMAGE_HEADER_SIZE);
  const uint32_t crc = crc32_words(payload, size);
  if (crc != h->payload_crc)
  {
    fail(ICBSP_BOOT_BAD_CRC, crc);
  }

  if (layout == ICBSP_IMAGE_LAYOUT_RAM)
  {
    volatile uint32_t *dst = (volatile uint32_t *)AXI_SRAM_BASE;
    for (uint32_t i = 0; i < size / 4U; ++i)
    {
      dst[i] = payload[i];
    }
    if (!vector_ok(load, size))
    {
      fail(ICBSP_BOOT_BAD_VECTOR, ((const volatile uint32_t *)load)[1]);
    }
    // 交还复位状态：应用会自己初始化 QUADSPI（MX_QUADSPI_Init）和 MPU
    mpu_clear();
    qspi_shutdown();
    RCC->AHB4RSTR |= RCC_AHB4RSTR_CRCRST;
    RCC->AHB4RSTR &= ~RCC_AHB4RSTR_CRCRST;
    RCC->AHB4ENR &= ~RCC_AHB4ENR_CRCEN;
    set_state(ICBSP_BOOT_JUMP_RAM, load);
  }
  else
  {
    if (!vector_ok(load, size))
    {
      fail(ICBSP_BOOT_BAD_VECTOR, ((const volatile uint32_t *)load)[1]);
    }
    RCC->AHB4RSTR |= RCC_AHB4RSTR_CRCRST;
    RCC->AHB4RSTR &= ~RCC_AHB4RSTR_CRCRST;
    RCC->AHB4ENR &= ~RCC_AHB4ENR_CRCEN;
    set_state(ICBSP_BOOT_JUMP_XIP, load);
  }
  jump(load);
}
