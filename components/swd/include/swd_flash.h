// swd_flash.h - nRF52 Flash Programming Interface
#ifndef SWD_FLASH_H
#define SWD_FLASH_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// nRF52840 Flash parameters
#define NRF52_FLASH_BASE     0x00000000U
#define NRF52_FLASH_SIZE     (1024U * 1024U)  // 1MB
#define NRF52_FLASH_PAGE_SIZE 4096U            // 4KB pages
#define NRF52_FLASH_WORD_SIZE 4U

#define CTRL_AP_RESET           0x00
#define CTRL_AP_ERASEALL        0x04
#define CTRL_AP_ERASEALLSTATUS  0x08
#define CTRL_AP_APPROTECTSTATUS 0x0C
#define CTRL_AP_IDR             0xFC

// Nordic CTRL-AP IDR value from pyOCD
#define NORDIC_CTRL_AP_IDR      0x12880000  // From pyOCD: AP_IDR_VALUE = 0x12880000

// NVMC Registers (Non-Volatile Memory Controller)
#define NVMC_BASE           0x4001E000U
#define NVMC_READY         (NVMC_BASE + 0x400U)
#define NVMC_READYNEXT     (NVMC_BASE + 0x408U)
#define NVMC_CONFIG        (NVMC_BASE + 0x504U)
#define NVMC_ERASEPAGE     (NVMC_BASE + 0x508U)
#define NVMC_ERASEALL      (NVMC_BASE + 0x50CU)
#define NVMC_ERASEUICR     (NVMC_BASE + 0x514U)
#define NVMC_ICACHECNF     (NVMC_BASE + 0x540U)

// NVMC CONFIG values
#define NVMC_CONFIG_REN    0x00  // Read-only
#define NVMC_CONFIG_WEN    0x01  // Write enable
#define NVMC_CONFIG_EEN    0x02  // Erase enable

// Initialize flash programming (must call swd_init first)
esp_err_t swd_flash_init(void);

// Page operations
esp_err_t swd_flash_erase_page(uint32_t addr);

// Write operations
esp_err_t swd_flash_write_buffer(uint32_t addr, const uint8_t *data, uint32_t size);

// Mass erase and protection management
esp_err_t swd_flash_disable_approtect(void);

// Reset and run
esp_err_t swd_flash_reset_and_run(void);

#endif // SWD_FLASH_H