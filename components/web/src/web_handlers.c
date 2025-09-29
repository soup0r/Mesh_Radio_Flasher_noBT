#include "web_server.h"
#include "esp_log.h"
#include "power_mgmt.h"
#include "cJSON.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "WEB_HANDLERS";

// Power status handler
static esp_err_t power_status_handler(httpd_req_t *req) {
    // Use the internal state function
    bool is_powered = power_target_is_on();

    ESP_LOGI(TAG, "Power status request: %s (internal state)", is_powered ? "ON" : "OFF");

    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    cJSON_AddBoolToObject(json, "powered", is_powered);
    cJSON_AddStringToObject(json, "status", is_powered ? "ON" : "OFF");

    char *json_string = cJSON_Print(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_string, strlen(json_string));

    free(json_string);
    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t battery_status_handler(httpd_req_t *req) {
    battery_status_t battery;
    esp_err_t ret = power_get_battery_status(&battery);
    cJSON *json = cJSON_CreateObject();
    if (ret == ESP_OK) {
        cJSON_AddBoolToObject(json, "success", true);
        cJSON_AddNumberToObject(json, "voltage", battery.voltage);
        cJSON_AddNumberToObject(json, "voltage_min", battery.voltage_min);
        cJSON_AddNumberToObject(json, "voltage_max", battery.voltage_max);
        cJSON_AddNumberToObject(json, "percentage", battery.percentage);
        cJSON_AddNumberToObject(json, "voltage_avg", battery.voltage_avg);
        cJSON_AddBoolToObject(json, "is_charging", battery.is_charging);
        cJSON_AddBoolToObject(json, "is_low", battery.is_low);
        cJSON_AddBoolToObject(json, "is_critical", battery.is_critical);
        cJSON_AddNumberToObject(json, "samples", battery.samples_count);
        const char *status_text = "Normal";
        if (battery.is_critical) status_text = "Critical";
        else if (battery.is_low) status_text = "Low";
        else if (battery.is_charging) status_text = "Charging";
        else if (battery.percentage > 90) status_text = "Full";
        cJSON_AddStringToObject(json, "status_text", status_text);
        ESP_LOGI(TAG, "Battery status: %.2fV (%.0f%%) %s",
                battery.voltage, battery.percentage, status_text);
    } else {
        cJSON_AddBoolToObject(json, "success", false);
        cJSON_AddStringToObject(json, "error", "Battery monitoring not available");
    }
    char *json_string = cJSON_Print(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_string, strlen(json_string));
    free(json_string);
    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t wifi_status_handler(httpd_req_t *req) {
    cJSON *json = cJSON_CreateObject();

    // Get WiFi connection status
    wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);

    if (ret == ESP_OK) {
        cJSON_AddBoolToObject(json, "connected", true);

        // SSID
        char ssid[33] = {0};
        memcpy(ssid, ap_info.ssid, 32);
        cJSON_AddStringToObject(json, "ssid", ssid);

        // BSSID (MAC address of AP)
        char bssid_str[18];
        snprintf(bssid_str, sizeof(bssid_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                ap_info.bssid[0], ap_info.bssid[1], ap_info.bssid[2],
                ap_info.bssid[3], ap_info.bssid[4], ap_info.bssid[5]);
        cJSON_AddStringToObject(json, "bssid", bssid_str);

        // Channel
        cJSON_AddNumberToObject(json, "channel", ap_info.primary);

        // RSSI (Received Signal Strength Indicator)
        cJSON_AddNumberToObject(json, "rssi", ap_info.rssi);

        // Signal quality percentage (rough estimate)
        int quality = 0;
        if (ap_info.rssi >= -50) quality = 100;
        else if (ap_info.rssi >= -60) quality = 90;
        else if (ap_info.rssi >= -70) quality = 75;
        else if (ap_info.rssi >= -80) quality = 50;
        else if (ap_info.rssi >= -90) quality = 25;
        else quality = 10;
        cJSON_AddNumberToObject(json, "quality", quality);

        // Connection type (LR or Normal)
        bool is_lr = power_get_wifi_is_lr();
        cJSON_AddStringToObject(json, "mode", is_lr ? "ESP-LR (Long Range)" : "Normal WiFi");
        cJSON_AddBoolToObject(json, "is_lr", is_lr);

        // PHY mode
        wifi_phy_mode_t phy_mode;
        esp_wifi_sta_get_negotiated_phymode(&phy_mode);
        const char* phy_str = "Unknown";
        switch(phy_mode) {
            case WIFI_PHY_MODE_11B: phy_str = "802.11b"; break;
            case WIFI_PHY_MODE_11G: phy_str = "802.11g"; break;
            case WIFI_PHY_MODE_HT20: phy_str = "802.11n (HT20)"; break;
            case WIFI_PHY_MODE_HT40: phy_str = "802.11n (HT40)"; break;
            case WIFI_PHY_MODE_LR: phy_str = "LR (Long Range)"; break;
            default: break;
        }
        cJSON_AddStringToObject(json, "phy_mode", phy_str);

        // Get additional statistics
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);

        // Station MAC address
        char sta_mac[18];
        snprintf(sta_mac, sizeof(sta_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(json, "sta_mac", sta_mac);

        // Get IP configuration
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                char ip_str[16], gw_str[16], mask_str[16];
                esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
                esp_ip4addr_ntoa(&ip_info.gw, gw_str, sizeof(gw_str));
                esp_ip4addr_ntoa(&ip_info.netmask, mask_str, sizeof(mask_str));

                cJSON_AddStringToObject(json, "ip", ip_str);
                cJSON_AddStringToObject(json, "gateway", gw_str);
                cJSON_AddStringToObject(json, "netmask", mask_str);
            }

            // DNS servers
            esp_netif_dns_info_t dns_info;
            if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK) {
                char dns_str[16];
                esp_ip4addr_ntoa(&dns_info.ip.u_addr.ip4, dns_str, sizeof(dns_str));
                cJSON_AddStringToObject(json, "dns", dns_str);
            }
        }

        // Connection time (approximate based on system uptime)
        cJSON_AddNumberToObject(json, "uptime_ms", esp_timer_get_time() / 1000);

        ESP_LOGI(TAG, "WiFi Status: SSID=%s, RSSI=%d, Mode=%s",
                ssid, ap_info.rssi, is_lr ? "LR" : "Normal");

    } else {
        cJSON_AddBoolToObject(json, "connected", false);
        cJSON_AddStringToObject(json, "error", "Not connected to WiFi");
        ESP_LOGI(TAG, "WiFi Status: Not connected");
    }

    char *json_string = cJSON_Print(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_string, strlen(json_string));

    free(json_string);
    cJSON_Delete(json);
    return ESP_OK;
}

