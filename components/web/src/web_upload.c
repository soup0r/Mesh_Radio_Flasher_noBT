#include "web_upload.h"
#include "esp_log.h"
#include "hex_parser.h"
#include "swd_flash.h"
#include "swd_mem.h"
#include "swd_core.h"
#include "nrf52_hal.h"
#include "power_mgmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <unistd.h>  // For close() function
#include <sys/socket.h>  // For socket options
#include <netinet/tcp.h>  // For TCP options
#include <sys/stat.h>  // For stat()
#include <stdio.h>  // For FILE operations

static const char *TAG = "WEB_UPLOAD";

#define UPLOAD_ABSOLUTE_TIMEOUT_SEC 600    // 10 minutes max for entire upload
#define UPLOAD_NO_PROGRESS_TIMEOUT_SEC 60  // Fail if no progress for 60 seconds
#define UPLOAD_LINE_BUFFER_SIZE 600        // Max hex line is ~520 chars
#define UPLOAD_RECV_BUFFER_SIZE 1024       // 1KB for ESP-LR compatibility
#define PAGE_BUFFER_SIZE (4 * 1024)        // Reduce from 16KB to 4KB
#define NRF52_PAGE_SIZE 4096               // Keep existing if already defined

// ESP-LR optimized chunk sizes
#define LR_CHUNK_SIZE 1024        // 1KB chunks for LR mode
#define NORMAL_CHUNK_SIZE 4096    // 4KB chunks for normal mode

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

// Enhanced structure with packet loss tolerance
typedef struct {
    bool in_progress;
    uint32_t total_bytes;
    uint32_t received_bytes;
    uint32_t flashed_bytes;
    uint32_t start_addr;
    uint32_t current_addr;
    hex_stream_parser_t *parser;
    uint8_t *page_buffer;
    uint32_t buffer_start_addr;
    uint32_t buffer_data_len;
    char status_msg[128];
    bool error;

    // Packet loss tolerance fields
    char line_buffer[UPLOAD_LINE_BUFFER_SIZE];  // Buffer for incomplete hex lines
    size_t line_pos;                            // Current position in line buffer
    uint32_t last_progress_bytes;               // Bytes at last progress check
    TickType_t last_progress_time;              // Time of last progress
    TickType_t upload_start_time;               // Upload start time
    uint32_t retry_count;                       // Current retry count
    uint32_t total_retries;                     // Total retries during upload
} upload_context_t;

static upload_context_t *g_upload_ctx = NULL;
static bool g_mass_erased = false;

// Flash status tracking
static bool g_flash_in_progress = false;
static int g_flash_percent = 0;
static char g_flash_status[128] = "Idle";

// Helper function to ensure SWD is ready
static esp_err_t ensure_swd_ready(void) {
    if (!swd_is_initialized()) {
        ESP_LOGI(TAG, "Reinitializing SWD for operation...");
        
        swd_config_t swd_cfg = {
            .pin_swclk = 4,  // ESP32C3 GPIO4
            .pin_swdio = 3,  // ESP32C3 GPIO3
            .pin_reset = 5,  // ESP32C3 GPIO5
            .delay_cycles = 0
        };
        
        esp_err_t ret = swd_init(&swd_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reinitialize SWD");
            return ret;
        }
    } else {
        swd_reinit();
    }
    
    if (!swd_is_connected()) {
        ESP_LOGI(TAG, "Connecting to target...");
        esp_err_t ret = swd_connect();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to connect to target");
            return ret;
        }
        
        ret = swd_flash_init();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Flash init failed: %s", esp_err_to_name(ret));
        }
    }
    
    return ESP_OK;
}


