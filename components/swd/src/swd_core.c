// swd_core.c - Complete SWD Protocol Implementation
#include "swd_core.h"
#include "swd_mem.h"
#include "nrf52_hal.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "soc/gpio_struct.h"
#ifdef CONFIG_IDF_TARGET_ESP32C3
#include "soc/gpio_reg.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SWD_CORE";

// GPIO fast access macros for ESP32C3
#define SWCLK_MASK (1UL << config.pin_swclk)
#define SWDIO_MASK (1UL << config.pin_swdio)

#define SWCLK_H()    REG_WRITE(GPIO_OUT_W1TS_REG, SWCLK_MASK)
#define SWCLK_L()    REG_WRITE(GPIO_OUT_W1TC_REG, SWCLK_MASK)
#define SWDIO_H()    REG_WRITE(GPIO_OUT_W1TS_REG, SWDIO_MASK)
#define SWDIO_L()    REG_WRITE(GPIO_OUT_W1TC_REG, SWDIO_MASK)
#define SWDIO_DRIVE()   REG_WRITE(GPIO_ENABLE_W1TS_REG, SWDIO_MASK)
#define SWDIO_RELEASE() REG_WRITE(GPIO_ENABLE_W1TC_REG, SWDIO_MASK)
#define READ_SWDIO()    ((REG_READ(GPIO_IN_REG) & SWDIO_MASK) != 0)

// Configuration storage
static swd_config_t config = {0};
static bool initialized = false;
static bool connected = false;
static bool drive_phase = true;
static portMUX_TYPE swd_mutex = portMUX_INITIALIZER_UNLOCKED;

// Timing delay
static inline void swd_delay(void) {
    for (int i = 0; i < config.delay_cycles; i++) {
        __asm__ __volatile__("nop");
    }
}

// Clock pulse
static inline void clock_pulse(void) {
    SWCLK_H();
    swd_delay();
    SWCLK_L();
    swd_delay();
}

// Calculate parity
static inline bool parity32(uint32_t x) {
    x ^= x >> 16;
    x ^= x >> 8;
    x ^= x >> 4;
    x &= 0xF;
    return (0x6996 >> x) & 1;
}

// Turn-around cycle
static void swd_turnaround(bool to_write) {
    if (to_write) {
        SWDIO_H();
        SWDIO_RELEASE();
        clock_pulse();
        SWDIO_DRIVE();
    } else {
        SWDIO_H();
        SWDIO_RELEASE();
        clock_pulse();
    }
    drive_phase = to_write;
}

// Write bits LSB first
static void write_bits(uint32_t value, uint8_t count) {
    if (!drive_phase) {
        swd_turnaround(true);
    }
    
    while (count--) {
        if (value & 1) {
            SWDIO_H();
        } else {
            SWDIO_L();
        }
        clock_pulse();
        value >>= 1;
    }
}

// Read bits LSB first
static uint32_t read_bits(uint8_t count) {
    if (drive_phase) {
        swd_turnaround(false);
    }
    
    uint32_t result = 0;
    uint32_t bit = 1;
    
    while (count--) {
        if (READ_SWDIO()) {
            result |= bit;
        }
        clock_pulse();
        bit <<= 1;
    }
    
    return result;
}

// Send SWD request
static void send_request(uint8_t addr, bool ap, bool read) {
    uint8_t request = 0x81;  // Start bit and park bit
    
    if (ap) request |= (1 << 1);
    if (read) request |= (1 << 2);
    
    request |= (addr & 0x0C) << 1;
    
    // Calculate parity
    bool parity = ap ^ read ^ ((addr >> 2) & 1) ^ ((addr >> 3) & 1);
    if (parity) request |= (1 << 5);
    
    write_bits(request, 8);
}

// Write parking bit
static void write_parking(void) {
    if (!drive_phase) {
        swd_turnaround(true);
    }
    SWDIO_L();
    clock_pulse();
}

// Line reset (50+ clocks high)
static void line_reset(void) {
    SWDIO_DRIVE();
    SWDIO_H();
    for (int i = 0; i < 60; i++) {
        clock_pulse();
    }
    SWDIO_L();
    clock_pulse();
}

// JTAG to SWD sequence
static void jtag_to_swd(void) {
    SWDIO_DRIVE();
    
    // Send magic sequence
    uint32_t sequence = 0xE79E;  // 16-bit JTAG-to-SWD
    for (int i = 0; i < 16; i++) {
        if (sequence & (1 << i)) {
            SWDIO_H();
        } else {
            SWDIO_L();
        }
        clock_pulse();
    }
    
    // Additional reset
    line_reset();
}

