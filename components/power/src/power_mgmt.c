#include "power_mgmt.h"
#include "config.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_err.h"  // Explicitly include for esp_err_to_name()
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_task_wdt.h"

static const char *TAG = "POWER_MGMT";
static power_config_t power_config = {0};
static system_health_t health_stats = {0};
static bool power_state = true;
static adc_oneshot_unit_handle_t adc1_handle = NULL;
static adc_cali_handle_t adc_cali_handle = NULL;
static battery_status_t battery_stats = {0};
static bool battery_monitoring_enabled = false;

// RTC memory variables (persist through deep sleep on ESP32-C3)
RTC_DATA_ATTR static uint32_t rtc_wake_count = 0;
RTC_DATA_ATTR static bool rtc_nrf_power_state = true;
RTC_DATA_ATTR static float rtc_last_battery_voltage = 0.0f;
RTC_DATA_ATTR static uint64_t rtc_nrf_off_total_ms = 0;
RTC_DATA_ATTR static uint64_t rtc_last_sleep_us = 0;
RTC_DATA_ATTR static bool rtc_nrf_power_off_active = false;

// 24-hour absolute timer - survives deep sleep and tracks total uptime
RTC_DATA_ATTR static uint64_t rtc_first_boot_timestamp = 0;
RTC_DATA_ATTR static uint64_t rtc_total_uptime_sec = 0;
RTC_DATA_ATTR static uint64_t rtc_last_wake_timestamp = 0;
RTC_DATA_ATTR static bool rtc_timer_initialized = false;

// Regular variables for WiFi/sleep management
static uint32_t wake_count = 0;

// WiFi connection tracking
static bool wifi_is_lr_mode = false;
static char wifi_current_ssid[33] = {0};  // Max SSID length is 32 + null terminator

#define VOLTAGE_DIVIDER_RATIO 2.0f
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_2
#define BATTERY_MAX_VOLTAGE 4.2f
#define BATTERY_MIN_VOLTAGE 3.0f
#define BATTERY_CRITICAL_VOLTAGE 3.2f
#define BATTERY_LOW_VOLTAGE 3.5f
#define ADC_SAMPLES_COUNT 10

// Hardware watchdog timeout (must be fed regularly or device reboots)
#define HARDWARE_WATCHDOG_TIMEOUT_SEC 60  // 60 seconds

esp_err_t power_battery_init(void) {
    ESP_LOGI(TAG, "Initializing battery monitoring on GPIO2");
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_config1, &adc1_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init ADC unit: %s", esp_err_to_name(ret));
        return ret;
    }
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    ret = adc_oneshot_config_channel(adc1_handle, BATTERY_ADC_CHANNEL, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to config ADC channel: %s", esp_err_to_name(ret));
        return ret;
    }
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ADC calibration enabled");
    } else {
        ESP_LOGW(TAG, "ADC calibration not supported, using raw values");
    }
    battery_monitoring_enabled = true;
    battery_stats.voltage_min = 999.0f;
    battery_stats.voltage_max = 0.0f;
    battery_stats.samples_count = 0;
    battery_stats.voltage_avg = 0.0f;
    battery_status_t status;
    power_get_battery_status(&status);
    ESP_LOGI(TAG, "Battery monitoring initialized. Initial voltage: %.2fV", status.voltage);
    return ESP_OK;
}

