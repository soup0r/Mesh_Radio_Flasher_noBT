#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED
} wifi_state_t;

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_connect(void);
void wifi_manager_disconnect_handler(void);
bool wifi_manager_is_connected(void);
wifi_state_t wifi_manager_get_state(void);
const char* wifi_manager_get_ip(void);
void wifi_manager_shutdown(void);

#endif