// Dormant wakeup sequence
static void dormant_wakeup(void) {
    SWDIO_DRIVE();
    
    // 8 cycles high
    SWDIO_H();
    for (int i = 0; i < 8; i++) {
        clock_pulse();
    }
    
    // 128-bit selection alert sequence
    uint32_t alert[4] = {0x49CF9046, 0xA9B4A161, 0x97F5BBC7, 0x45703D98};
    
    for (int w = 0; w < 4; w++) {
        uint32_t word = alert[w];
        for (int b = 31; b >= 0; b--) {
            if (word & (1UL << b)) {
                SWDIO_H();
            } else {
                SWDIO_L();
            }
            clock_pulse();
        }
    }
    
    // 4 cycles low
    SWDIO_L();
    for (int i = 0; i < 4; i++) {
        clock_pulse();
    }
    
    // Activation code (0x58 for SWD)
    uint8_t activation = 0x58;
    for (int i = 7; i >= 0; i--) {
        if (activation & (1 << i)) {
            SWDIO_H();
        } else {
            SWDIO_L();
        }
        clock_pulse();
    }
    
    // Line reset
    line_reset();
}

// Initialize SWD interface
esp_err_t swd_init(const swd_config_t *cfg) {
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    
    config = *cfg;
    
    // Initialize GPIOs
    gpio_reset_pin((gpio_num_t)config.pin_swclk);
    gpio_reset_pin((gpio_num_t)config.pin_swdio);
    
    gpio_set_direction((gpio_num_t)config.pin_swclk, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)config.pin_swdio, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_pull_mode((gpio_num_t)config.pin_swdio, GPIO_PULLUP_ONLY);
    
    SWCLK_L();
    SWDIO_H();
    SWDIO_DRIVE();
    
    // Optional reset pin
    if (config.pin_reset >= 0) {
        gpio_reset_pin((gpio_num_t)config.pin_reset);
        gpio_set_direction((gpio_num_t)config.pin_reset, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)config.pin_reset, 1);
    }
    
    drive_phase = true;
    initialized = true;
    connected = false;
    
    ESP_LOGI(TAG, "SWD initialized: SWCLK=%d, SWDIO=%d, nRST=%d",
             config.pin_swclk, config.pin_swdio, config.pin_reset);
    
    return ESP_OK;
}

// Raw SWD transfer
swd_ack_t swd_transfer_raw(uint8_t addr, bool ap, bool read, uint32_t *data) {
    portENTER_CRITICAL(&swd_mutex);
    
    // Send request
    send_request(addr, ap, read);
    
    // Read ACK
    if (drive_phase) {
        swd_turnaround(false);
    }
    
    uint8_t ack = (uint8_t)read_bits(3);
    
    if (ack == SWD_ACK_OK) {
        if (read) {
            // Read data and parity
            uint32_t value = read_bits(32);
            uint8_t parity_bit = (uint8_t)read_bits(1);
            
            // Turnaround to write
            swd_turnaround(true);
            write_parking();
            
            portEXIT_CRITICAL(&swd_mutex);
            
            // Verify parity
            if (parity_bit != parity32(value)) {
                ESP_LOGW(TAG, "Parity error on read");
                return SWD_ACK_FAULT;
            }
            
            *data = value;
        } else {
            // Turnaround to write
            swd_turnaround(true);
            
            // Write data and parity
            write_bits(*data, 32);
            write_bits(parity32(*data), 1);
            write_parking();
            
            portEXIT_CRITICAL(&swd_mutex);
        }
    } else {
        // Error - send dummy clocks
        swd_turnaround(true);
        write_bits(0, 32);
        write_parking();
        
        portEXIT_CRITICAL(&swd_mutex);
    }
    
    return (swd_ack_t)ack;
}

// DP read with retry
esp_err_t swd_dp_read(uint8_t addr, uint32_t *data) {
    if (!initialized || !data) {
        return ESP_ERR_INVALID_STATE;
    }
    
    for (int retry = 0; retry < 10; retry++) {
        swd_ack_t ack = swd_transfer_raw(addr, false, true, data);
        
        if (ack == SWD_ACK_OK) {
            return ESP_OK;
        } else if (ack == SWD_ACK_WAIT) {
            vTaskDelay(1);
        } else if (ack == SWD_ACK_FAULT) {
            // Clear sticky error
            swd_clear_errors();
        }
    }
    
    ESP_LOGE(TAG, "DP read failed: addr=0x%02X", addr);
    return ESP_FAIL;
}

