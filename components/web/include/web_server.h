#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"

// Register power control handlers
esp_err_t register_power_handlers(httpd_handle_t server);

#endif // WEB_SERVER_H