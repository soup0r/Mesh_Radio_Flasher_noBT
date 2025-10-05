#ifndef TFTP_SERVER_H
#define TFTP_SERVER_H

#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    uint16_t port;
    const char* storage_path;
    size_t max_file_size;
} tftp_config_t;

typedef void (*tftp_upload_callback_t)(const char* filename, size_t size, bool success);

esp_err_t tftp_server_start(const tftp_config_t* config);
esp_err_t tftp_server_stop(void);
bool tftp_server_is_running(void);
void tftp_server_set_callback(tftp_upload_callback_t callback);

typedef struct {
    bool active;
    char filename[64];
    size_t bytes_received;
    size_t total_bytes;
    int percent;
} tftp_progress_t;

esp_err_t tftp_server_get_progress(tftp_progress_t* progress);

#endif // TFTP_SERVER_H