// DP write with retry
esp_err_t swd_dp_write(uint8_t addr, uint32_t data) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    for (int retry = 0; retry < 10; retry++) {
        swd_ack_t ack = swd_transfer_raw(addr, false, false, &data);
        
        if (ack == SWD_ACK_OK) {
            return ESP_OK;
        } else if (ack == SWD_ACK_WAIT) {
            vTaskDelay(1);
        } else if (ack == SWD_ACK_FAULT) {
            swd_clear_errors();
        }
    }
    
    ESP_LOGE(TAG, "DP write failed: addr=0x%02X data=0x%08lX", addr, data);
    return ESP_FAIL;
}

esp_err_t swd_dp_disconnect(void) {
    ESP_LOGI(TAG, "Performing full DP disconnect sequence...");

    // 1. First power down the debug domain
    esp_err_t ret = swd_dp_write(DP_CTRL_STAT, 0x00000000);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to clear CTRL_STAT");
    }

    // 2. Wait for power down acknowledgment
    for (int i = 0; i < 50; i++) {
        uint32_t status = 0;
        ret = swd_dp_read(DP_CTRL_STAT, &status);
        if (ret == ESP_OK) {
            // Check if both CSYSPWRUPACK and CDBGPWRUPACK are cleared
            if ((status & 0xA0000000) == 0) {
                ESP_LOGI(TAG, "Debug domain powered down");
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 3. Send disconnect sequence (50+ clocks with SWDIO high)
    // This is the JTAG-to-dormant sequence that puts the DP in dormant state
    SWDIO_DRIVE();
    SWDIO_H();
    for (int i = 0; i < 60; i++) {
        clock_pulse();
    }

    // 4. Send SWD-to-dormant sequence (specific pattern)
    // This ensures the DP is fully dormant
    uint32_t dormant_seq = 0xE3BC;  // SWD to dormant
    for (int i = 0; i < 16; i++) {
        if (dormant_seq & (1 << i)) {
            SWDIO_H();
        } else {
            SWDIO_L();
        }
        clock_pulse();
    }

    ESP_LOGI(TAG, "DP disconnect complete");
    return ESP_OK;
}

// AP read with retry
esp_err_t swd_ap_read(uint8_t addr, uint32_t *data) {
    if (!initialized || !data) {
        return ESP_ERR_INVALID_STATE;
    }
    
    for (int retry = 0; retry < 10; retry++) {
        swd_ack_t ack = swd_transfer_raw(addr, true, true, data);
        
        if (ack == SWD_ACK_OK) {
            // Need to read RDBUFF for the actual data
            return swd_dp_read(DP_RDBUFF, data);
        } else if (ack == SWD_ACK_WAIT) {
            vTaskDelay(1);
        } else if (ack == SWD_ACK_FAULT) {
            swd_clear_errors();
        }
    }
    
    ESP_LOGE(TAG, "AP read failed: addr=0x%02X", addr);
    return ESP_FAIL;
}

// AP write with retry
esp_err_t swd_ap_write(uint8_t addr, uint32_t data) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    for (int retry = 0; retry < 10; retry++) {
        swd_ack_t ack = swd_transfer_raw(addr, true, false, &data);
        
        if (ack == SWD_ACK_OK) {
            return ESP_OK;
        } else if (ack == SWD_ACK_WAIT) {
            vTaskDelay(1);
        } else if (ack == SWD_ACK_FAULT) {
            swd_clear_errors();
        }
    }
    
    ESP_LOGE(TAG, "AP write failed: addr=0x%02X data=0x%08lX", addr, data);
    return ESP_FAIL;
}

// Connect to target
esp_err_t swd_connect(void) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Attempting SWD connection...");
    
    // Try dormant wakeup first
    dormant_wakeup();
    
    uint32_t idcode = 0;
    esp_err_t ret = swd_dp_read(DP_IDCODE, &idcode);
    
    if (ret != ESP_OK || idcode == 0 || idcode == 0xFFFFFFFF) {
        ESP_LOGW(TAG, "Dormant wakeup failed, trying JTAG-to-SWD");
        
        // Try JTAG to SWD sequence
        line_reset();
        jtag_to_swd();
        
        ret = swd_dp_read(DP_IDCODE, &idcode);
        if (ret != ESP_OK || idcode == 0 || idcode == 0xFFFFFFFF) {
            ESP_LOGE(TAG, "Failed to connect to target");
            return ESP_FAIL;
        }
    }
    
    ESP_LOGI(TAG, "Connected: IDCODE=0x%08lX", idcode);
    
    // Power up debug domain
    ret = swd_power_up();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to power up debug");
        return ret;
    }
    
    connected = true;
    return ESP_OK;
}

