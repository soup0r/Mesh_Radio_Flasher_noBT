#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED
} wifi_state_t;

// WiFi manager initialization and control
esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_connect(void);
void wifi_manager_disconnect_handler(void);
bool wifi_manager_is_connected(void);
const char* wifi_manager_get_ip(void);

// External function from main.c for web server cleanup during disconnect
// Note: This creates an explicit dependency on main.c's stop_webserver()
// Used to cleanly shut down the web server before entering deep sleep
extern void stop_webserver(void);

#endif