// Flush buffer to flash
static esp_err_t flush_buffer(upload_context_t *ctx) {
    if (ctx->buffer_data_len == 0) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Flushing buffer: addr=0x%08lX, len=%lu",
             ctx->buffer_start_addr, ctx->buffer_data_len);

    // Only erase if not mass erased
    if (!g_mass_erased) {
        uint32_t start_page = ctx->buffer_start_addr & ~(NRF52_PAGE_SIZE - 1);
        uint32_t end_addr = ctx->buffer_start_addr + ctx->buffer_data_len - 1;
        uint32_t end_page = end_addr & ~(NRF52_PAGE_SIZE - 1);

        for (uint32_t page = start_page; page <= end_page; page += NRF52_PAGE_SIZE) {
            ESP_LOGI(TAG, "Erasing page 0x%08lX", page);
            esp_err_t ret = swd_flash_erase_page(page);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to erase page 0x%08lX", page);
                return ret;  // Fail immediately
            }
        }
    } else {
        ESP_LOGI(TAG, "Skipping page erases (mass erase done)");
    }

    // Single write attempt, no retries
    ESP_LOGI(TAG, "Writing %lu bytes", ctx->buffer_data_len);
    esp_err_t ret = swd_flash_write_buffer(ctx->buffer_start_addr,
                                           ctx->page_buffer,
                                           ctx->buffer_data_len);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write buffer");
        return ret;  // Fail immediately
    }

    ctx->flashed_bytes += ctx->buffer_data_len;
    ESP_LOGI(TAG, "Successfully flushed %lu bytes", ctx->buffer_data_len);

    // Clear buffer
    memset(ctx->page_buffer, 0xFF, PAGE_BUFFER_SIZE);
    ctx->buffer_data_len = 0;

    return ESP_OK;
}

// Hex record callback
static void hex_flash_callback(hex_record_t *record, uint32_t abs_addr, void *ctx) {
    upload_context_t *uctx = (upload_context_t*)ctx;
    
    switch (record->type) {
        case HEX_TYPE_DATA: {
            // Check if this data fits in current buffer
            uint32_t offset_in_buffer = abs_addr - uctx->buffer_start_addr;
            
            if (uctx->buffer_data_len == 0) {
                // First data - initialize buffer
                uctx->buffer_start_addr = abs_addr;
                offset_in_buffer = 0;
            } else if (abs_addr < uctx->buffer_start_addr || 
                      offset_in_buffer + record->byte_count > PAGE_BUFFER_SIZE) {
                // Data doesn't fit - flush current buffer
                flush_buffer(uctx);
                uctx->buffer_start_addr = abs_addr;
                offset_in_buffer = 0;
            }
            
            // Copy data to buffer
            memcpy(uctx->page_buffer + offset_in_buffer, record->data, record->byte_count);
            
            // Update buffer length
            uint32_t new_end = offset_in_buffer + record->byte_count;
            if (new_end > uctx->buffer_data_len) {
                uctx->buffer_data_len = new_end;
            }
            
            uctx->current_addr = abs_addr + record->byte_count;
            break;
        }
        
        case HEX_TYPE_EOF:
            flush_buffer(uctx);
            g_mass_erased = false;  // Clear flag after use
            ESP_LOGI(TAG, "Upload complete: %lu bytes flashed", uctx->flashed_bytes);
            snprintf(uctx->status_msg, sizeof(uctx->status_msg),
                    "Success: Flashed %lu bytes", uctx->flashed_bytes);

            // UNCOMMENT THESE - they were working before:
            ESP_LOGI(TAG, "Flashing complete, performing reset sequence...");
            swd_flash_reset_and_run();  // <-- RESTORE THIS
            swd_shutdown();              // <-- RESTORE THIS
            ESP_LOGI(TAG, "Target released - should now boot normally");
            break;
            
        case HEX_TYPE_EXT_LIN_ADDR:
            // Flush buffer before address change
            if (uctx->buffer_data_len > 0) {
                flush_buffer(uctx);
            }
            break;
    }
}