// Replace the existing swd_disconnect() function with:
esp_err_t swd_disconnect(void) {
    return swd_disconnect_enhanced();
}

// Check connection status
bool swd_is_connected(void) {
    if (!connected) {
        return false;
    }
    
    // Verify by reading IDCODE
    uint32_t idcode = 0;
    esp_err_t ret = swd_dp_read(DP_IDCODE, &idcode);
    
    if (ret != ESP_OK || idcode == 0 || idcode == 0xFFFFFFFF) {
        connected = false;
        return false;
    }
    
    return true;
}

/**
 * @brief Reset target device via hardware reset pin
 *
 * Asserts nRST, waits for target to reset, then attempts reconnection.
 * Includes proper backoff to avoid hammering the DP while target is resetting.
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t swd_reset_target(void) {
    if (config.pin_reset < 0) {
        ESP_LOGW(TAG, "No reset pin configured");
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "Resetting target...");

    // Assert hardware reset
    gpio_set_level((gpio_num_t)config.pin_reset, 0);  // LOW = Reset asserted
    vTaskDelay(pdMS_TO_TICKS(10));  // Hold reset for 10ms
    gpio_set_level((gpio_num_t)config.pin_reset, 1);  // HIGH = Release reset

    // CRITICAL: Wait for target to complete reset sequence
    // Don't poll during this time - target is not responding
    ESP_LOGI(TAG, "Waiting for target reset to complete...");
    vTaskDelay(pdMS_TO_TICKS(50));  // 50ms reset recovery time

    // Now target should be ready - attempt clean reconnection
    ESP_LOGI(TAG, "Attempting reconnection after reset...");

    // Try to read IDCODE as a connectivity test
    uint32_t idcode = 0;
    esp_err_t ret = swd_dp_read(DP_IDCODE, &idcode);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Initial IDCODE read failed after reset, retrying...");
        vTaskDelay(pdMS_TO_TICKS(50));  // Additional backoff
        ret = swd_dp_read(DP_IDCODE, &idcode);
    }

    if (ret == ESP_OK && idcode != 0 && idcode != 0xFFFFFFFF) {
        ESP_LOGI(TAG, "Target reset complete, IDCODE: 0x%08lX", idcode);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Target not responding after reset (IDCODE: 0x%08lX)", idcode);
        return ESP_FAIL;
    }
}

// Clear sticky errors
esp_err_t swd_clear_errors(void) {
    // Write ABORT register to clear errors
    // ORUNERRCLR, WDERRCLR, STKERRCLR, STKCMPCLR, DAPABORT
    uint32_t abort_val = 0x1E;
    return swd_dp_write(DP_ABORT, abort_val);
}

// Get IDCODE
uint32_t swd_get_idcode(void) {
    uint32_t idcode = 0;
    swd_dp_read(DP_IDCODE, &idcode);
    return idcode;
}

/**
 * @brief Power up debug domain
 *
 * Requests debug power and waits for acknowledgment with proper backoff.
 *
 * @return ESP_OK on success
 */
esp_err_t swd_power_up(void) {
    ESP_LOGI(TAG, "Powering up debug domain...");

    // Clear any errors first
    swd_clear_errors();

    // Request debug and system power (CDBGPWRUPREQ | CSYSPWRUPREQ)
    esp_err_t ret = swd_dp_write(DP_CTRL_STAT, 0x50000000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to request power");
        return ret;
    }

    // CRITICAL: Initial delay before first poll
    // Give hardware time to respond before we start checking
    vTaskDelay(pdMS_TO_TICKS(10));

    // Poll for power-up acknowledgment with exponential backoff
    uint32_t status = 0;
    for (int retry = 0; retry < 20; retry++) {
        ret = swd_dp_read(DP_CTRL_STAT, &status);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "CTRL/STAT read failed during power-up (retry %d)", retry);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Check if both power domains are up (CDBGPWRUPACK | CSYSPWRUPACK)
        bool cdbg_up = (status & 0x20000000) != 0;
        bool csys_up = (status & 0x80000000) != 0;

        if (cdbg_up && csys_up) {
            ESP_LOGI(TAG, "Debug powered up: status=0x%08lX", status);

            // Clear any sticky errors that might have occurred during power-up
            swd_clear_errors();
            return ESP_OK;
        }

        // Exponential backoff: 5ms, 10ms, 20ms, 40ms, ...
        uint32_t delay_ms = 5 << (retry / 4);  // Increases every 4 retries
        if (delay_ms > 100) delay_ms = 100;    // Cap at 100ms

        ESP_LOGD(TAG, "Waiting for power-up ack (retry %d, delay %lums): CDBG=%d CSYS=%d",
                retry, delay_ms, cdbg_up, csys_up);

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    ESP_LOGE(TAG, "Debug power-up timeout (final status: 0x%08lX)", status);
    return ESP_ERR_TIMEOUT;
}