// Power control handlers
static esp_err_t power_on_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "=== Power ON request received ===");

    esp_err_t ret = power_target_on();

    // Always check state after operation using internal state
    bool is_on = power_target_is_on();
    ESP_LOGI(TAG, "State after operation: %s", is_on ? "ON" : "OFF");

    cJSON *json = cJSON_CreateObject();
    if (ret == ESP_OK) {
        cJSON_AddBoolToObject(json, "success", true);
        cJSON_AddStringToObject(json, "message", "Power turned on");
        cJSON_AddBoolToObject(json, "powered", true);  // Add this
        ESP_LOGI(TAG, "Power ON successful");
    } else {
        cJSON_AddBoolToObject(json, "success", false);
        cJSON_AddStringToObject(json, "message", "Failed to turn on power");
        cJSON_AddBoolToObject(json, "powered", is_on);
        ESP_LOGE(TAG, "Power ON failed!");
    }

    char *json_string = cJSON_Print(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_string, strlen(json_string));

    free(json_string);
    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t power_off_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "=== Power OFF request received ===");

    esp_err_t ret = power_target_off();

    // Always check state after operation using internal state
    bool is_on = power_target_is_on();
    ESP_LOGI(TAG, "State after operation: %s", is_on ? "ON" : "OFF");

    cJSON *json = cJSON_CreateObject();
    if (ret == ESP_OK) {
        cJSON_AddBoolToObject(json, "success", true);
        cJSON_AddStringToObject(json, "message", "Power turned off");
        cJSON_AddBoolToObject(json, "powered", false);  // Add this
        ESP_LOGI(TAG, "Power OFF successful");
    } else {
        cJSON_AddBoolToObject(json, "success", false);
        cJSON_AddStringToObject(json, "message", "Failed to turn off power");
        cJSON_AddBoolToObject(json, "powered", is_on);
        ESP_LOGE(TAG, "Power OFF failed!");
    }

    char *json_string = cJSON_Print(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_string, strlen(json_string));

    free(json_string);
    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t power_reboot_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Power reboot request");

    esp_err_t ret = power_target_reset();

    cJSON *json = cJSON_CreateObject();
    if (ret == ESP_OK) {
        cJSON_AddBoolToObject(json, "success", true);
        cJSON_AddStringToObject(json, "message", "Reboot cycle started");
    } else {
        cJSON_AddBoolToObject(json, "success", false);
        cJSON_AddStringToObject(json, "message", "Failed to reboot");
    }

    char *json_string = cJSON_Print(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_string, strlen(json_string));

    free(json_string);
    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t power_cycle_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Power cycle request");

    // Parse query parameter for time
    char query[64] = {0};
    uint32_t off_time = 15000; // Default 15 seconds

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(query, "time", param, sizeof(param)) == ESP_OK) {
            off_time = (uint32_t)atoi(param);
            // Clamp to reasonable limits
            if (off_time < 100) off_time = 100;
            if (off_time > 60000) off_time = 60000;
        }
    }

    ESP_LOGI(TAG, "Power cycling for %lu ms", off_time);
    esp_err_t ret = power_target_cycle(off_time);

    cJSON *json = cJSON_CreateObject();
    if (ret == ESP_OK) {
        cJSON_AddBoolToObject(json, "success", true);
        cJSON_AddStringToObject(json, "message", "Power cycle complete");
        cJSON_AddNumberToObject(json, "off_time_ms", off_time);
    } else {
        cJSON_AddBoolToObject(json, "success", false);
        cJSON_AddStringToObject(json, "message", "Failed to cycle power");
    }

    char *json_string = cJSON_Print(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_string, strlen(json_string));

    free(json_string);
    cJSON_Delete(json);
    return ESP_OK;
}


