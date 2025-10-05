#include "tftp_server.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* TAG = "TFTP";

#define TFTP_OP_RRQ   1
#define TFTP_OP_WRQ   2
#define TFTP_OP_DATA  3
#define TFTP_OP_ACK   4
#define TFTP_OP_ERROR 5

#define TFTP_ERR_NOT_DEFINED      0
#define TFTP_ERR_FILE_NOT_FOUND   1
#define TFTP_ERR_ACCESS_VIOLATION 2
#define TFTP_ERR_DISK_FULL        3
#define TFTP_ERR_ILLEGAL_OP       4
#define TFTP_ERR_UNKNOWN_TID      5
#define TFTP_ERR_FILE_EXISTS      6
#define TFTP_ERR_NO_SUCH_USER     7

#define TFTP_BLOCK_SIZE   512
#define TFTP_MAX_PACKET   516
#define TFTP_TIMEOUT_SEC  5
#define TFTP_MAX_RETRIES  5

static struct {
    int sock;
    TaskHandle_t task;
    tftp_config_t config;
    tftp_upload_callback_t callback;
    tftp_progress_t progress;
    SemaphoreHandle_t progress_mutex;
    bool running;
} tftp_server = {0};

static void send_error(int sock, struct sockaddr_in* client_addr,
                      uint16_t error_code, const char* error_msg) {
    uint8_t packet[TFTP_MAX_PACKET];
    uint16_t opcode = htons(TFTP_OP_ERROR);
    uint16_t err = htons(error_code);

    memcpy(packet, &opcode, 2);
    memcpy(packet + 2, &err, 2);
    strcpy((char*)(packet + 4), error_msg);

    int len = 4 + strlen(error_msg) + 1;
    sendto(sock, packet, len, 0, (struct sockaddr*)client_addr, sizeof(*client_addr));

    ESP_LOGW(TAG, "Sent error %d: %s", error_code, error_msg);
}

