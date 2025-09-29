// swd_mem.h - Memory Access Interface
#ifndef SWD_MEM_H
#define SWD_MEM_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Memory AP CSW register settings
#define CSW_SIZE_8BIT   0x00000000
#define CSW_SIZE_16BIT  0x00000001
#define CSW_SIZE_32BIT  0x00000002
#define CSW_ADDRINC_OFF 0x00000000
#define CSW_ADDRINC_ON  0x00000010
#define CSW_DEVICE_EN   0x00000040
#define CSW_TRIN_PROG   0x00000080
#define CSW_SPIDEN      0x00800000
#define CSW_MASTER_DBG  0x20000000
#define CSW_HPROT       0x02000000
#define CSW_PROT        0x00000007

// Default CSW for 32-bit auto-increment access
#define CSW_DEFAULT (CSW_SIZE_32BIT | CSW_ADDRINC_ON | CSW_DEVICE_EN | \
                    CSW_MASTER_DBG | CSW_HPROT | 0x00000002)

// Initialize memory access (must call after swd_connect)
esp_err_t swd_mem_init(void);

// Single word access
esp_err_t swd_mem_read32(uint32_t addr, uint32_t *data);
esp_err_t swd_mem_write32(uint32_t addr, uint32_t data);

// Multiple word access (auto-increment)
esp_err_t swd_mem_read_buffer(uint32_t addr, uint8_t *buffer, uint32_t size);

// Block write for optimized flash programming
esp_err_t swd_mem_write_block32(uint32_t addr, const uint32_t *data, uint32_t count);

// System control registers
#define DHCSR_ADDR      0xE000EDF0  // Debug Halting Control and Status
#define DCRSR_ADDR      0xE000EDF4  // Debug Core Register Selector
#define DCRDR_ADDR      0xE000EDF8  // Debug Core Register Data
#define DEMCR_ADDR      0xE000EDFC  // Debug Exception and Monitor Control

// DHCSR bits
#define DHCSR_DBGKEY    (0xA05F << 16)
#define DHCSR_S_RESET   (1 << 25)
#define DHCSR_S_RETIRE  (1 << 24)
#define DHCSR_S_LOCKUP  (1 << 19)
#define DHCSR_S_SLEEP   (1 << 18)
#define DHCSR_S_HALT    (1 << 17)
#define DHCSR_S_REGRDY  (1 << 16)
#define DHCSR_C_SNAPSTALL (1 << 5)
#define DHCSR_C_MASKINTS  (1 << 3)
#define DHCSR_C_STEP    (1 << 2)
#define DHCSR_C_HALT    (1 << 1)
#define DHCSR_C_DEBUGEN (1 << 0)

#endif // SWD_MEM_H