esp_err_t register_power_handlers(httpd_handle_t server) {
    httpd_uri_t power_status_uri = {
        .uri = "/power_status",
        .method = HTTP_GET,
        .handler = power_status_handler,
        .user_ctx = NULL
    };

    httpd_uri_t power_on_uri = {
        .uri = "/power_on",
        .method = HTTP_POST,
        .handler = power_on_handler,
        .user_ctx = NULL
    };

    httpd_uri_t power_off_uri = {
        .uri = "/power_off",
        .method = HTTP_POST,
        .handler = power_off_handler,
        .user_ctx = NULL
    };

    httpd_uri_t power_reboot_uri = {
        .uri = "/power_reboot",
        .method = HTTP_POST,
        .handler = power_reboot_handler,
        .user_ctx = NULL
    };

    httpd_uri_t power_cycle_uri = {
        .uri = "/power_cycle",
        .method = HTTP_POST,
        .handler = power_cycle_handler,
        .user_ctx = NULL
    };

    httpd_uri_t battery_status_uri = {
        .uri = "/battery_status",
        .method = HTTP_GET,
        .handler = battery_status_handler,
        .user_ctx = NULL
    };

    httpd_uri_t wifi_status_uri = {
        .uri = "/wifi_status",
        .method = HTTP_GET,
        .handler = wifi_status_handler,
        .user_ctx = NULL
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &battery_status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_status_uri));

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &power_status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &power_on_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &power_off_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &power_reboot_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &power_cycle_uri));

    ESP_LOGI(TAG, "Power control and WiFi status handlers registered");
    return ESP_OK;
}