static esp_err_t send_ack(int sock, struct sockaddr_in* client_addr, uint16_t block_num) {
    uint8_t packet[4];
    uint16_t opcode = htons(TFTP_OP_ACK);
    uint16_t block = htons(block_num);

    memcpy(packet, &opcode, 2);
    memcpy(packet + 2, &block, 2);

    int ret = sendto(sock, packet, 4, 0, (struct sockaddr*)client_addr, sizeof(*client_addr));
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send ACK: %d", errno);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void update_progress(const char* filename, size_t received, size_t total, bool active) {
    if (xSemaphoreTake(tftp_server.progress_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        tftp_server.progress.active = active;
        if (filename) {
            strncpy(tftp_server.progress.filename, filename,
                   sizeof(tftp_server.progress.filename) - 1);
        }
        tftp_server.progress.bytes_received = received;
        tftp_server.progress.total_bytes = total;
        if (total > 0) {
            tftp_server.progress.percent = (received * 100) / total;
        }
        xSemaphoreGive(tftp_server.progress_mutex);
    }
}

static void handle_write_request(struct sockaddr_in* client_addr,
                                uint8_t* buffer, int recv_len) {
    char filename[128] = {0};
    char mode[16] = {0};

    int offset = 2;
    strncpy(filename, (char*)(buffer + offset), sizeof(filename) - 1);
    offset += strlen(filename) + 1;
    strncpy(mode, (char*)(buffer + offset), sizeof(mode) - 1);

    ESP_LOGI(TAG, "Write request: %s (mode: %s) from %s:%d",
            filename, mode,
            inet_ntoa(client_addr->sin_addr),
            ntohs(client_addr->sin_port));

    if (strcasecmp(mode, "octet") != 0) {
        send_error(tftp_server.sock, client_addr, TFTP_ERR_ILLEGAL_OP,
                  "Only octet mode supported");
        return;
    }

    const char* filepath = "/storage/firmware.hex";

    ESP_LOGI(TAG, "Client sent: %s", filename);
    ESP_LOGI(TAG, "Saving as: %s (overwrites any existing file)", filepath);

    unlink(filepath);

    FILE* f = fopen(filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", filepath);
        send_error(tftp_server.sock, client_addr, TFTP_ERR_ACCESS_VIOLATION,
                  "Cannot create file");
        return;
    }

    ESP_LOGI(TAG, "File opened successfully");
    update_progress(filename, 0, 0, true);

    // === FIX: Make socket BLOCKING during transfer ===
    // Save original flags
    int orig_flags = fcntl(tftp_server.sock, F_GETFL, 0);

    // Clear O_NONBLOCK flag (make socket blocking)
    fcntl(tftp_server.sock, F_SETFL, orig_flags & ~O_NONBLOCK);
    ESP_LOGI(TAG, "Socket set to BLOCKING mode for transfer");
    // =================================================

    send_ack(tftp_server.sock, client_addr, 0);

    uint16_t expected_block = 1;
    size_t total_received = 0;
    bool transfer_complete = false;
    int consecutive_timeouts = 0;

    // Set receive timeout (now this will actually work because socket is blocking)
    struct timeval tv;
    tv.tv_sec = TFTP_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(tftp_server.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (!transfer_complete && consecutive_timeouts < TFTP_MAX_RETRIES) {
        uint8_t packet[TFTP_MAX_PACKET];
        struct sockaddr_in sender_addr;
        socklen_t sender_len = sizeof(sender_addr);

        int len = recvfrom(tftp_server.sock, packet, sizeof(packet), 0,
                          (struct sockaddr*)&sender_addr, &sender_len);

        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Real timeout (not immediate return)
                ESP_LOGW(TAG, "Timeout waiting for block %d, resending ACK %d",
                        expected_block, expected_block - 1);
                send_ack(tftp_server.sock, client_addr, expected_block - 1);
                consecutive_timeouts++;
                continue;
            } else {
                ESP_LOGE(TAG, "Receive error: %d", errno);
                break;
            }
        }

        consecutive_timeouts = 0;

        if (sender_addr.sin_addr.s_addr != client_addr->sin_addr.s_addr ||
            sender_addr.sin_port != client_addr->sin_port) {
            send_error(tftp_server.sock, &sender_addr, TFTP_ERR_UNKNOWN_TID,
                      "Unknown transfer ID");
            continue;
        }

        uint16_t opcode = ntohs(*(uint16_t*)packet);

        if (opcode != TFTP_OP_DATA) {
            ESP_LOGW(TAG, "Expected DATA, got opcode %d", opcode);
            continue;
        }

        uint16_t block_num = ntohs(*(uint16_t*)(packet + 2));

        if (block_num < expected_block) {
            ESP_LOGD(TAG, "Duplicate block %d (expected %d), resending ACK",
                    block_num, expected_block);
            send_ack(tftp_server.sock, client_addr, block_num);
            continue;
        }

        if (block_num > expected_block) {
            ESP_LOGE(TAG, "Out of order block %d (expected %d)",
                    block_num, expected_block);
            send_error(tftp_server.sock, client_addr, TFTP_ERR_NOT_DEFINED,
                      "Out of order block");
            break;
        }

        int data_len = len - 4;
        if (data_len > 0) {
            size_t written = fwrite(packet + 4, 1, data_len, f);
            if (written != (size_t)data_len) {
                ESP_LOGE(TAG, "Write error: wrote %d of %d bytes", written, data_len);
                send_error(tftp_server.sock, client_addr, TFTP_ERR_DISK_FULL,
                          "Disk full");
                break;
            }
            total_received += written;

            update_progress(filename, total_received, 0, true);

            if ((total_received % 10240) == 0) {
                ESP_LOGI(TAG, "Received %d KB", total_received / 1024);
            }
        }

        send_ack(tftp_server.sock, client_addr, block_num);

        if (data_len < TFTP_BLOCK_SIZE) {
            transfer_complete = true;
            ESP_LOGI(TAG, "Transfer complete: %d bytes", total_received);
        }

        expected_block++;

        if (total_received > tftp_server.config.max_file_size) {
            ESP_LOGE(TAG, "File too large: %d bytes", total_received);
            send_error(tftp_server.sock, client_addr, TFTP_ERR_DISK_FULL,
                      "File too large");
            break;
        }

        vTaskDelay(1);
    }

    fclose(f);

    // === FIX: Restore original socket flags ===
    fcntl(tftp_server.sock, F_SETFL, orig_flags);
    ESP_LOGI(TAG, "Socket restored to NON-BLOCKING mode");
    // ==========================================

    // Clear timeout
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(tftp_server.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (transfer_complete) {
        ESP_LOGI(TAG, "✓ Upload successful: %s (%d bytes)", filepath, total_received);
        update_progress(filename, total_received, total_received, false);

        if (tftp_server.callback) {
            tftp_server.callback(filepath, total_received, true);
        }
    } else {
        ESP_LOGE(TAG, "✗ Upload failed");
        unlink(filepath);
        update_progress(filename, total_received, total_received, false);

        if (tftp_server.callback) {
            tftp_server.callback(filepath, total_received, false);
        }
    }
}

static void tftp_server_task(void* arg) {
    ESP_LOGI(TAG, "TFTP server task started on port %d", tftp_server.config.port);

    uint8_t buffer[TFTP_MAX_PACKET];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (tftp_server.running) {
        int recv_len = recvfrom(tftp_server.sock, buffer, sizeof(buffer), 0,
                               (struct sockaddr*)&client_addr, &client_len);

        if (recv_len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            ESP_LOGE(TAG, "Receive error: %d", errno);
            continue;
        }

        if (recv_len < 4) {
            ESP_LOGW(TAG, "Packet too short: %d bytes", recv_len);
            continue;
        }

        uint16_t opcode = ntohs(*(uint16_t*)buffer);

        ESP_LOGD(TAG, "Received opcode %d from %s:%d",
                opcode,
                inet_ntoa(client_addr.sin_addr),
                ntohs(client_addr.sin_port));

        switch (opcode) {
            case TFTP_OP_WRQ:
                handle_write_request(&client_addr, buffer, recv_len);
                break;

            case TFTP_OP_RRQ:
                send_error(tftp_server.sock, &client_addr, TFTP_ERR_ILLEGAL_OP,
                          "Read not supported");
                break;

            default:
                ESP_LOGW(TAG, "Unknown opcode: %d", opcode);
                send_error(tftp_server.sock, &client_addr, TFTP_ERR_ILLEGAL_OP,
                          "Unknown operation");
                break;
        }

        vTaskDelay(1);
    }

    ESP_LOGI(TAG, "TFTP server task ending");
    vTaskDelete(NULL);
}

esp_err_t tftp_server_start(const tftp_config_t* config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (tftp_server.running) {
        ESP_LOGW(TAG, "TFTP server already running");
        return ESP_OK;
    }

    tftp_server.config = *config;

    tftp_server.progress_mutex = xSemaphoreCreateMutex();
    if (!tftp_server.progress_mutex) {
        return ESP_ERR_NO_MEM;
    }

    tftp_server.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (tftp_server.sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: %d", errno);
        vSemaphoreDelete(tftp_server.progress_mutex);
        return ESP_FAIL;
    }

    int flags = fcntl(tftp_server.sock, F_GETFL, 0);
    fcntl(tftp_server.sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(config->port)
    };

    if (bind(tftp_server.sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind to port %d: %d", config->port, errno);
        close(tftp_server.sock);
        vSemaphoreDelete(tftp_server.progress_mutex);
        return ESP_FAIL;
    }

    tftp_server.running = true;
    BaseType_t ret = xTaskCreate(tftp_server_task, "tftp_server", 8192, NULL, 5,
                                 &tftp_server.task);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TFTP server task");
        close(tftp_server.sock);
        vSemaphoreDelete(tftp_server.progress_mutex);
        tftp_server.running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "✓ TFTP server started on port %d", config->port);
    ESP_LOGI(TAG, "  Storage path: %s", config->storage_path);
    ESP_LOGI(TAG, "  Max file size: %d bytes", config->max_file_size);

    return ESP_OK;
}

esp_err_t tftp_server_stop(void) {
    if (!tftp_server.running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping TFTP server...");

    tftp_server.running = false;

    vTaskDelay(pdMS_TO_TICKS(100));

    if (tftp_server.sock >= 0) {
        close(tftp_server.sock);
        tftp_server.sock = -1;
    }

    if (tftp_server.progress_mutex) {
        vSemaphoreDelete(tftp_server.progress_mutex);
        tftp_server.progress_mutex = NULL;
    }

    ESP_LOGI(TAG, "TFTP server stopped");
    return ESP_OK;
}

bool tftp_server_is_running(void) {
    return tftp_server.running;
}

void tftp_server_set_callback(tftp_upload_callback_t callback) {
    tftp_server.callback = callback;
}

esp_err_t tftp_server_get_progress(tftp_progress_t* progress) {
    if (!progress) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(tftp_server.progress_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *progress = tftp_server.progress;
        xSemaphoreGive(tftp_server.progress_mutex);
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}