// Check SWD connection handler
esp_err_t check_swd_handler(httpd_req_t *req) {
    char resp[4096];  // Increased buffer size
    ESP_LOGI(TAG, "=== SWD Status Check Requested ===");

    // Check and try to reconnect if needed
    esp_err_t ret = ensure_swd_ready();
    bool connected = (ret == ESP_OK);

    if (connected) {
        ESP_LOGI(TAG, "SWD Connected - Reading registers...");
        ESP_LOGI(TAG, "=== NRF52 Register Dump ===");

        // Read all important registers
        uint32_t nvmc_ready = 0, nvmc_readynext = 0, nvmc_config = 0;
        uint32_t approtect = 0, bootloader_addr = 0, nrffw0 = 0, nrffw1 = 0;
        uint32_t codepagesize = 0, codesize = 0, deviceid0 = 0, deviceid1 = 0;
        uint32_t info_part = 0, info_variant = 0, info_ram = 0, info_flash = 0;
        uint32_t dhcsr = 0, demcr = 0;
        uint32_t flash_0x0 = 0, flash_0x1000 = 0, flash_0xF4000 = 0;

        // NVMC registers
        swd_mem_read32(NVMC_READY, &nvmc_ready);
        ESP_LOGI(TAG, "NVMC_READY: 0x%08lX", nvmc_ready);

        swd_mem_read32(NVMC_READYNEXT, &nvmc_readynext);
        ESP_LOGI(TAG, "NVMC_READYNEXT: 0x%08lX", nvmc_readynext);

        swd_mem_read32(NVMC_CONFIG, &nvmc_config);
        ESP_LOGI(TAG, "NVMC_CONFIG: 0x%08lX", nvmc_config);

        // UICR registers
        swd_mem_read32(UICR_APPROTECT, &approtect);
        ESP_LOGI(TAG, "UICR_APPROTECT: 0x%08lX", approtect);

        swd_mem_read32(UICR_BOOTLOADERADDR, &bootloader_addr);
        ESP_LOGI(TAG, "UICR_BOOTLOADERADDR: 0x%08lX", bootloader_addr);

        swd_mem_read32(UICR_NRFFW0, &nrffw0);
        ESP_LOGI(TAG, "UICR_NRFFW0: 0x%08lX", nrffw0);

        swd_mem_read32(UICR_NRFFW1, &nrffw1);
        ESP_LOGI(TAG, "UICR_NRFFW1: 0x%08lX", nrffw1);

        // FICR registers
        swd_mem_read32(FICR_CODEPAGESIZE, &codepagesize);
        ESP_LOGI(TAG, "FICR_CODEPAGESIZE: 0x%08lX", codepagesize);

        swd_mem_read32(FICR_CODESIZE, &codesize);
        ESP_LOGI(TAG, "FICR_CODESIZE: 0x%08lX", codesize);

        swd_mem_read32(FICR_DEVICEID0, &deviceid0);
        ESP_LOGI(TAG, "FICR_DEVICEID0: 0x%08lX", deviceid0);

        swd_mem_read32(FICR_DEVICEID1, &deviceid1);
        ESP_LOGI(TAG, "FICR_DEVICEID1: 0x%08lX", deviceid1);

        swd_mem_read32(FICR_INFO_PART, &info_part);
        ESP_LOGI(TAG, "FICR_INFO_PART: 0x%08lX", info_part);

        swd_mem_read32(FICR_INFO_VARIANT, &info_variant);
        ESP_LOGI(TAG, "FICR_INFO_VARIANT: 0x%08lX", info_variant);

        swd_mem_read32(FICR_INFO_RAM, &info_ram);
        ESP_LOGI(TAG, "FICR_INFO_RAM: 0x%08lX", info_ram);

        swd_mem_read32(FICR_INFO_FLASH, &info_flash);
        ESP_LOGI(TAG, "FICR_INFO_FLASH: 0x%08lX", info_flash);

        // Debug registers
        swd_mem_read32(DHCSR_ADDR, &dhcsr);
        ESP_LOGI(TAG, "DHCSR: 0x%08lX", dhcsr);

        swd_mem_read32(DEMCR_ADDR, &demcr);
        ESP_LOGI(TAG, "DEMCR: 0x%08lX", demcr);

        // Flash content samples
        swd_mem_read32(0x00000000, &flash_0x0);
        ESP_LOGI(TAG, "Flash[0x0000]: 0x%08lX", flash_0x0);

        swd_mem_read32(0x00001000, &flash_0x1000);
        ESP_LOGI(TAG, "Flash[0x1000]: 0x%08lX", flash_0x1000);

        swd_mem_read32(0x000F4000, &flash_0xF4000);
        ESP_LOGI(TAG, "Flash[0xF4000]: 0x%08lX", flash_0xF4000);

        ESP_LOGI(TAG, "=== Register Dump Complete ===");

        // Determine APPROTECT status string
        const char *approtect_status;
        if (approtect == 0xFFFFFFFF) {
            approtect_status = "Disabled (Open for debug)";
        } else if (approtect == 0x0000005A || approtect == 0xFFFFFF5A) {
            approtect_status = "HwDisabled (Hardware unlocked)";
        } else if (approtect == 0xFFFFFF00 || approtect == 0x00000000) {
            approtect_status = "ENABLED (Locked - Mass erase required!)";
        } else {
            approtect_status = "Unknown/Custom value";
        }

        // Determine NVMC state
        const char *nvmc_state;
        if ((nvmc_config & 0x3) == 0x00) {
            nvmc_state = "Read-only";
        } else if ((nvmc_config & 0x3) == 0x01) {
            nvmc_state = "Write enabled";
        } else if ((nvmc_config & 0x3) == 0x02) {
            nvmc_state = "Erase enabled";
        } else {
            nvmc_state = "Unknown";
        }

        // Check if core is halted
        bool core_halted = (dhcsr & DHCSR_S_HALT) != 0;

        // Build JSON response with ALL register data
        snprintf(resp, sizeof(resp),
            "{"
            "\"connected\":true,"
            "\"status\":\"Connected\","
            "\"approtect\":\"0x%08lX\","
            "\"approtect_status\":\"%s\","
            "\"nvmc_ready\":%s,"
            "\"nvmc_state\":\"%s\","
            "\"core_halted\":%s,"
            "\"bootloader_addr\":\"0x%08lX\","
            "\"device_id\":\"0x%08lX%08lX\","
            "\"flash_size\":%lu,"
            "\"ram_size\":%lu,"
            "\"registers\":{"
                "\"nvmc_ready\":\"0x%08lX\","
                "\"nvmc_readynext\":\"0x%08lX\","
                "\"nvmc_config\":\"0x%08lX\","
                "\"approtect\":\"0x%08lX\","
                "\"bootloader_addr\":\"0x%08lX\","
                "\"nrffw0\":\"0x%08lX\","
                "\"nrffw1\":\"0x%08lX\","
                "\"codepagesize\":\"0x%08lX\","
                "\"codesize\":\"0x%08lX\","
                "\"deviceid0\":\"0x%08lX\","
                "\"deviceid1\":\"0x%08lX\","
                "\"info_part\":\"0x%08lX\","
                "\"info_variant\":\"0x%08lX\","
                "\"info_ram\":\"0x%08lX\","
                "\"info_flash\":\"0x%08lX\","
                "\"dhcsr\":\"0x%08lX\","
                "\"demcr\":\"0x%08lX\","
                "\"flash_0x0\":\"0x%08lX\","
                "\"flash_0x1000\":\"0x%08lX\","
                "\"flash_0xF4000\":\"0x%08lX\""
            "}"
            "}",
            approtect,
            approtect_status,
            (nvmc_ready & 0x1) ? "true" : "false",
            nvmc_state,
            core_halted ? "true" : "false",
            bootloader_addr,
            deviceid1, deviceid0,
            info_flash * 1024UL,
            info_ram * 1024UL,
            nvmc_ready,
            nvmc_readynext,
            nvmc_config,
            approtect,
            bootloader_addr,
            nrffw0,
            nrffw1,
            codepagesize,
            codesize,
            deviceid0,
            deviceid1,
            info_part,
            info_variant,
            info_ram,
            info_flash,
            dhcsr,
            demcr,
            flash_0x0,
            flash_0x1000,
            flash_0xF4000
        );

    } else {
        ESP_LOGE(TAG, "SWD Not Connected");
        snprintf(resp, sizeof(resp),
            "{\"connected\":false,\"status\":\"Disconnected\",\"error\":\"%s\"}",
            esp_err_to_name(ret));
    }

    if (connected) {
        ESP_LOGI(TAG, "Status check complete, releasing SWD...");
        swd_shutdown();
    }

    ESP_LOGI(TAG, "=== SWD Status Check Complete ===");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

// Mass erase handler
esp_err_t mass_erase_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "=== Mass Erase Request from Web Interface ===");
    
    char resp[256];
    
    // First check SWD connection
    esp_err_t ret = ensure_swd_ready();

    // Perform mass erase (which also handles APPROTECT)
    ret = swd_flash_disable_approtect();
    
    if (ret == ESP_OK) {
        g_mass_erased = true;  // Set flag
        ESP_LOGI(TAG, "Mass erase successful, skipping page erases on next upload");
        strcpy(resp, "{\"success\":true,\"message\":\"Mass erase complete, APPROTECT disabled\"}");
    } else {
        ESP_LOGE(TAG, "Mass erase failed: %s", esp_err_to_name(ret));
        snprintf(resp, sizeof(resp),
            "{\"success\":false,\"message\":\"Mass erase failed: %s\"}",
            esp_err_to_name(ret));
    }
    
    ESP_LOGI(TAG, "Mass erase complete, releasing target...");
    swd_release_target();
    swd_shutdown();
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}


