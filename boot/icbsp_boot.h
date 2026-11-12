/**
 * @file icbsp_boot.h
 * @brief icbsp bootloader 与宿主 / 工具共用的格式：QSPI 镜像头、启动状态字
 *
 * 镜像头的生成见 tools/qspi_image.py；两边改动必须同步。
 */
#ifndef ICBSP_BOOT_H
#define ICBSP_BOOT_H

#include <stdint.h>

#define ICBSP_IMAGE_MAGIC 0x49424349U ///< 'ICBI'
#define ICBSP_IMAGE_VERSION 1U
#define ICBSP_IMAGE_LAYOUT_RAM 1U ///< 方案 A：拷进 AXI SRAM 运行
#define ICBSP_IMAGE_LAYOUT_XIP 2U ///< 方案 B：QSPI 上就地执行
#define ICBSP_IMAGE_HEADER_SIZE 0x400U
#define ICBSP_QSPI_BASE 0x90000000U

/** QSPI 0x90000000 处的镜像头（32 字节，后面补 0xFF 到 1 KB） */
typedef struct
{
  uint32_t magic;
  uint32_t version;
  uint32_t layout;
  uint32_t load_addr;
  uint32_t vector_addr;
  uint32_t size;        ///< 载荷字节数（4 字节对齐），载荷从 0x90000400 开始
  uint32_t payload_crc; ///< CRC-32（zlib）
  uint32_t header_crc;  ///< 前 28 字节的 CRC-32
} icbsp_image_header_t;

/** 启动状态（bootloader 每次启动都整块重写；调试器读 ICBSP_BOOT_STATUS_ADDR） */
typedef enum
{
  ICBSP_BOOT_STARTED = 1,
  ICBSP_BOOT_QSPI_TIMEOUT = 2,   ///< QUADSPI 命令超时（引脚 / 时钟 / 芯片没响应）
  ICBSP_BOOT_JEDEC_MISMATCH = 3, ///< detail = 读到的 JEDEC ID
  ICBSP_BOOT_QE_FAIL = 4,        ///< 四线使能位写不进去
  ICBSP_BOOT_NO_IMAGE = 5,       ///< 镜像头 magic / 头 CRC 不对（没烧镜像或被擦了）
  ICBSP_BOOT_BAD_HEADER = 6,     ///< 头字段越界（detail = 出错的字段序号）
  ICBSP_BOOT_BAD_CRC = 7,        ///< 载荷 CRC 不对（detail = 算出来的 CRC）
  ICBSP_BOOT_BAD_VECTOR = 8,     ///< 向量表里的栈顶 / 入口不合理
  ICBSP_BOOT_JUMP_RAM = 0x10,    ///< 已跳转到方案 A 程序
  ICBSP_BOOT_JUMP_XIP = 0x11,    ///< 已跳转到方案 B 程序
} icbsp_boot_state_t;

#define ICBSP_BOOT_STATUS_MAGIC 0x544F4F42U ///< 'BOOT'
#define ICBSP_BOOT_VERSION 1U

typedef struct
{
  uint32_t magic;
  uint32_t version;
  uint32_t state;  ///< icbsp_boot_state_t
  uint32_t detail; ///< 见各状态说明
  uint32_t jedec;  ///< 读到的 JEDEC ID（0xMMTTCC）
  uint32_t layout; ///< 镜像头 layout
  uint32_t size;   ///< 镜像头 size
  uint32_t qspi_cr;
} icbsp_boot_status_t;

#endif