float power_get_battery_voltage_real(void) {
    if (!battery_monitoring_enabled || !adc1_handle) {
        return 0.0f;
    }
    int adc_reading = 0;
    int voltage_mv = 0;
    float total_voltage = 0.0f;
    for (int i = 0; i < ADC_SAMPLES_COUNT; i++) {
        esp_err_t ret = adc_oneshot_read(adc1_handle, BATTERY_ADC_CHANNEL, &adc_reading);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read ADC");
            continue;
        }
        if (adc_cali_handle) {
            adc_cali_raw_to_voltage(adc_cali_handle, adc_reading, &voltage_mv);
            total_voltage += (voltage_mv / 1000.0f);
        } else {
            float voltage = (adc_reading / 4095.0f) * 3.3f;
            total_voltage += voltage;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    float adc_voltage = total_voltage / ADC_SAMPLES_COUNT;
    float battery_voltage = adc_voltage * VOLTAGE_DIVIDER_RATIO;
    return battery_voltage;
}

esp_err_t power_get_battery_status(battery_status_t *status) {
    if (!status || !battery_monitoring_enabled) {
        return ESP_ERR_INVALID_ARG;
    }
    float current_voltage = power_get_battery_voltage_real();
    battery_stats.voltage = current_voltage;
    battery_stats.samples_count++;
    if (current_voltage > 2.0f && current_voltage < battery_stats.voltage_min) {
        battery_stats.voltage_min = current_voltage;
    }
    if (current_voltage > battery_stats.voltage_max) {
        battery_stats.voltage_max = current_voltage;
    }
    if (battery_stats.samples_count == 1) {
        battery_stats.voltage_avg = current_voltage;
    } else {
        battery_stats.voltage_avg = (battery_stats.voltage_avg * 0.95f) + (current_voltage * 0.05f);
    }
    if (current_voltage >= BATTERY_MAX_VOLTAGE) {
        battery_stats.percentage = 100.0f;
    } else if (current_voltage <= BATTERY_MIN_VOLTAGE) {
        battery_stats.percentage = 0.0f;
    } else {
        battery_stats.percentage = ((current_voltage - BATTERY_MIN_VOLTAGE) /
                                   (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE)) * 100.0f;
    }
    battery_stats.is_charging = (current_voltage > 4.1f);
    battery_stats.is_critical = (current_voltage < BATTERY_CRITICAL_VOLTAGE);
    battery_stats.is_low = (current_voltage < BATTERY_LOW_VOLTAGE);
    *status = battery_stats;
    ESP_LOGD(TAG, "Battery: %.2fV (%.0f%%) Min:%.2fV Max:%.2fV Avg:%.2fV %s%s",
            status->voltage, status->percentage,
            status->voltage_min, status->voltage_max, status->voltage_avg,
            status->is_charging ? "[CHARGING]" : "",
            status->is_critical ? "[CRITICAL]" : (status->is_low ? "[LOW]" : ""));
    return ESP_OK;
}

esp_err_t power_mgmt_init(const power_config_t *config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    power_config = *config;

    // Check if we're waking from deep sleep
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    bool waking_from_sleep = (wake_cause != ESP_SLEEP_WAKEUP_UNDEFINED);

    if (power_config.target_power_gpio >= 0) {
        gpio_num_t gpio = (gpio_num_t)power_config.target_power_gpio;

        // IMPORTANT: Make sure any hold is disabled first
        gpio_hold_dis(gpio);
        gpio_deep_sleep_hold_dis();

        // Now configure the GPIO
        gpio_reset_pin(gpio);
        gpio_set_direction(gpio, GPIO_MODE_INPUT_OUTPUT);
        gpio_set_pull_mode(gpio, GPIO_FLOATING);  // No pull resistors

        if (waking_from_sleep) {
            // Restore state from RTC memory
            ESP_LOGI(TAG, "Waking from deep sleep, restoring power state: %s",
                    rtc_nrf_power_state ? "ON" : "OFF");
            power_state = rtc_nrf_power_state;
            gpio_set_level(gpio, power_state ? 0 : 1);
        } else {
            // Fresh boot - default to ON
            gpio_set_level(gpio, 0);  // 0 = ON
            power_state = true;
            rtc_nrf_power_state = true;
            ESP_LOGI(TAG, "Fresh boot, setting power ON");
        }

        // Verify the GPIO is actually set
        int actual_state = gpio_get_level(gpio);
        ESP_LOGI(TAG, "Power control GPIO%d initialized: actual GPIO level = %d (%s)",
                power_config.target_power_gpio, actual_state,
                actual_state == 0 ? "ON" : "OFF");
    }

    power_battery_init();
    ESP_LOGI(TAG, "Power management initialized");
    return ESP_OK;
}

esp_err_t power_target_on(void) {
    if (power_config.target_power_gpio >= 0) {
        gpio_num_t gpio = (gpio_num_t)power_config.target_power_gpio;

        // Make absolutely sure hold is disabled
        gpio_hold_dis(gpio);

        // Set direction and level
        gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
        gpio_set_level(gpio, 0);  // 0 = ON
        power_state = true;
        rtc_nrf_power_state = true;  // Update RTC variable too

        // Give it more time to settle
        vTaskDelay(pdMS_TO_TICKS(50));  // Increased from 10ms

        // Verify it actually changed
        int actual = gpio_get_level(gpio);
        ESP_LOGI(TAG, "Target power ON (GPIO%d set to LOW), actual GPIO level = %d",
                power_config.target_power_gpio, actual);

        if (actual != 0) {
            ESP_LOGW(TAG, "GPIO readback different than expected, but operation completed");
        }

        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t power_target_off(void) {
    if (power_config.target_power_gpio >= 0) {
        gpio_num_t gpio = (gpio_num_t)power_config.target_power_gpio;

        // Make absolutely sure hold is disabled
        gpio_hold_dis(gpio);

        // Set direction and level
        gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
        gpio_set_level(gpio, 1);  // 1 = OFF
        power_state = false;
        rtc_nrf_power_state = false;  // Update RTC variable too

        // Give it more time to settle
        vTaskDelay(pdMS_TO_TICKS(50));  // Increased from 10ms

        // Verify it actually changed
        int actual = gpio_get_level(gpio);
        ESP_LOGI(TAG, "Target power OFF (GPIO%d set to HIGH), actual GPIO level = %d",
                power_config.target_power_gpio, actual);

        if (actual != 1) {
            ESP_LOGW(TAG, "GPIO readback different than expected, but operation completed");
        }

        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t power_target_reset(void) {
    if (power_config.target_power_gpio < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "Target reset: turning OFF for 15 seconds, then ON");

    // Turn off
    esp_err_t ret = power_target_off();
    if (ret != ESP_OK) {
        return ret;
    }

    // Wait 15 seconds
    vTaskDelay(pdMS_TO_TICKS(15000));

    // Turn back on
    ret = power_target_on();
    if (ret != ESP_OK) {
        return ret;
    }

    // Wait for power on delay
    vTaskDelay(pdMS_TO_TICKS(power_config.power_on_delay_ms));

    ESP_LOGI(TAG, "Target reset complete");
    return ESP_OK;
}

esp_err_t power_target_cycle(uint32_t off_time_ms) {
    if (power_config.target_power_gpio < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "Power cycling target (off for %lu ms)", (unsigned long)off_time_ms);
    power_target_off();
    vTaskDelay(pdMS_TO_TICKS(off_time_ms));
    power_target_on();
    vTaskDelay(pdMS_TO_TICKS(power_config.power_on_delay_ms));
    ESP_LOGI(TAG, "Power cycle complete");
    return ESP_OK;
}

bool power_target_is_on(void) {
    // Trust the internal state variable instead of reading GPIO
    // (GPIO readback might be unreliable with certain hardware configurations)
    return power_state;
}

void power_prepare_for_sleep(void) {
    if (power_config.target_power_gpio >= 0) {
        power_state = power_target_is_on();
        ESP_LOGI(TAG, "Preparing for deep sleep, power state: %s",
                power_state ? "ON" : "OFF");
    }
}

void power_restore_after_sleep(void) {
    if (power_config.target_power_gpio >= 0) {
        ESP_LOGI(TAG, "Restored after deep sleep, power state: %s",
                power_state ? "ON" : "OFF");
    }
}

void power_watchdog_feed(void) {}

void power_get_health_status(system_health_t *health) {
    if (health) {
        *health = health_stats;
        health_stats.uptime_seconds++;
    }
}

wake_reason_t power_get_wake_reason(void) {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch(cause) {
        case ESP_SLEEP_WAKEUP_TIMER:
            return WAKE_REASON_TIMER;
        case ESP_SLEEP_WAKEUP_GPIO:
            return WAKE_REASON_GPIO;
        case ESP_SLEEP_WAKEUP_UART:
            return WAKE_REASON_UART;
        default:
            return WAKE_REASON_RESET;
    }
}


// WiFi connection info functions
void power_set_wifi_info(bool is_lr, const char* ssid) {
    wifi_is_lr_mode = is_lr;
    if (ssid) {
        strncpy(wifi_current_ssid, ssid, sizeof(wifi_current_ssid) - 1);
        wifi_current_ssid[sizeof(wifi_current_ssid) - 1] = '\0';
    }
}

bool power_get_wifi_is_lr(void) {
    return wifi_is_lr_mode;
}

const char* power_get_wifi_ssid(void) {
    return wifi_current_ssid;
}


// Function to calculate deep sleep duration based on battery level
uint64_t calculate_sleep_duration_us(float battery_voltage) {
    uint32_t sleep_seconds;

    if (battery_voltage < BATTERY_CRITICAL_THRESHOLD) {
        sleep_seconds = DEEP_SLEEP_CRITICAL_SEC;
        ESP_LOGW(TAG, "Critical battery (%.2fV) - sleeping for %d seconds",
                battery_voltage, DEEP_SLEEP_CRITICAL_SEC);
    } else if (battery_voltage < BATTERY_MEDIUM_HIGH_THRESHOLD) {
        sleep_seconds = DEEP_SLEEP_LOW_BATTERY_SEC;
        ESP_LOGI(TAG, "Low battery (%.2fV) - sleeping for %d seconds",
                battery_voltage, DEEP_SLEEP_LOW_BATTERY_SEC);
    } else if (battery_voltage < BATTERY_HIGH_THRESHOLD) {
        sleep_seconds = DEEP_SLEEP_MEDIUM_BATTERY_SEC;
        ESP_LOGI(TAG, "Medium battery (%.2fV) - sleeping for %d seconds",
                battery_voltage, DEEP_SLEEP_MEDIUM_BATTERY_SEC);
    } else {
        sleep_seconds = DEEP_SLEEP_HIGH_BATTERY_SEC;
        ESP_LOGI(TAG, "High battery (%.2fV) - sleeping for %d seconds",
                battery_voltage, DEEP_SLEEP_HIGH_BATTERY_SEC);
    }

    return sleep_seconds * 1000000ULL;
}

// =============================================================================
// 24-HOUR ABSOLUTE TIMER IMPLEMENTATION
// =============================================================================

/**
 * @brief Check and enforce 24-hour absolute reboot timer
 *
 * This function MUST be called at every boot/wake before any other operations.
 * It tracks total uptime across sleep/wake cycles using RTC memory.
 * After 24 hours of accumulated uptime, it forces a reboot.
 *
 * This is the ultimate safety mechanism - ensures device reboots every 24 hours
 * regardless of WiFi status, sleep cycles, or any other conditions.
 *
 * @return ESP_OK if within time limit, never returns if time exceeded (reboots)
 */
esp_err_t power_check_absolute_timer(void) {
    // Check if timer is enabled in config
    if (!power_config.enable_absolute_timer) {
        ESP_LOGI(TAG, "24-hour absolute timer disabled in config");
        return ESP_OK;
    }

    uint64_t current_time = esp_timer_get_time() / 1000000ULL;  // Uptime in seconds

    // First boot ever (or RTC memory was lost/corrupted)
    if (!rtc_timer_initialized) {
        ESP_LOGW(TAG, "╔════════════════════════════════════════════════════════════╗");
        ESP_LOGW(TAG, "║        24-HOUR ABSOLUTE TIMER INITIALIZED                  ║");
        ESP_LOGW(TAG, "║   Device will force reboot after %lu hours total uptime   ║",
                power_config.absolute_reboot_interval_sec / 3600);
        ESP_LOGW(TAG, "╚════════════════════════════════════════════════════════════╝");

        rtc_first_boot_timestamp = current_time;
        rtc_total_uptime_sec = 0;
        rtc_last_wake_timestamp = current_time;
        rtc_timer_initialized = true;

        ESP_LOGI(TAG, "Absolute timer: Starting new %lu-hour cycle",
                power_config.absolute_reboot_interval_sec / 3600);
        return ESP_OK;
    }

    // Subsequent boot/wake - accumulate uptime
    // This handles cases where device sleeps and wakes multiple times
    uint64_t session_duration = current_time - rtc_last_wake_timestamp;
    rtc_total_uptime_sec += session_duration;
    rtc_last_wake_timestamp = current_time;

    uint64_t remaining_sec = power_config.absolute_reboot_interval_sec - rtc_total_uptime_sec;
    uint64_t remaining_hours = remaining_sec / 3600;

    ESP_LOGI(TAG, "Absolute timer: Total uptime %llu/%lu sec (%llu hours remaining)",
            rtc_total_uptime_sec,
            power_config.absolute_reboot_interval_sec,
            remaining_hours);

    // Check if timer expired - FORCE REBOOT
    if (rtc_total_uptime_sec >= power_config.absolute_reboot_interval_sec) {
        ESP_LOGW(TAG, "");
        ESP_LOGW(TAG, "╔════════════════════════════════════════════════════════════╗");
        ESP_LOGW(TAG, "║     24-HOUR ABSOLUTE TIMER EXPIRED - FORCING REBOOT        ║");
        ESP_LOGW(TAG, "╚════════════════════════════════════════════════════════════╝");
        ESP_LOGW(TAG, "Total uptime: %llu seconds (%.1f hours)",
                rtc_total_uptime_sec, rtc_total_uptime_sec / 3600.0);
        ESP_LOGW(TAG, "Configured limit: %lu seconds (%.1f hours)",
                power_config.absolute_reboot_interval_sec,
                power_config.absolute_reboot_interval_sec / 3600.0);
        ESP_LOGW(TAG, "This is a safety mechanism - device will reboot now");

        // Get final battery status before reboot
        battery_status_t battery;
        if (power_get_battery_status(&battery) == ESP_OK) {
            ESP_LOGI(TAG, "Final battery: %.2fV (%.0f%%)", battery.voltage, battery.percentage);
        }

        // Reset timer for next cycle
        rtc_timer_initialized = false;
        rtc_total_uptime_sec = 0;

        // Prepare GPIO states for reboot
        power_prepare_for_sleep();
        vTaskDelay(pdMS_TO_TICKS(1000));  // Allow logs to flush

        ESP_LOGW(TAG, "=== ABSOLUTE TIMER REBOOT NOW ===");
        vTaskDelay(pdMS_TO_TICKS(100));

        // Force restart - execution never returns from here
        esp_restart();
    }

    return ESP_OK;
}

/**
 * @brief Get status of 24-hour absolute timer for monitoring/web interface
 *
 * @param[out] uptime_sec Total accumulated uptime in seconds
 * @param[out] remaining_sec Seconds remaining until forced reboot
 */
void power_get_absolute_timer_status(uint64_t *uptime_sec, uint64_t *remaining_sec) {
    if (uptime_sec) {
        *uptime_sec = rtc_total_uptime_sec;
    }

    if (remaining_sec) {
        if (rtc_total_uptime_sec < power_config.absolute_reboot_interval_sec) {
            *remaining_sec = power_config.absolute_reboot_interval_sec - rtc_total_uptime_sec;
        } else {
            *remaining_sec = 0;
        }
    }
}

/**
 * @brief Reset the 24-hour timer (for testing or maintenance mode)
 *
 * WARNING: Use only for testing. In production, let timer run normally.
 */
void power_reset_absolute_timer(void) {
    ESP_LOGW(TAG, "Manually resetting 24-hour absolute timer");
    rtc_timer_initialized = false;
    rtc_total_uptime_sec = 0;
    rtc_first_boot_timestamp = 0;
    rtc_last_wake_timestamp = 0;
}

// =============================================================================
// HARDWARE WATCHDOG IMPLEMENTATION
// =============================================================================

/**
 * @brief Initialize hardware watchdog timer
 *
 * The hardware watchdog must be fed every 60 seconds or the device will reboot.
 * This catches complete hangs where the entire system becomes unresponsive.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t power_init_hardware_watchdog(void) {
    ESP_LOGI(TAG, "Initializing hardware watchdog (%d second timeout)",
            HARDWARE_WATCHDOG_TIMEOUT_SEC);

    // Configure watchdog
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = HARDWARE_WATCHDOG_TIMEOUT_SEC * 1000,
        .idle_core_mask = 0,        // Don't watch idle task
        .trigger_panic = false      // Just reboot, don't panic
    };

    esp_err_t ret = esp_task_wdt_init(&wdt_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init hardware watchdog: %s", esp_err_to_name(ret));
        return ret;
    }

    // Subscribe current task (main task)
    ret = esp_task_wdt_add(NULL);  // NULL = current task
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add task to watchdog: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGW(TAG, "⚠️  Hardware watchdog active - must feed every %d seconds or device reboots",
            HARDWARE_WATCHDOG_TIMEOUT_SEC);

    return ESP_OK;
}

/**
 * @brief Feed the hardware watchdog
 *
 * This function MUST be called regularly (at least every 60 seconds)
 * to prevent the hardware watchdog from triggering a reboot.
 *
 * Call this in your main monitoring loop.
 */
void power_feed_watchdog(void) {
    esp_task_wdt_reset();
}

// Enhanced deep sleep function for ESP32-C3 with proper timing
esp_err_t power_enter_adaptive_deep_sleep(void) {
    if (!ENABLE_DEEP_SLEEP_POWER_MGMT) {
        ESP_LOGI(TAG, "Deep sleep disabled in config");
        return ESP_OK;
    }

    // Get current battery voltage
    battery_status_t battery;
    power_get_battery_status(&battery);
    float voltage = battery.voltage;

    // Increment for next wake with overflow protection
    wake_count++;
    if (wake_count > 1000) {  // Arbitrary safety limit
        ESP_LOGW(TAG, "Wake count overflow protection - resetting to 0");
        wake_count = 0;
    }

    ESP_LOGI(TAG, "Entering deep sleep (this will be wake #%lu)", wake_count);

    // Store values in RTC memory
    rtc_last_battery_voltage = voltage;
    rtc_wake_count = wake_count;

    // Calculate sleep duration
    uint64_t sleep_duration_us = calculate_sleep_duration_us(voltage);

    // Store sleep duration for accurate timing after wake
    rtc_last_sleep_us = sleep_duration_us;

    // Handle critical voltage - power off nRF52
    if (voltage < NRF52_POWER_OFF_VOLTAGE && ENABLE_BATTERY_PROTECTION) {
        ESP_LOGW(TAG, "Battery voltage %.2fV below threshold %.2fV - powering off nRF52",
                voltage, NRF52_POWER_OFF_VOLTAGE);
        power_target_off();
        rtc_nrf_power_state = false;

        // Mark that we're starting/continuing power-off period
        if (!rtc_nrf_power_off_active) {
            rtc_nrf_power_off_active = true;
            rtc_nrf_off_total_ms = 0;  // Reset the timer
            ESP_LOGI(TAG, "Starting nRF52 power-off period");
        }
    } else {
        // Store current power state in RTC memory
        rtc_nrf_power_state = power_state;
    }

    // Configure GPIO to maintain state during deep sleep
    if (power_config.target_power_gpio >= 0) {
        gpio_num_t power_gpio = (gpio_num_t)power_config.target_power_gpio;

        ESP_LOGI(TAG, "Configuring GPIO%d to hold state %s during deep sleep",
                power_gpio, power_state ? "ON (LOW)" : "OFF (HIGH)");

        // DON'T reset the pin - just ensure it's configured correctly
        // gpio_reset_pin() would momentarily make it an input!

        // Make sure direction is output (if not already)
        gpio_set_direction(power_gpio, GPIO_MODE_OUTPUT);

        // Set the desired level
        gpio_set_level(power_gpio, power_state ? 0 : 1);

        // Enable hold on this GPIO
        esp_err_t ret = gpio_hold_en(power_gpio);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable hold on GPIO%d: %s",
                    power_gpio, esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Hold enabled on GPIO%d", power_gpio);
        }
    }

    // Enable deep sleep hold for all held GPIOs
    gpio_deep_sleep_hold_en();

    ESP_LOGI(TAG, "Entering deep sleep for %llu seconds (wake count: %lu)",
            sleep_duration_us / 1000000ULL, wake_count);
    ESP_LOGI(TAG, "DEBUG: Timer microseconds = %llu", sleep_duration_us);

    if (rtc_nrf_power_off_active) {
        ESP_LOGI(TAG, "nRF52 has been off for %llu ms total", rtc_nrf_off_total_ms);
    }

    // Configure wake up timer
    esp_sleep_enable_timer_wakeup(sleep_duration_us);
    ESP_LOGI(TAG, "DEBUG: Timer configured, entering sleep NOW");

    // Enter deep sleep
    esp_deep_sleep_start();

    // Never reaches here
    return ESP_OK;
}

// Function to restore state after wake from deep sleep (with proper glitch prevention)
esp_err_t power_restore_from_deep_sleep(void) {
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();

    if (wake_cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
        // Fresh boot, not waking from sleep
        ESP_LOGI(TAG, "Fresh boot detected - initializing RTC variables");
        rtc_wake_count = 0;
        rtc_nrf_power_state = true;
        rtc_last_battery_voltage = 0.0f;
        rtc_nrf_off_total_ms = 0;
        rtc_last_sleep_us = 0;
        rtc_nrf_power_off_active = false;
        return ESP_OK;
    }

    uint64_t actual_wake_time_us = esp_timer_get_time();

    ESP_LOGI(TAG, "=== Wake from Deep Sleep ===");
    ESP_LOGI(TAG, "Wake cause: %d", wake_cause);
    ESP_LOGI(TAG, "Wake count: %lu", rtc_wake_count);
    ESP_LOGI(TAG, "Last battery: %.2fV", rtc_last_battery_voltage);
    ESP_LOGI(TAG, "Expected sleep: %llu seconds", rtc_last_sleep_us / 1000000ULL);
    ESP_LOGI(TAG, "Actual wake time: %llu us since boot", actual_wake_time_us);
    ESP_LOGI(TAG, "DEBUG: Configured sleep was %llu us", rtc_last_sleep_us);
    ESP_LOGI(TAG, "NRF52 state in RTC: %s", rtc_nrf_power_state ? "ON" : "OFF");

    // Update accumulated off time if nRF52 was powered off
    if (rtc_nrf_power_off_active) {
        rtc_nrf_off_total_ms += rtc_last_sleep_us / 1000ULL;
        ESP_LOGI(TAG, "nRF52 total off time: %llu ms (%llu hours)",
                rtc_nrf_off_total_ms, rtc_nrf_off_total_ms / 3600000ULL);
    }

    // Restore wake count from RTC memory
    wake_count = rtc_wake_count;

    // IMPORTANT: Configure GPIO BEFORE releasing hold to prevent glitches
    if (power_config.target_power_gpio >= 0) {
        gpio_num_t power_gpio = (gpio_num_t)power_config.target_power_gpio;

        // Restore power state from RTC memory
        power_state = rtc_nrf_power_state;

        // Configure GPIO while hold is still active (prevents glitch)
        gpio_set_direction(power_gpio, GPIO_MODE_OUTPUT);
        gpio_set_level(power_gpio, power_state ? 0 : 1);

        // NOW disable hold - pin will maintain the level we just set
        esp_err_t ret = gpio_hold_dis(power_gpio);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to disable hold on GPIO%d: %s",
                    power_gpio, esp_err_to_name(ret));
        }

        ESP_LOGI(TAG, "Restored GPIO%d to %s after deep sleep (glitch-free)",
                power_gpio, power_state ? "ON (LOW)" : "OFF (HIGH)");
    }

    // Disable global deep sleep hold after individual pins are handled
    gpio_deep_sleep_hold_dis();

    // Check battery and manage nRF52 power
    battery_status_t battery;
    power_get_battery_status(&battery);

    ESP_LOGI(TAG, "Current battery: %.2fV, NRF52 state: %s, Protection active: %s",
            battery.voltage,
            power_state ? "ON" : "OFF",
            rtc_nrf_power_off_active ? "YES" : "NO");

    // =========================================================================
    // NRF52 Power Management Logic
    // All thresholds configured in config.h
    // =========================================================================

    // CASE 1: Battery is LOW - turn OFF or keep OFF
    if (battery.voltage < NRF52_POWER_OFF_VOLTAGE && ENABLE_BATTERY_PROTECTION) {

        if (power_state) {
            // Currently ON but battery is low - turn OFF
            ESP_LOGW(TAG, "Battery low (%.2fV < %.2fV) - turning nRF52 OFF",
                    battery.voltage, NRF52_POWER_OFF_VOLTAGE);
            power_target_off();
            rtc_nrf_power_state = false;

            // Start power-off protection period
            if (!rtc_nrf_power_off_active) {
                rtc_nrf_power_off_active = true;
                rtc_nrf_off_total_ms = 0;
                ESP_LOGI(TAG, "Starting nRF52 power-off protection period");
            }
        } else {
            // Already OFF - keep it off
            ESP_LOGI(TAG, "Battery still low (%.2fV) - keeping nRF52 OFF (off for %llu ms)",
                    battery.voltage, rtc_nrf_off_total_ms);
        }
    }

    // CASE 2: Battery has RECOVERED - decide whether to turn ON
    else if (rtc_nrf_power_off_active && !power_state) {
        // We're in protection mode and NRF52 is OFF

        float turn_on_threshold = NRF52_POWER_OFF_VOLTAGE + NRF52_POWER_ON_HYSTERESIS;
        uint32_t min_off_time_ms = NRF52_MIN_OFF_TIME_SEC * 1000UL;
        uint32_t max_off_time_ms = NRF52_MAX_OFF_TIME_SEC * 1000UL;

        bool voltage_good = (battery.voltage >= turn_on_threshold);
        bool min_time_elapsed = (rtc_nrf_off_total_ms >= min_off_time_ms);
        bool max_time_exceeded = (rtc_nrf_off_total_ms >= max_off_time_ms);

        ESP_LOGI(TAG, "NRF52 Recovery Check:");
        ESP_LOGI(TAG, "  Battery: %.2fV (need %.2fV) %s",
                battery.voltage, turn_on_threshold, voltage_good ? "✓" : "✗");
        ESP_LOGI(TAG, "  Min time: %llu/%lu ms %s",
                rtc_nrf_off_total_ms, min_off_time_ms, min_time_elapsed ? "✓" : "✗");
        ESP_LOGI(TAG, "  Max time: %llu/%lu ms %s",
                rtc_nrf_off_total_ms, max_off_time_ms, max_time_exceeded ? "(exceeded)" : "");

        // Turn ON if:
        // (Battery is good AND minimum time elapsed) OR maximum time exceeded
        if ((voltage_good && min_time_elapsed) || max_time_exceeded) {

            if (max_time_exceeded && !voltage_good) {
                ESP_LOGW(TAG, "Maximum off time (%lu sec) exceeded - attempting turn-on despite marginal voltage",
                        NRF52_MAX_OFF_TIME_SEC);
            }

            ESP_LOGI(TAG, "✓ Conditions met - turning nRF52 ON");
            power_target_on();
            rtc_nrf_power_state = true;

            // Exit protection mode
            rtc_nrf_power_off_active = false;
            rtc_nrf_off_total_ms = 0;

            ESP_LOGI(TAG, "nRF52 power protection period ended");

        } else {
            // Not ready to turn on yet
            ESP_LOGI(TAG, "Keeping nRF52 OFF - conditions not met");

            if (!voltage_good) {
                ESP_LOGI(TAG, "  Waiting for battery: %.2fV → %.2fV (need +%.2fV)",
                        battery.voltage, turn_on_threshold,
                        turn_on_threshold - battery.voltage);
            }
            if (!min_time_elapsed) {
                uint32_t remaining_ms = min_off_time_ms - rtc_nrf_off_total_ms;
                ESP_LOGI(TAG, "  Waiting for minimum time: %lu seconds remaining",
                        remaining_ms / 1000);
            }
        }
    }

    // CASE 3: Battery is GOOD and not in protection mode
    else if (battery.voltage >= NRF52_POWER_OFF_VOLTAGE && !power_state) {
        // Battery is good but NRF52 is off (not in protection mode)
        // This can happen after a manual power off or other edge cases
        ESP_LOGI(TAG, "Battery good (%.2fV) and NRF52 is OFF - turning ON",
                battery.voltage);
        power_target_on();
        rtc_nrf_power_state = true;
    }

    // CASE 4: Everything is normal
    else if (power_state) {
        ESP_LOGI(TAG, "NRF52 ON, battery sufficient (%.2fV)", battery.voltage);
    }

    ESP_LOGI(TAG, "=== Wake Restoration Complete ===");
    return ESP_OK;
}


// Helper functions
uint32_t power_get_wake_count(void) {
    return wake_count;
}

void power_reset_wake_count(void) {
    wake_count = 0;
    rtc_wake_count = 0;
}