// Add all these new functions at the end of swd_core.c

// Check if SWD interface is initialized
bool swd_is_initialized(void) {
    return initialized;
}

// Release target from debug mode
esp_err_t swd_release_target(void) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Releasing target from debug mode...");

    // 1. Resume core if halted
    uint32_t dhcsr;
    esp_err_t ret = swd_mem_read32(DHCSR_ADDR, &dhcsr);
    if (ret == ESP_OK && (dhcsr & DHCSR_S_HALT)) {
        ESP_LOGI(TAG, "Core is halted, resuming...");
        ret = swd_mem_write32(DHCSR_ADDR, DHCSR_DBGKEY | DHCSR_C_DEBUGEN);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 2. Clear all debug features
    ESP_LOGI(TAG, "Disabling debug mode...");
    ret = swd_mem_write32(DHCSR_ADDR, DHCSR_DBGKEY);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to clear DHCSR");
    }

    // 3. Clear DEMCR
    ret = swd_mem_write32(DEMCR_ADDR, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to clear DEMCR");
    }

    // 4. Perform system reset
    if (config.pin_reset >= 0) {
        ESP_LOGI(TAG, "Performing hardware reset...");
        gpio_set_level((gpio_num_t)config.pin_reset, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level((gpio_num_t)config.pin_reset, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    } else {
        ESP_LOGI(TAG, "Performing software reset via AIRCR...");
        ret = swd_mem_write32(NRF52_AIRCR, 0x05FA0004);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // 5. CRITICAL: Perform full DP disconnect (this is what pyOCD does)
    swd_dp_disconnect();

    ESP_LOGI(TAG, "Target release complete");
    return ESP_OK;
}

esp_err_t swd_shutdown(void) {
    if (!initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Shutting down SWD interface...");

    if (connected) {
        // Do full disconnect with DP power down
        swd_dp_disconnect();
        connected = false;
    }

    // Now release GPIO pins to high-impedance
    gpio_set_direction((gpio_num_t)config.pin_swclk, GPIO_MODE_INPUT);
    gpio_set_direction((gpio_num_t)config.pin_swdio, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)config.pin_swclk, GPIO_FLOATING);
    gpio_set_pull_mode((gpio_num_t)config.pin_swdio, GPIO_FLOATING);

    if (config.pin_reset >= 0) {
        gpio_set_level((gpio_num_t)config.pin_reset, 1);
        gpio_set_direction((gpio_num_t)config.pin_reset, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)config.pin_reset, GPIO_FLOATING);
    }

    initialized = false;
    drive_phase = true;

    ESP_LOGI(TAG, "SWD interface shutdown complete");
    return ESP_OK;
}

// Enhanced disconnect
esp_err_t swd_disconnect_enhanced(void) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Disconnecting from target...");
    
    if (connected) {
        swd_release_target();
        connected = false;
    }
    
    ESP_LOGI(TAG, "Disconnected from target");
    return ESP_OK;
}

// Reinitialize SWD when needed
esp_err_t swd_reinit(void) {
    if (initialized) {
        gpio_set_direction((gpio_num_t)config.pin_swclk, GPIO_MODE_OUTPUT);
        gpio_set_direction((gpio_num_t)config.pin_swdio, GPIO_MODE_INPUT_OUTPUT);
        gpio_set_pull_mode((gpio_num_t)config.pin_swdio, GPIO_PULLUP_ONLY);
        
        if (config.pin_reset >= 0) {
            gpio_set_direction((gpio_num_t)config.pin_reset, GPIO_MODE_OUTPUT);
            gpio_set_level((gpio_num_t)config.pin_reset, 1);
        }
        
        SWCLK_L();
        SWDIO_H();
        SWDIO_DRIVE();
        drive_phase = true;
        
        return ESP_OK;
    }
    
    return swd_init(&config);
}