// Upload handler - saves raw binary data to storage
static esp_err_t upload_post_handler(httpd_req_t *req) {
    char filepath[] = "/storage/firmware.hex";

    ESP_LOGI(TAG, "Starting raw hex file upload");

    // Delete old file if exists
    unlink(filepath);

    // Open in BINARY WRITE mode - critical!
    FILE *f = fopen(filepath, "wb");  // "wb" not "w"
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open storage");
        return ESP_FAIL;
    }

    uint8_t buf[1024];  // Use uint8_t for binary data
    int remaining = req->content_len;
    int received = 0;

    ESP_LOGI(TAG, "Receiving %d bytes", remaining);

    // Receive and save raw bytes
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, (char*)buf, MIN(remaining, sizeof(buf)));

        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "Socket timeout");
                continue;
            }
            ESP_LOGE(TAG, "Receive failed");
            fclose(f);
            unlink(filepath);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
            return ESP_FAIL;
        }

        // Write exact bytes received
        size_t written = fwrite(buf, 1, recv_len, f);
        if (written != (size_t)recv_len) {
            ESP_LOGE(TAG, "File write failed");
            fclose(f);
            unlink(filepath);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Storage full");
            return ESP_FAIL;
        }

        received += recv_len;
        remaining -= recv_len;

        // Log progress
        if ((received % 51200) == 0 || remaining == 0) {
            int percent = (received * 100) / req->content_len;
            ESP_LOGI(TAG, "Upload progress: %d%% (%d/%d bytes)",
                    percent, received, req->content_len);
        }
    }

    fclose(f);

    // Verify file
    struct stat st;
    stat(filepath, &st);
    ESP_LOGI(TAG, "File saved: %ld bytes", st.st_size);

    // Send response
    char resp[256];
    snprintf(resp, sizeof(resp),
            "{\"status\":\"success\",\"message\":\"Upload complete\",\"size\":%ld}",
            st.st_size);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    return ESP_OK;
}

