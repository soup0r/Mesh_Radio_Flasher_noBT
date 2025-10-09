#include "web_upload.h"
#include "esp_log.h"
#include "esp_system.h"
#include "hex_parser.h"
#include "swd_flash.h"
#include "swd_mem.h"
#include "swd_core.h"
#include "nrf52_hal.h"
#include "power_mgmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <sys/stat.h>
#include <stdio.h>

static const char *TAG = "WEB_UPLOAD";

#define NRF52_PAGE_SIZE 4096

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

// Flash status tracking - only g_mass_erased is actually used
static bool g_mass_erased = false;

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

// Hex record callback for live streaming upload
static void hex_flash_callback(hex_record_t *record, uint32_t abs_addr, void *ctx);

// Live upload handler - streams hex file directly to flash
static esp_err_t upload_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "=== Streaming Firmware Upload Started ===");
    ESP_LOGI(TAG, "Content length: %d bytes", req->content_len);

    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty file");
        return ESP_FAIL;
    }

    // Initialize SWD
    esp_err_t ret = ensure_swd_ready();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SWD init failed - rebooting in 2 seconds");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SWD init failed");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
        return ESP_FAIL;  // Won't reach here
    }

    // Create hex parser
    hex_stream_parser_t *parser = hex_stream_create(hex_flash_callback, NULL);
    if (!parser) {
        ESP_LOGE(TAG, "Parser creation failed - rebooting in 2 seconds");
        swd_shutdown();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Parser creation failed");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
        return ESP_FAIL;  // Won't reach here
    }

    // Receive and parse hex data in chunks
    uint8_t buf[1024];
    int remaining = req->content_len;
    int received = 0;

    ESP_LOGI(TAG, "Streaming upload in progress...");

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, (char*)buf, MIN(remaining, sizeof(buf)));

        // ANY receive error = immediate reboot (NO RETRIES, NO CONTINUE)
        if (recv_len <= 0) {
            ESP_LOGE(TAG, "❌ Upload failed: recv=%d - REBOOTING in 2 seconds", recv_len);
            hex_stream_free(parser);
            swd_shutdown();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();  // ← IMMEDIATE REBOOT
            return ESP_FAIL;  // Won't reach here
        }

        // Parse this chunk
        ret = hex_stream_parse(parser, buf, recv_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Parse error at byte %d - rebooting in 2 seconds", received);
            hex_stream_free(parser);
            swd_shutdown();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Parse error");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();  // ← IMMEDIATE REBOOT
            return ESP_FAIL;  // Won't reach here
        }

        received += recv_len;
        remaining -= recv_len;

        // Log progress every 50KB
        if ((received % 51200) == 0 || remaining == 0) {
            int percent = (received * 100) / req->content_len;
            ESP_LOGI(TAG, "Upload progress: %d%% (%d/%d bytes)",
                    percent, received, req->content_len);
        }

        // Yield to prevent watchdog
        vTaskDelay(1);
    }

    // SUCCESS PATH
    ESP_LOGI(TAG, "✓ Upload complete: %d bytes received", received);

    // Cleanup
    hex_stream_free(parser);

    // Reset and release target
    ESP_LOGI(TAG, "Resetting target...");
    swd_flash_reset_and_run();
    swd_shutdown();

    // Send success response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true,\"message\":\"Upload complete\"}",
                   HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "=== Upload Successful ===");
    return ESP_OK;
}

// Callback for flashing hex records
static void hex_flash_callback(hex_record_t *record, uint32_t abs_addr, void *ctx) {
    static uint8_t page_buffer[4096];
    static uint32_t buffer_start_addr = 0xFFFFFFFF;
    static uint32_t buffer_data_len = 0;

    switch (record->type) {
        case HEX_TYPE_DATA: {
            // Check if this data fits in current buffer
            uint32_t offset_in_buffer = abs_addr - buffer_start_addr;

            if (buffer_data_len == 0) {
                // First data - initialize buffer
                buffer_start_addr = abs_addr;
                offset_in_buffer = 0;
                memset(page_buffer, 0xFF, sizeof(page_buffer));
            } else if (abs_addr < buffer_start_addr ||
                      offset_in_buffer + record->byte_count > sizeof(page_buffer)) {
                // Data doesn't fit - flush current buffer
                if (buffer_data_len > 0) {
                    // Erase page if not mass erased
                    if (!g_mass_erased) {
                        uint32_t page_addr = buffer_start_addr & ~(NRF52_PAGE_SIZE - 1);
                        ESP_LOGI(TAG, "Erasing page 0x%08lX", page_addr);
                        swd_flash_erase_page(page_addr);
                    }

                    // Write buffer
                    ESP_LOGI(TAG, "Writing %lu bytes to 0x%08lX",
                            buffer_data_len, buffer_start_addr);
                    swd_flash_write_buffer(buffer_start_addr, page_buffer, buffer_data_len);
                }

                buffer_start_addr = abs_addr;
                offset_in_buffer = 0;
                buffer_data_len = 0;
                memset(page_buffer, 0xFF, sizeof(page_buffer));
            }

            // Copy data to buffer
            memcpy(page_buffer + offset_in_buffer, record->data, record->byte_count);

            // Update buffer length
            uint32_t new_end = offset_in_buffer + record->byte_count;
            if (new_end > buffer_data_len) {
                buffer_data_len = new_end;
            }
            break;
        }

        case HEX_TYPE_EOF:
            // Flush remaining data
            if (buffer_data_len > 0) {
                if (!g_mass_erased) {
                    uint32_t page_addr = buffer_start_addr & ~(NRF52_PAGE_SIZE - 1);
                    ESP_LOGI(TAG, "Erasing page 0x%08lX", page_addr);
                    swd_flash_erase_page(page_addr);
                }

                ESP_LOGI(TAG, "Writing final %lu bytes to 0x%08lX",
                        buffer_data_len, buffer_start_addr);
                swd_flash_write_buffer(buffer_start_addr, page_buffer, buffer_data_len);
            }

            g_mass_erased = false;  // Clear flag
            ESP_LOGI(TAG, "Flashing complete");

            // Reset buffer for next upload
            buffer_data_len = 0;
            buffer_start_addr = 0xFFFFFFFF;
            break;

        case HEX_TYPE_EXT_LIN_ADDR:
            // Flush buffer before address change
            if (buffer_data_len > 0) {
                if (!g_mass_erased) {
                    uint32_t page_addr = buffer_start_addr & ~(NRF52_PAGE_SIZE - 1);
                    swd_flash_erase_page(page_addr);
                }

                swd_flash_write_buffer(buffer_start_addr, page_buffer, buffer_data_len);
                buffer_data_len = 0;
                buffer_start_addr = 0xFFFFFFFF;
            }
            break;
    }
}

// Check SWD connection handler
esp_err_t check_swd_handler(httpd_req_t *req) {
    char resp[4096];
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

// Handler to reset target
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
        .handler = upload_handler,
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
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &check_swd_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &mass_erase_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &reset_uri));

    ESP_LOGI(TAG, "Upload handlers registered");
    return ESP_OK;
}
