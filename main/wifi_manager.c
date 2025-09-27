#include "wifi_manager.h"
#include "config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "power_mgmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "WIFI_MGR";
static EventGroupHandle_t wifi_event_group = NULL;
static const int CONNECTED_BIT = BIT0;
static wifi_state_t current_state = WIFI_STATE_DISCONNECTED;
static char current_ip[16] = "Not connected";
static bool wifi_initialized = false;
static bool is_connecting = false;  // Track if we're in connection process

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!is_connecting) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        // ALWAYS set connected bit when WiFi connects (regardless of is_connecting state)
        ESP_LOGI(TAG, "WiFi connected to AP");
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        current_state = WIFI_STATE_CONNECTED;
        // Don't clear is_connecting here - let it be cleared when we're done

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGI(TAG, "WiFi disconnected (reason: %d)", disconn->reason);

        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        current_state = WIFI_STATE_DISCONNECTED;
        strcpy(current_ip, "Not connected");

        // Only handle disconnect if we're not actively connecting
        if (!is_connecting) {
            wifi_manager_disconnect_handler();
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        snprintf(current_ip, sizeof(current_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP address: %s", current_ip);
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        current_state = WIFI_STATE_CONNECTED;
        power_reset_wake_count();
        // Clear is_connecting when we get IP
        is_connecting = false;
    }
}

static esp_err_t try_wifi_mode(const char* ssid, const char* password, bool is_lr, int timeout_sec) {
    ESP_LOGI(TAG, "Attempting %s WiFi: %s (timeout: %ds)",
             is_lr ? "ESP-LR" : "Normal", ssid, timeout_sec);

    // Clear the connected bit
    xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);

    // Stop WiFi first
    esp_wifi_stop();

    // Small delay to let things settle
    vTaskDelay(pdMS_TO_TICKS(100));

    // Configure WiFi
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "",
            .password = "",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    strcpy((char*)wifi_config.sta.ssid, ssid);
    strcpy((char*)wifi_config.sta.password, password);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Set protocol
    if (is_lr) {
        ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR));
        ESP_LOGI(TAG, "WiFi protocol set to LR (Long Range) mode");
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
            WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
        ESP_LOGI(TAG, "WiFi protocol set to 802.11 BGN mode");
    }

    // Start WiFi
    ESP_ERROR_CHECK(esp_wifi_start());

    // Small delay before connect
    vTaskDelay(pdMS_TO_TICKS(100));

    // Manually connect
    ESP_LOGI(TAG, "Calling esp_wifi_connect()...");
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    // Wait for connection OR timeout
    ESP_LOGI(TAG, "Waiting for connection...");
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
        CONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_sec * 1000));

    if (bits & CONNECTED_BIT) {
        ESP_LOGI(TAG, "✓ Connected successfully in %s mode", is_lr ? "LR" : "Normal");
        power_set_wifi_info(is_lr, ssid);

        // Give DHCP extra time to get IP
        ESP_LOGI(TAG, "Waiting for DHCP IP address...");
        vTaskDelay(pdMS_TO_TICKS(3000));

        // Check if we have an IP
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info;
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char ip_str[16];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            if (strcmp(ip_str, "0.0.0.0") != 0) {
                ESP_LOGI(TAG, "Got IP: %s", ip_str);
                strcpy(current_ip, ip_str);
            } else {
                ESP_LOGW(TAG, "Connected but no IP yet - proceeding anyway");
                strcpy(current_ip, "Waiting for IP");
            }
        }

        return ESP_OK;
    }

    ESP_LOGW(TAG, "✗ %s mode connection failed", is_lr ? "LR" : "Normal");
    return ESP_FAIL;
}

esp_err_t wifi_manager_init(void) {
    if (wifi_initialized) {
        return ESP_OK;
    }
    
    wifi_event_group = xEventGroupCreate();
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler, NULL));
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_manager_connect(void) {
    ESP_LOGI(TAG, "=== Starting WiFi Connection Sequence ===");
    current_state = WIFI_STATE_CONNECTING;
    is_connecting = true;

    // Always try LR first if enabled
    if (WIFI_LR_ENABLED) {
        ESP_LOGI(TAG, "Step 1: Trying ESP-LR connection");
        if (try_wifi_mode(WIFI_LR_SSID, WIFI_LR_PASSWORD, true,
                         WIFI_LR_CONNECT_TIMEOUT_SEC) == ESP_OK) {
            ESP_LOGI(TAG, "=== WiFi Connected (LR mode) ===");
            is_connecting = false;  // Clear flag on success
            return ESP_OK;
        }
    }

    // Fall back to normal WiFi
    ESP_LOGI(TAG, "Step 2: Trying Normal WiFi connection");
    if (try_wifi_mode(WIFI_SSID, WIFI_PASSWORD, false,
                     WIFI_CONNECT_TIMEOUT_SEC) == ESP_OK) {
        ESP_LOGI(TAG, "=== WiFi Connected (Normal mode) ===");
        is_connecting = false;  // Clear flag on success
        return ESP_OK;
    }

    ESP_LOGE(TAG, "=== All WiFi connection attempts failed ===");
    current_state = WIFI_STATE_DISCONNECTED;
    is_connecting = false;  // Clear flag on failure
    return ESP_FAIL;
}

void wifi_manager_disconnect_handler(void) {
    static int reconnect_count = 0;
    static bool handling_disconnect = false;
    
    // Prevent recursive calls
    if (handling_disconnect || is_connecting) {
        return;
    }
    
    handling_disconnect = true;
    reconnect_count++;
    
    ESP_LOGI(TAG, "Disconnect handler - reconnect attempt %d/%d", 
            reconnect_count, WIFI_RECONNECT_ATTEMPTS);
    
    if (reconnect_count <= WIFI_RECONNECT_ATTEMPTS) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // Try full connection sequence again (LR first, then normal)
        if (wifi_manager_connect() == ESP_OK) {
            reconnect_count = 0;
            handling_disconnect = false;
            return;
        }
    }
    
    // All reconnection attempts failed
    ESP_LOGW(TAG, "Max reconnect attempts reached - waiting %d seconds before deep sleep",
             WIFI_DISCONNECT_GRACE_SEC);
    
    reconnect_count = 0;
    esp_wifi_stop();
    
    vTaskDelay(pdMS_TO_TICKS(WIFI_DISCONNECT_GRACE_SEC * 1000));
    
    if (current_state != WIFI_STATE_CONNECTED) {
        ESP_LOGI(TAG, "Entering deep sleep after disconnect");
        power_enter_adaptive_deep_sleep();
        // Never returns
    }
    
    handling_disconnect = false;
}

bool wifi_manager_is_connected(void) {
    return current_state == WIFI_STATE_CONNECTED;
}

wifi_state_t wifi_manager_get_state(void) {
    return current_state;
}

const char* wifi_manager_get_ip(void) {
    return current_ip;
}

void wifi_manager_shutdown(void) {
    if (wifi_initialized) {
        esp_wifi_stop();
        wifi_initialized = false;
    }
}