// Handler to flash firmware from stored file
static esp_err_t flash_stored_handler(httpd_req_t *req) {
    char filepath[] = "/storage/firmware.hex";

    ESP_LOGI(TAG, "Starting flash from stored file");

    // Check file exists
    struct stat st;
    if (stat(filepath, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No firmware file found");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Firmware file size: %ld bytes", st.st_size);

    // Set status
    g_flash_in_progress = true;
    g_flash_percent = 0;
    strcpy(g_flash_status, "Starting flash...");

    // Send immediate response before flashing
    httpd_resp_sendstr(req, "{\"status\":\"started\",\"message\":\"Flashing started\"}");

    // Open file
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open firmware file");
        strcpy(g_flash_status, "Failed to open file");
        g_flash_in_progress = false;
        return ESP_FAIL;
    }

    // Initialize SWD
    esp_err_t ret = ensure_swd_ready();
    if (ret != ESP_OK) {
        fclose(f);
        ESP_LOGE(TAG, "SWD not ready");
        strcpy(g_flash_status, "SWD not ready");
        g_flash_in_progress = false;
        return ESP_FAIL;
    }

    // Create upload context
    upload_context_t *ctx = calloc(1, sizeof(upload_context_t));
    ctx->page_buffer = malloc(PAGE_BUFFER_SIZE);
    memset(ctx->page_buffer, 0xFF, PAGE_BUFFER_SIZE);
    ctx->parser = hex_stream_create(hex_flash_callback, ctx);
    ctx->start_addr = 0xFFFFFFFF;

    // Read and parse file in chunks
    uint8_t read_buffer[1024];
    size_t bytes_read;
    size_t total_read = 0;

    while ((bytes_read = fread(read_buffer, 1, sizeof(read_buffer), f)) > 0) {
        // Pass raw data directly to the stream parser
        ret = hex_stream_parse(ctx->parser, read_buffer, bytes_read);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Hex parse failed at byte %zu", total_read);
            strcpy(g_flash_status, "Flash failed");
            g_flash_in_progress = false;
            break;
        }

        total_read += bytes_read;

        // Update global status
        g_flash_percent = (total_read * 100) / st.st_size;
        snprintf(g_flash_status, sizeof(g_flash_status),
                "Flashing: %d%% (%lu bytes)", g_flash_percent, ctx->flashed_bytes);

        // Progress log every 50KB
        if ((total_read % 51200) == 0 || total_read == (size_t)st.st_size) {
            ESP_LOGI(TAG, "Processing: %d%% (%zu/%ld bytes), flashed %lu bytes",
                    g_flash_percent, total_read, st.st_size, ctx->flashed_bytes);
        }

        // Yield to prevent watchdog
        vTaskDelay(1);
    }

    fclose(f);

    ESP_LOGI(TAG, "Flash complete: %lu bytes flashed from %zu byte file",
            ctx->flashed_bytes, total_read);

    // Set final status
    if (ret == ESP_OK) {
        snprintf(g_flash_status, sizeof(g_flash_status),
                "Flash complete: %lu bytes", ctx->flashed_bytes);
    }
    g_flash_in_progress = false;

    // Cleanup
    if (ctx->parser) hex_stream_free(ctx->parser);
    free(ctx->page_buffer);
    free(ctx);

    // Delete file to free storage space
    unlink(filepath);
    ESP_LOGI(TAG, "Firmware file deleted");

    return ESP_OK;
}

