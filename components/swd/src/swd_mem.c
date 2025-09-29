// swd_mem.c - Memory Access Implementation
#include "swd_mem.h"
#include "swd_core.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "SWD_MEM";

// Initialize memory access
esp_err_t swd_mem_init(void) {
    // Select AP 0 (typically AHB-AP)
    esp_err_t ret = swd_dp_write(DP_SELECT, 0x00000000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to select AP0");
        return ret;
    }
    
    // Configure CSW for 32-bit auto-increment
    ret = swd_ap_write(AP_CSW, CSW_DEFAULT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure CSW");
        return ret;
    }
    
    ESP_LOGI(TAG, "Memory access initialized");
    return ESP_OK;
}

// Read single 32-bit word
esp_err_t swd_mem_read32(uint32_t addr, uint32_t *data) {
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Set target address
    esp_err_t ret = swd_ap_write(AP_TAR, addr);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Initiate read (dummy read to trigger transfer)
    uint32_t dummy;
    ret = swd_ap_read(AP_DRW, &dummy);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Read actual data from RDBUFF
    ret = swd_dp_read(DP_RDBUFF, data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read from 0x%08lX", addr);
    }
    
    return ret;
}

// Write single 32-bit word
esp_err_t swd_mem_write32(uint32_t addr, uint32_t data) {
    // Set target address
    esp_err_t ret = swd_ap_write(AP_TAR, addr);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Write data
    ret = swd_ap_write(AP_DRW, data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write 0x%08lX to 0x%08lX", data, addr);
        return ret;
    }
    
    // Read RDBUFF to ensure write completes
    uint32_t dummy;
    ret = swd_dp_read(DP_RDBUFF, &dummy);
    
    return ret;
}

// Read buffer
esp_err_t swd_mem_read_buffer(uint32_t addr, uint8_t *buffer, uint32_t size) {
    if (!buffer || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ESP_OK;
    
    // Handle unaligned start
    if (addr & 0x3) {
        uint32_t aligned_addr = addr & ~0x3;
        uint32_t offset = addr & 0x3;
        uint32_t word;
        
        ret = swd_mem_read32(aligned_addr, &word);
        if (ret != ESP_OK) return ret;
        
        uint8_t *word_bytes = (uint8_t*)&word;
        uint32_t bytes_to_copy = 4 - offset;
        if (bytes_to_copy > size) bytes_to_copy = size;
        
        memcpy(buffer, &word_bytes[offset], bytes_to_copy);
        
        addr += bytes_to_copy;
        buffer += bytes_to_copy;
        size -= bytes_to_copy;
    }
    
    // Read aligned words
    while (size >= 4) {
        uint32_t word;
        ret = swd_mem_read32(addr, &word);
        if (ret != ESP_OK) return ret;
        
        memcpy(buffer, &word, 4);
        addr += 4;
        buffer += 4;
        size -= 4;
    }
    
    // Handle remaining bytes
    if (size > 0) {
        uint32_t word;
        ret = swd_mem_read32(addr, &word);
        if (ret != ESP_OK) return ret;
        
        memcpy(buffer, &word, size);
    }
    
    return ESP_OK;
}

// Add this function to swd_mem.c with minimal CSW configuration

esp_err_t swd_mem_write_block32(uint32_t addr, const uint32_t *data, uint32_t count) {
    if (!data || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Must be word-aligned
    if (addr & 0x3) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;

    // Set CSW for 32-bit auto-increment mode with basic settings
    // Just use the essential bits needed for auto-increment writes
    uint32_t csw_value = CSW_ADDRINC_ON | CSW_SIZE_32BIT | CSW_DEVICE_EN | CSW_MASTER_DBG;

    ret = swd_ap_write(AP_CSW, csw_value);
    if (ret != ESP_OK) return ret;

    // Write words in chunks that don't cross auto-increment boundaries
    // The auto-increment wraps at 1KB boundaries (0x400) by default
    uint32_t auto_inc_size = 0x400;  // 1KB auto-increment boundary

    while (count > 0) {
        // Calculate how many words we can write before hitting boundary
        uint32_t offset_in_page = addr & (auto_inc_size - 1);
        uint32_t words_in_page = (auto_inc_size - offset_in_page) / 4;
        if (words_in_page > count) {
            words_in_page = count;
        }

        // Set target address (only needed once per auto-increment sequence)
        ret = swd_ap_write(AP_TAR, addr);
        if (ret != ESP_OK) return ret;

        // Write all words in this chunk using auto-increment
        // This is the key optimization - we write multiple words to DRW
        // without updating TAR each time
        for (uint32_t i = 0; i < words_in_page; i++) {
            ret = swd_ap_write(AP_DRW, data[i]);
            if (ret != ESP_OK) return ret;
        }

        // Read RDBUFF once to ensure all writes complete
        uint32_t dummy;
        ret = swd_dp_read(DP_RDBUFF, &dummy);
        if (ret != ESP_OK) return ret;

        // Move to next chunk
        addr += words_in_page * 4;
        data += words_in_page;
        count -= words_in_page;
    }

    return ESP_OK;
}
