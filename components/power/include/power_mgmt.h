// power_mgmt.h - Power and Recovery Management
#ifndef POWER_MGMT_H
#define POWER_MGMT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Power management configuration
typedef struct {
    // Target control
    int target_power_gpio;      // GPIO for MOSFET/transistor control
    uint32_t power_on_delay_ms; // Delay after power on
    uint32_t reset_hold_ms;     // Reset pulse duration

    // Deep sleep settings
    uint32_t sleep_duration_sec;    // Deep sleep duration
    uint32_t wifi_check_interval_ms; // How often to check WiFi in active mode
    uint32_t wifi_timeout_ms;       // Max time to wait for WiFi connection
    const char *wake_ssid;          // SSID to check for wake condition

    // Watchdog settings
    uint32_t watchdog_timeout_sec;  // Hardware watchdog timeout
    bool enable_brownout_detect;    // Enable brownout detection

    // Recovery settings
    uint32_t error_cooldown_ms;     // Delay between error retries

    // 24-Hour Absolute Timer
    uint32_t absolute_reboot_interval_sec;  // Max accumulated uptime before reboot
    bool enable_absolute_timer;     // Enable/disable absolute timer
} power_config_t;

// Initialize power management
esp_err_t power_mgmt_init(const power_config_t *config);

// Target power control
esp_err_t power_target_on(void);
esp_err_t power_target_off(void);
esp_err_t power_target_reset(void);
esp_err_t power_target_cycle(uint32_t off_time_ms);
bool power_target_is_on(void);

// Prepare for deep sleep (hold GPIO state)
void power_prepare_for_sleep(void);

// Battery monitoring
typedef struct {
    float voltage;
    float voltage_min;
    float voltage_max;
    float percentage;
    bool is_charging;
    bool is_low;
    bool is_critical;
    uint32_t samples_count;
    float voltage_avg;
} battery_status_t;

esp_err_t power_battery_init(void);
esp_err_t power_get_battery_status(battery_status_t *status);
float power_get_battery_voltage_real(void);

// Enhanced adaptive deep sleep management
esp_err_t power_enter_adaptive_deep_sleep(void);
esp_err_t power_restore_from_deep_sleep(void);
uint64_t calculate_sleep_duration_us(float battery_voltage);

// Helper functions for deep sleep management
uint32_t power_get_wake_count(void);

// Wake reason
typedef enum {
    WAKE_REASON_TIMER,
    WAKE_REASON_GPIO,
    WAKE_REASON_UART,
    WAKE_REASON_RESET,
    WAKE_REASON_UNKNOWN
} wake_reason_t;

wake_reason_t power_get_wake_reason(void);

// WiFi connection info functions
void power_set_wifi_info(bool is_lr, const char* ssid);
bool power_get_wifi_is_lr(void);
const char* power_get_wifi_ssid(void);

// Absolute uptime timer (scheduled maintenance reboot)
esp_err_t power_check_absolute_timer(void);
esp_err_t power_get_absolute_timer_status(uint64_t *accumulated_sec, uint64_t *limit_sec, uint64_t *remaining_sec);

#endif // POWER_MGMT_H