// Handler to check if firmware file exists
static esp_err_t check_stored_handler(httpd_req_t *req) {
    char filepath[] = "/storage/firmware.hex";
    struct stat st;

    char resp[256];
    if (stat(filepath, &st) == 0) {
        snprintf(resp, sizeof(resp),
                "{\"exists\":true,\"size\":%ld}", st.st_size);
    } else {
        strcpy(resp, "{\"exists\":false}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

// Handler to get flash status
static esp_err_t flash_status_handler(httpd_req_t *req) {
    char resp[256];
    snprintf(resp, sizeof(resp),
            "{\"in_progress\":%s,\"percent\":%d,\"status\":\"%s\"}",
            g_flash_in_progress ? "true" : "false",
            g_flash_percent,
            g_flash_status);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t progress_handler(httpd_req_t *req) {
    char resp[512];

    if (g_upload_ctx && g_upload_ctx->in_progress) {
        snprintf(resp, sizeof(resp),
                "{\"in_progress\":true,\"received\":%lu,\"flashed\":%lu,\"total\":%lu}",
                g_upload_ctx->received_bytes,
                g_upload_ctx->flashed_bytes,
                g_upload_ctx->total_bytes);
    } else if (g_upload_ctx) {
        snprintf(resp, sizeof(resp),
                "{\"in_progress\":false,\"message\":\"%s\",\"received\":%lu,\"flashed\":%lu,\"total\":%lu}",
                g_upload_ctx->status_msg,
                g_upload_ctx->received_bytes,
                g_upload_ctx->flashed_bytes,
                g_upload_ctx->total_bytes);
    } else {
        strcpy(resp, "{\"in_progress\":false,\"message\":\"Ready\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

// In components/web/src/web_upload.c, replace the existing disable_protection_handler with:

esp_err_t disable_protection_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "=== Mass Erase Request from Web Interface ===");
    
    char resp[256];
    
    // Check connection first
    if (!swd_is_connected()) {
        ESP_LOGE(TAG, "SWD not connected");
        strcpy(resp, "{\"success\":false,\"message\":\"SWD not connected\"}");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, strlen(resp));
        return ESP_OK;
    }
    
    // Perform mass erase via CTRL-AP
    esp_err_t ret = swd_flash_disable_approtect();
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✓ Mass erase successful");
        strcpy(resp, "{\"success\":true,\"message\":\"Mass erase complete. Device fully erased and unlocked.\"}");
    } else {
        ESP_LOGE(TAG, "✗ Mass erase failed: %s", esp_err_to_name(ret));
        snprintf(resp, sizeof(resp), 
                "{\"success\":false,\"message\":\"Mass erase failed: %s\"}", 
                esp_err_to_name(ret));
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

// Also in components/web/src/web_upload.c, update erase_all_handler:

esp_err_t erase_all_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "=== Full Chip Erase Request ===");
    
    char resp[256];
    
    // Check connection first
    if (!swd_is_connected()) {
        ESP_LOGE(TAG, "SWD not connected");
        strcpy(resp, "{\"success\":false,\"message\":\"SWD not connected\"}");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, strlen(resp));
        return ESP_OK;
    }
    
    // Perform mass erase via CTRL-AP
    esp_err_t ret = swd_flash_disable_approtect();
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✓ Chip erase successful");
        strcpy(resp, "{\"success\":true,\"message\":\"Chip erased successfully. All memory cleared.\"}");
    } else {
        ESP_LOGE(TAG, "✗ Chip erase failed: %s", esp_err_to_name(ret));
        snprintf(resp, sizeof(resp), 
                "{\"success\":false,\"message\":\"Chip erase failed: %s\"}", 
                esp_err_to_name(ret));
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

// Add this handler for target reset
static esp_err_t reset_target_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Target reset requested");

    char resp[128];
    esp_err_t ret = swd_flash_reset_and_run();

    if (ret == ESP_OK) {
        strcpy(resp, "{\"success\":true,\"message\":\"Target reset\"}");
    } else {
        strcpy(resp, "{\"success\":false,\"message\":\"Reset failed\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    // Clean up SWD after sending response
    swd_shutdown();

    return ESP_OK;
}

// Register all handlers
esp_err_t register_upload_handlers(httpd_handle_t server) {
    httpd_uri_t upload_uri = {
        .uri = "/upload",
        .method = HTTP_POST,
        .handler = upload_post_handler,
        .user_ctx = NULL
    };

    httpd_uri_t flash_stored_uri = {
        .uri = "/flash_stored",
        .method = HTTP_POST,
        .handler = flash_stored_handler,
        .user_ctx = NULL
    };

    httpd_uri_t check_stored_uri = {
        .uri = "/check_stored",
        .method = HTTP_GET,
        .handler = check_stored_handler,
        .user_ctx = NULL
    };

    httpd_uri_t flash_status_uri = {
        .uri = "/flash_status",
        .method = HTTP_GET,
        .handler = flash_status_handler,
        .user_ctx = NULL
    };

    httpd_uri_t progress_uri = {
        .uri = "/progress",
        .method = HTTP_GET,
        .handler = progress_handler,
        .user_ctx = NULL
    };

    httpd_uri_t check_swd_uri = {
        .uri = "/check_swd",
        .method = HTTP_GET,
        .handler = check_swd_handler,
        .user_ctx = NULL
    };

    httpd_uri_t mass_erase_uri = {
        .uri = "/mass_erase",
        .method = HTTP_GET,
        .handler = mass_erase_handler,
        .user_ctx = NULL
    };

    httpd_uri_t reset_uri = {
        .uri = "/reset_target",
        .method = HTTP_POST,
        .handler = reset_target_handler,
        .user_ctx = NULL
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &upload_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &flash_stored_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &check_stored_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &flash_status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &progress_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &check_swd_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &mass_erase_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &reset_uri));

    ESP_LOGI(TAG, "All handlers registered");
    return ESP_OK;
}