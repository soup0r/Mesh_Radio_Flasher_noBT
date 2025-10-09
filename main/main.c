// main.c - Resilient Field Flasher Main Application
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "nrf52_hal.h"
#include "config.h"
#include "esp_http_server.h"
#include "web_upload.h"
#include "web_server.h"
#include "esp_spiffs.h"

// Custom modules
#include "swd_core.h"
#include "swd_mem.h"
#include "swd_flash.h"
#include "power_mgmt.h"
#include "wifi_manager.h"


static const char *TAG = "FLASHER";

// Global variables
static char* device_ip = "Not connected";
static httpd_handle_t web_server = NULL;

// Failsafe reboot timer (CRITICAL: ensures device returns to sleep/wake cycle)
static TaskHandle_t failsafe_task_handle = NULL;
static bool failsafe_armed = false;
static uint32_t failsafe_start_time = 0;

// Brownout loop protection (RTC memory - persists through resets and deep sleep)
RTC_DATA_ATTR static uint32_t rtc_brownout_count = 0;
RTC_DATA_ATTR static uint64_t rtc_last_brownout_time = 0;

// Wake state machine types
typedef enum {
    WAKE_STATE_INIT,
    WAKE_STATE_BATTERY_CHECK,
    WAKE_STATE_NRF52_DECISION,
    WAKE_STATE_WIFI_SCAN,
    WAKE_STATE_ACTIVE,
    WAKE_STATE_SLEEP
} wake_state_t;

typedef struct {
    wake_state_t state;
    battery_status_t battery;
    bool nrf52_should_be_on;
    bool wifi_connected;
    uint32_t wake_count;
} wake_context_t;

// Function declarations
static void init_system(void);
static void init_storage(void);
static void system_health_task(void *arg);
static esp_err_t start_webserver(void);
void stop_webserver(void);  // Made global for wifi_manager cleanup
static esp_err_t release_swd_handler(httpd_req_t *req);

// Initialize SPIFFS storage partition
static void init_storage(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 2,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS (%s)", esp_err_to_name(ret));
        return;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "Storage partition: %d bytes total, %d bytes used", total, used);
}

// Enhanced web server handler with new tabbed interface
static esp_err_t root_handler(httpd_req_t *req) {
    // Part 1: HTML header and styles
    const char* html_start =
        "<!DOCTYPE html><html><head><title>Mesh Radio Flasher</title>"
        "<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>"
        "<style>"
        "*{margin:0;padding:0;box-sizing:border-box;}"
        "body{font-family:Arial;background:#f5f5f5;color:#333;}"
        ".container{max-width:1200px;margin:0 auto;padding:20px;}"
        "header{text-align:center;margin-bottom:30px;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);"
        "color:white;padding:20px;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.1);}"
        "h1{font-size:2.5em;margin-bottom:10px;}.subtitle{font-size:1.2em;opacity:0.9;}"
        ".tabs{display:flex;background:white;border-radius:10px 10px 0 0;box-shadow:0 2px 4px rgba(0,0,0,0.1);overflow:hidden;}"
        ".tab{flex:1;padding:15px 20px;background:#e9ecef;border:none;cursor:pointer;font-size:16px;font-weight:500;"
        "transition:background-color 0.3s,color 0.3s;border-right:1px solid #dee2e6;}"
        ".tab:last-child{border-right:none;}.tab.active{background:white;color:#667eea;border-bottom:3px solid #667eea;}"
        ".tab:hover:not(.active){background:#f8f9fa;}"
        ".tab-content{background:white;border-radius:0 0 10px 10px;padding:30px;box-shadow:0 2px 4px rgba(0,0,0,0.1);min-height:500px;}"
        ".tab-pane{display:none;}.tab-pane.active{display:block;}"
        ".info-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:20px;margin:20px 0;}"
        ".info-card{background:#f8f9fa;padding:20px;border-radius:8px;border-left:4px solid #667eea;}"
        ".info-card h3{color:#667eea;margin-bottom:15px;font-size:1.3em;}"
        ".info-item{display:flex;justify-content:space-between;margin:8px 0;padding:5px 0;border-bottom:1px solid #e9ecef;}"
        ".info-item:last-child{border-bottom:none;}.info-label{font-weight:500;color:#495057;}"
        ".info-value{color:#6c757d;font-family:monospace;}"
        ".btn{background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);color:white;border:none;padding:12px 24px;"
        "border-radius:6px;cursor:pointer;font-size:16px;margin:5px;transition:transform 0.2s,box-shadow 0.2s;}"
        ".btn:hover{transform:translateY(-2px);box-shadow:0 4px 8px rgba(0,0,0,0.2);}"
        ".btn-danger{background:linear-gradient(135deg,#ff6b6b 0%,#ee5a52 100%);}"
        ".btn-warning{background:linear-gradient(135deg,#ffa726 0%,#fb8c00 100%);}"
        ".btn-success{background:linear-gradient(135deg,#66bb6a 0%,#43a047 100%);}"
        ".btn:disabled{background:#ccc;cursor:not-allowed;transform:none;}"
        ".progress-bar{width:100%;height:30px;background:#eee;border-radius:5px;overflow:hidden;}"
        ".progress-fill{height:100%;background:#4CAF50;transition:width 0.3s;}"
        ".warning{color:#f44336;font-weight:bold;}"
        ".status-indicator{display:inline-block;width:12px;height:12px;border-radius:50%;margin-right:8px;}"
        ".status-online{background-color:#28a745;}.status-offline{background-color:#dc3545;}"
        ".status-unknown{background-color:#ffc107;}.status-warning{background-color:#fd7e14;}"
        "@media (max-width:768px){.tabs{flex-direction:column;}.tab{border-right:none;border-bottom:1px solid #dee2e6;}"
        ".tab:last-child{border-bottom:none;}.info-grid{grid-template-columns:1fr;}.container{padding:10px;}}"
        "</style></head><body><div class='container'>"
        "<header><h1>Mesh Radio Flasher</h1><p class='subtitle'>Wireless Development & Power Management Interface</p></header>"
        "<div class='tabs'>"
        "<button class='tab active' onclick='openTab(event,\"home\")'>Home</button>"
        "<button class='tab' onclick='openTab(event,\"power-control\")'>Power Control</button>"
        "<button class='tab' onclick='openTab(event,\"flashing\")'>Flashing</button>"
        "</div><div class='tab-content'>";

    httpd_resp_send_chunk(req, html_start, strlen(html_start));

    // Get current device IP
    char ip_str[16] = "Not connected";
    if (strlen(device_ip) > 0) {
        strcpy(ip_str, device_ip);
    }

    // Part 2: Home tab with all status fields
    char home_content[4096];  // Increased size for WiFi status card
    snprintf(home_content, sizeof(home_content),
        "<div id='home' class='tab-pane active'>"
        "<script>"
        "setTimeout(function() {"
        "  if (window.refreshStatus) refreshStatus();"
        "  if (window.checkPowerStatus) checkPowerStatus();"
        "  if (window.updateBatteryStatus) updateBatteryStatus();"
        "  if (window.updateWiFiStatus) updateWiFiStatus();"
        "}, 1000);"
        "</script>"
        "<h2>System Overview</h2>"
        "<div class='info-grid'>"
        "<div class='info-card'>"
        "<h3>ESP32 Status</h3>"
        "<div class='info-item'><span class='info-label'>Status:</span><span class='info-value'><span class='status-indicator status-online'></span>Online</span></div>"
        "<div class='info-item'><span class='info-label'>Device IP:</span><span class='info-value' id='device-ip'>%s</span></div>"
        "<div class='info-item'><span class='info-label'>Free Heap:</span><span class='info-value' id='free-heap'>%lu bytes</span></div>"
        "</div>"
        "<div class='info-card'>"
        "<h3>🔋 Battery Status</h3>"
        "<div class='info-item'><span class='info-label'>Voltage:</span><span class='info-value' id='battery-voltage'>Checking...</span></div>"
        "<div class='info-item'><span class='info-label'>Percentage:</span><span class='info-value' id='battery-percentage'>Checking...</span></div>"
        "<div class='info-item'><span class='info-label'>Status:</span><span class='info-value' id='battery-status'>Checking...</span></div>"
        "<div class='info-item'><span class='info-label'>Min/Max:</span><span class='info-value' id='battery-range'>Checking...</span></div>"
        "<div class='info-item'><span class='info-label'>Average:</span><span class='info-value' id='battery-avg'>Checking...</span></div>"
        "</div>"
        "<div class='info-card'>"
        "<h3>📶 WiFi Status</h3>"
        "<div class='info-item'><span class='info-label'>Connection:</span><span class='info-value' id='wifi-mode'>Checking...</span></div>"
        "<div class='info-item'><span class='info-label'>SSID:</span><span class='info-value' id='wifi-ssid'>Checking...</span></div>"
        "<div class='info-item'><span class='info-label'>Signal:</span><span class='info-value' id='wifi-rssi'>Checking...</span></div>"
        "<div class='info-item'><span class='info-label'>Quality:</span><span class='info-value' id='wifi-quality'>Checking...</span></div>"
        "<div class='info-item'><span class='info-label'>Channel:</span><span class='info-value' id='wifi-channel'>Checking...</span></div>"
        "<div class='info-item'><span class='info-label'>PHY Mode:</span><span class='info-value' id='wifi-phy'>Checking...</span></div>"
        "<div class='info-item'><span class='info-label'>Gateway:</span><span class='info-value' id='wifi-gateway'>Checking...</span></div>"
        "</div>"
        "<div class='info-card'>"
        "<h3>Target Radio</h3>"
        "<div class='info-item'><span class='info-label'>SWD Status:</span><span class='info-value' id='swd-status'>Checking...</span></div>"
        "<div class='info-item'><span class='info-label'>APPROTECT:</span><span class='info-value' id='approtect-status'>Checking...</span></div>"
        "<div class='info-item'><span class='info-label'>Device ID:</span><span class='info-value' id='device-id'>Checking...</span></div>"
        "<div class='info-item'><span class='info-label'>Flash Size:</span><span class='info-value' id='flash-size'>Checking...</span></div>"
        "<div class='info-item'><span class='info-label'>RAM Size:</span><span class='info-value' id='ram-size'>Checking...</span></div>"
        "<div class='info-item'><span class='info-label'>Core State:</span><span class='info-value' id='core-state'>Checking...</span></div>"
        "<div class='info-item'><span class='info-label'>NVMC State:</span><span class='info-value' id='nvmc-state'>Checking...</span></div>"
        "<div class='info-item'><span class='info-label'>Last Check:</span><span class='info-value' id='last-check'>Never</span></div>"
        "</div>"
        "</div>"
        "<div class='info-card' style='margin-top:20px;'>"
        "<h3>Quick Actions</h3>"
        "<button class='btn' onclick='refreshStatus()'>Refresh Status</button>"
        "<button class='btn' onclick='openTab(event,\"flashing\")'>Start Flashing</button>"
        "<button class='btn' onclick='openTab(event,\"power-control\")'>Power Control</button>"
        "</div>"
        "</div>",
        ip_str,
        esp_get_free_heap_size()
    );

    httpd_resp_send_chunk(req, home_content, strlen(home_content));

    // Part 3: Other tabs
    const char* other_tabs =

        "<div id='power-control' class='tab-pane'>"
        "<h2>⚡ Power Control</h2>"
        "<div class='info-card'>"
        "<h3>🔋 Power Source</h3>"
        "<div id='batteryDetails' style='margin-bottom:15px;'>"
        "<div style='display:flex;justify-content:space-between;margin:10px 0;'>"
        "<span>Voltage:</span><span id='power-battery-voltage' style='font-weight:bold;'>--</span>"
        "</div>"
        "<div style='display:flex;justify-content:space-between;margin:10px 0;'>"
        "<span>Charge:</span><span id='power-battery-percent' style='font-weight:bold;'>--</span>"
        "</div>"
        "<div style='display:flex;justify-content:space-between;margin:10px 0;'>"
        "<span>Status:</span><span id='power-battery-status' style='font-weight:bold;'>--</span>"
        "</div>"
        "<div style='background:#f0f0f0;height:30px;border-radius:15px;overflow:hidden;margin:15px 0;'>"
        "<div id='battery-bar' style='height:100%;background:linear-gradient(90deg,#28a745,#66bb6a);width:0%;transition:width 0.5s;'></div>"
        "</div>"
        "</div>"
        "</div>"
        "<div class='info-card'>"
        "<h3>Target Power Management</h3>"
        "<p>Control power to the target mesh radio device.</p>"
        "<div style='margin:20px 0;'>"
        "<div id='powerStatus' style='margin-bottom:15px;font-weight:bold;'>"
        "<span class='status-indicator status-unknown'></span>Power Status: Unknown"
        "</div>"
        "<button class='btn btn-success' onclick='powerOn()'>Power On</button>"
        "<button class='btn btn-danger' onclick='powerOff()'>Power Off</button>"
        "<button class='btn btn-warning' onclick='powerReboot()'>Reboot (15s)</button>"
        "</div>"
        "<div id='powerOperationStatus' style='margin-top:15px;padding:10px;background:#f8f9fa;border-radius:5px;'>"
        "Ready for operations"
        "</div>"
        "</div>"
        "</div>"

        "<div id='flashing' class='tab-pane'>"
        "<h2>📱 Mesh Radio Flashing</h2>"
        "<div class='info-card'>"
        "<h3>SWD Debug Status</h3>"
        "<div style='margin-bottom:10px;'>"
        "<button class='btn' onclick='checkSWD()'>Check SWD Status</button>"
        "<button class='btn' onclick='releaseSWD()'>Release Target</button>"
        "</div>"
        "<div id='protStatus' style='margin:10px 0;font-weight:bold;'>Click 'Check SWD Status' to begin</div>"
        "<pre id='swdRegisterDump' style='font-family:monospace;font-size:12px;background:#f5f5f5;padding:10px;border-radius:5px;max-height:400px;overflow-y:auto;'>"
        "Register dump will appear here..."
        "</pre>"
        "</div>"
        "<div class='info-card'>"
        "<h3>Flash Operations</h3>"
        "<p class='warning'>⚠️ Warning: Mass erase will DELETE ALL DATA!</p>"
        "<button class='btn btn-danger' onclick='massErase()'>Mass Erase & Disable APPROTECT</button>"
        "</div>"
        "<div class='info-card'>"
        "<h3>Firmware Upload</h3>"
        "<p style='color:#6c757d;margin-bottom:15px;'>Select a .hex file to upload and flash. The hex file contains all address information.</p>"
        "<input type='file' id='hexFile' accept='.hex' style='margin-bottom:10px;'/><br>"
        "<button id='uploadBtn' class='btn' onclick='uploadFirmware()'>Upload & Flash</button>"
        "<div style='margin-top:20px;'>"
        "<div class='progress-bar'><div id='progressBar' class='progress-fill' style='width:0%;'></div></div>"
        "<div id='status' style='margin-top:10px;font-weight:500;'>Ready</div>"
        "</div>"
        "</div>"
        "</div>";

    httpd_resp_send_chunk(req, other_tabs, strlen(other_tabs));

    // Part 4: JavaScript - Split into chunks to avoid memory issues
    const char* js_start =
        "</div></div>"
        "<script>"
        "let progressTimer = null;"
        ""
        "function openTab(evt, tabName) {"
        "  var i, tabcontent, tabs;"
        "  tabcontent = document.getElementsByClassName('tab-pane');"
        "  for (i = 0; i < tabcontent.length; i++) {"
        "    tabcontent[i].classList.remove('active');"
        "  }"
        "  tabs = document.getElementsByClassName('tab');"
        "  for (i = 0; i < tabs.length; i++) {"
        "    tabs[i].classList.remove('active');"
        "  }"
        "  document.getElementById(tabName).classList.add('active');"
        "  evt.currentTarget.classList.add('active');"
        "  if (tabName === 'home') {"
        "    refreshStatus();"
        "    checkPowerStatus();"
        "    updateBatteryStatus();"
        "    updateWiFiStatus();"
        "  } else if (tabName === 'power-control') {"
        "    checkPowerStatus();"
        "    updateBatteryStatus();"
        "  }"
        "}"
        ""
        "function refreshStatus() {"
        "  fetch('/check_swd')"
        "    .then(response => response.json())"
        "    .then(data => {"
        "      updateHomeStatus(data);"
        "    })"
        "    .catch(error => {"
        "      console.error('Error fetching status:', error);"
        "      document.getElementById('swd-status').innerHTML = '<span style=\"color:red;\">Connection Error</span>';"
        "      document.getElementById('last-check').textContent = 'Failed: ' + error.message;"
        "    });"
        "}"
        ""
        "function updateHomeStatus(data) {"
        "  console.log('Updating home status with:', data);"
        "  "
        "  if (data.connected) {"
        "    document.getElementById('swd-status').innerHTML = '<span style=\"color:green;\">Connected</span>';"
        "    "
        "    if (data.approtect_status) {"
        "      let approtectHtml = data.approtect_status;"
        "      if (data.approtect_status.includes('ENABLED')) {"
        "        approtectHtml = '<span style=\"color:#dc3545;\">🔒 ' + data.approtect_status + '</span>';"
        "      } else if (data.approtect_status.includes('Disabled')) {"
        "        approtectHtml = '<span style=\"color:#28a745;\">🔓 ' + data.approtect_status + '</span>';"
        "      }"
        "      document.getElementById('approtect-status').innerHTML = approtectHtml;"
        "    } else {"
        "      document.getElementById('approtect-status').textContent = 'Unknown';"
        "    }"
        "    "
        "    document.getElementById('device-id').textContent = data.device_id || 'Unknown';"
        "    "
        "    if (data.flash_size) {"
        "      document.getElementById('flash-size').textContent = (data.flash_size / 1024) + ' KB';"
        "    } else {"
        "      document.getElementById('flash-size').textContent = 'Unknown';"
        "    }"
        "    "
        "    if (data.ram_size) {"
        "      document.getElementById('ram-size').textContent = (data.ram_size / 1024) + ' KB';"
        "    } else {"
        "      document.getElementById('ram-size').textContent = 'Unknown';"
        "    }"
        "    "
        "    if (data.core_halted !== undefined) {"
        "      if (data.core_halted) {"
        "        document.getElementById('core-state').innerHTML = '<span style=\"color:#dc3545;\">⏸️ Halted</span>';"
        "      } else {"
        "        document.getElementById('core-state').innerHTML = '<span style=\"color:#28a745;\">▶️ Running</span>';"
        "      }"
        "    } else {"
        "      document.getElementById('core-state').textContent = 'Unknown';"
        "    }"
        "    "
        "    if (data.nvmc_ready !== undefined) {"
        "      let nvmcText = data.nvmc_ready ? '✅ Ready' : '⏳ Busy';"
        "      if (data.nvmc_state) {"
        "        nvmcText += ' (' + data.nvmc_state + ')';"
        "      }"
        "      document.getElementById('nvmc-state').innerHTML = nvmcText;"
        "    } else {"
        "      document.getElementById('nvmc-state').textContent = 'Unknown';"
        "    }"
        "  } else {"
        "    document.getElementById('swd-status').innerHTML = '<span style=\"color:red;\">Disconnected</span>';"
        "    document.getElementById('approtect-status').textContent = 'N/A';"
        "    document.getElementById('device-id').textContent = 'N/A';"
        "    document.getElementById('flash-size').textContent = 'N/A';"
        "    document.getElementById('ram-size').textContent = 'N/A';"
        "    document.getElementById('core-state').textContent = 'N/A';"
        "    document.getElementById('nvmc-state').textContent = 'N/A';"
        "  }"
        "  "
        "  document.getElementById('last-check').textContent = new Date().toLocaleTimeString();"
        "      updateBatteryStatus();"
        "      updateWiFiStatus();"
        "}"
        ""
        "function checkSWD() {"
        "  document.getElementById('protStatus').innerHTML = 'Checking SWD connection...';"
        "  document.getElementById('swdRegisterDump').textContent = 'Fetching register data...';"
        "  "
        "  fetch('/check_swd')"
        "    .then(response => response.json())"
        "    .then(data => {"
        "      let output = '';"
        "      "
        "      if (data.connected) {"
        "        output += '=== 🔗 SWD CONNECTION STATUS ===\\n';"
        "        output += 'Status: CONNECTED\\n';"
        "        output += 'Timestamp: ' + new Date().toLocaleString() + '\\n\\n';"
        "        "
        "        output += '=== 🖥️ DEVICE INFORMATION ===\\n';"
        "        if (data.device_id) output += 'Device ID: ' + data.device_id + '\\n';"
        "        if (data.flash_size) output += 'Flash Size: ' + (data.flash_size/1024) + ' KB\\n';"
        "        if (data.ram_size) output += 'RAM Size: ' + (data.ram_size/1024) + ' KB\\n';"
        "        if (data.bootloader_addr) output += 'Bootloader: ' + data.bootloader_addr + '\\n';"
        "        output += '\\n';"
        "        "
        "        output += '=== 🔒 SECURITY STATUS ===\\n';"
        "        output += 'APPROTECT Raw: ' + (data.approtect || 'Unknown') + '\\n';"
        "        output += 'APPROTECT Status: ' + (data.approtect_status || 'Unknown') + '\\n';"
        "        if (data.approtect_status && data.approtect_status.includes('ENABLED')) {"
        "          output += '⚠️ WARNING: Device is PROTECTED - Mass erase required!\\n';"
        "        }"
        "        output += '\\n';"
        "        "
        "        output += '=== ⚙️ CORE STATUS ===\\n';"
        "        output += 'Core State: ' + (data.core_halted ? 'HALTED 🛑' : 'RUNNING ▶️') + '\\n';"
        "        output += 'NVMC Ready: ' + (data.nvmc_ready ? 'YES ✅' : 'NO ❌') + '\\n';"
        "        output += 'NVMC State: ' + (data.nvmc_state || 'Unknown') + '\\n';"
        "        output += '\\n';"
        "        "
        "        if (data.registers) {"
        "          output += '=== 📊 DETAILED REGISTER DUMP ===\\n\\n';"
        "          "
        "          output += '--- NVMC (Non-Volatile Memory Controller) ---\\n';"
        "          if (data.registers.nvmc_ready) output += 'NVMC_READY: ' + data.registers.nvmc_ready + '\\n';"
        "          if (data.registers.nvmc_readynext) output += 'NVMC_READYNEXT: ' + data.registers.nvmc_readynext + '\\n';"
        "          if (data.registers.nvmc_config) output += 'NVMC_CONFIG: ' + data.registers.nvmc_config + '\\n';"
        "          output += '\\n';"
        "          "
        "          output += '--- UICR (User Information Config) ---\\n';"
        "          if (data.registers.approtect) output += 'UICR_APPROTECT: ' + data.registers.approtect + '\\n';"
        "          if (data.registers.bootloader_addr) output += 'UICR_BOOTLOADERADDR: ' + data.registers.bootloader_addr + '\\n';"
        "          if (data.registers.nrffw0) output += 'UICR_NRFFW0: ' + data.registers.nrffw0 + '\\n';"
        "          if (data.registers.nrffw1) output += 'UICR_NRFFW1: ' + data.registers.nrffw1 + '\\n';"
        "          output += '\\n';"
        "          "
        "          output += '--- FICR (Factory Information Config) ---\\n';"
        "          if (data.registers.codepagesize) output += 'FICR_CODEPAGESIZE: ' + data.registers.codepagesize + '\\n';"
        "          if (data.registers.codesize) output += 'FICR_CODESIZE: ' + data.registers.codesize + '\\n';"
        "          if (data.registers.deviceid0) output += 'FICR_DEVICEID0: ' + data.registers.deviceid0 + '\\n';"
        "          if (data.registers.deviceid1) output += 'FICR_DEVICEID1: ' + data.registers.deviceid1 + '\\n';"
        "          if (data.registers.info_part) output += 'FICR_INFO_PART: ' + data.registers.info_part + '\\n';"
        "          if (data.registers.info_variant) output += 'FICR_INFO_VARIANT: ' + data.registers.info_variant + '\\n';"
        "          if (data.registers.info_ram) output += 'FICR_INFO_RAM: ' + data.registers.info_ram + '\\n';"
        "          if (data.registers.info_flash) output += 'FICR_INFO_FLASH: ' + data.registers.info_flash + '\\n';"
        "          output += '\\n';"
        "          "
        "          output += '--- Debug Registers ---\\n';"
        "          if (data.registers.dhcsr) output += 'DHCSR: ' + data.registers.dhcsr + '\\n';"
        "          if (data.registers.demcr) output += 'DEMCR: ' + data.registers.demcr + '\\n';"
        "          output += '\\n';"
        "          "
        "          output += '--- Flash Memory Samples ---\\n';"
        "          if (data.registers.flash_0x0) output += 'Flash[0x00000]: ' + data.registers.flash_0x0 + ' (Reset Vector)\\n';"
        "          if (data.registers.flash_0x1000) output += 'Flash[0x01000]: ' + data.registers.flash_0x1000 + '\\n';"
        "          if (data.registers.flash_0xF4000) output += 'Flash[0xF4000]: ' + data.registers.flash_0xF4000 + ' (Bootloader)\\n';"
        "        }"
        "        "
        "        document.getElementById('protStatus').innerHTML = '<b style=\"color:green;\">✅ SWD Connected</b>';"
        "        if (data.approtect_status && data.approtect_status.includes('ENABLED')) {"
        "          document.getElementById('protStatus').innerHTML += ' - <span style=\"color:red;\">🔒 PROTECTION ENABLED</span>';"
        "        }"
        "      } else {"
        "        output += '=== ❌ SWD CONNECTION FAILED ===\\n\\n';"
        "        output += 'Status: DISCONNECTED\\n';"
        "        if (data.error) output += 'Error: ' + data.error + '\\n';"
        "        output += '\\nTroubleshooting:\\n';"
        "        output += '1. Check SWD connections\\n';"
        "        output += '2. Verify target power\\n';"
        "        output += '3. Try power cycling target\\n';"
        "        document.getElementById('protStatus').innerHTML = '<b style=\"color:red;\">❌ SWD Disconnected</b>';"
        "      }"
        "      "
        "      document.getElementById('swdRegisterDump').textContent = output;"
        "    })"
        "    .catch(error => {"
        "      document.getElementById('protStatus').innerHTML = '<b style=\"color:red;\">Error: ' + error.message + '</b>';"
        "      document.getElementById('swdRegisterDump').textContent = 'Failed to fetch SWD status: ' + error.message;"
        "    });"
        "}"
        ""
        "function releaseSWD() {"
        "  fetch('/release_swd').then(() => {"
        "    document.getElementById('protStatus').innerText = 'SWD Released';"
        "  });"
        "}"
        ""
        "function massErase() {"
        "  if (!confirm('This will ERASE EVERYTHING on the chip. Continue?')) return;"
        "  document.getElementById('protStatus').innerText = 'Performing mass erase...';"
        "  fetch('/mass_erase')"
        "    .then(r => r.json())"
        "    .then(data => {"
        "      document.getElementById('protStatus').innerText = data.message;"
        "      setTimeout(checkSWD, 2000);"
        "    });"
        "}"
        ""
        "function checkPowerStatus() {"
        "  fetch('/power_status')"
        "    .then(r => r.json())"
        "    .then(data => {"
        "      if (data.success) {"
        "        let statusDiv = document.getElementById('powerStatus');"
        "        let statusHtml = '<span class=\"status-indicator status-';"
        "        if (data.powered) {"
        "          statusHtml += 'online\"></span>Power Status: <span style=\"color:#28a745;\">ON</span>';"
        "        } else {"
        "          statusHtml += 'offline\"></span>Power Status: <span style=\"color:#dc3545;\">OFF</span>';"
        "        }"
        "        statusDiv.innerHTML = statusHtml;"
        "      }"
        "    })"
        "    .catch(error => {"
        "      document.getElementById('powerStatus').innerHTML = "
        "        '<span class=\"status-indicator status-unknown\"></span>Power Status: Error';"
        "    });"
        "}"
        ""
        "function updateBatteryStatus() {"
        "  fetch('/battery_status')"
        "    .then(r => r.json())"
        "    .then(data => {"
        "      if (data.success) {"
        "        document.getElementById('battery-voltage').textContent = data.voltage.toFixed(2) + ' V';"
        "        document.getElementById('battery-percentage').textContent = data.percentage.toFixed(0) + '%';"
        "        let statusColor = '#28a745';"
        "        if (data.is_critical) statusColor = '#dc3545';"
        "        else if (data.is_low) statusColor = '#ffc107';"
        "        else if (data.is_charging) statusColor = '#17a2b8';"
        "        document.getElementById('battery-status').innerHTML = '<span style=\"color:' + statusColor + ';font-weight:bold;\">' + data.status_text + '</span>';"
        "        document.getElementById('battery-range').textContent = data.voltage_min.toFixed(2) + 'V / ' + data.voltage_max.toFixed(2) + 'V';"
        "        document.getElementById('battery-avg').textContent = data.voltage_avg.toFixed(2) + ' V';"
        "        let voltageEl = document.getElementById('power-battery-voltage');"
        "        if (voltageEl) {"
        "          voltageEl.textContent = data.voltage.toFixed(2) + ' V';"
        "          voltageEl.style.color = statusColor;"
        "        }"
        "        let percentEl = document.getElementById('power-battery-percent');"
        "        if (percentEl) {"
        "          percentEl.textContent = data.percentage.toFixed(0) + '%';"
        "          percentEl.style.color = statusColor;"
        "        }"
        "        let statusEl = document.getElementById('power-battery-status');"
        "        if (statusEl) {"
        "          statusEl.textContent = data.status_text;"
        "          statusEl.style.color = statusColor;"
        "        }"
        "        let barEl = document.getElementById('battery-bar');"
        "        if (barEl) {"
        "          barEl.style.width = data.percentage + '%';"
        "          if (data.is_critical) {"
        "            barEl.style.background = 'linear-gradient(90deg,#dc3545,#ff6b6b)';"
        "          } else if (data.is_low) {"
        "            barEl.style.background = 'linear-gradient(90deg,#ffc107,#ffeb3b)';"
        "          } else if (data.is_charging) {"
        "            barEl.style.background = 'linear-gradient(90deg,#17a2b8,#5bc0de)';"
        "          } else {"
        "            barEl.style.background = 'linear-gradient(90deg,#28a745,#66bb6a)';"
        "          }"
        "        }"
        "      }"
        "    })"
        "    .catch(error => {"
        "      console.error('Battery status error:', error);"
        "    });"
        "}"
        ""
        "function updateWiFiStatus() {"
        "  fetch('/wifi_status')"
        "    .then(response => response.json())"
        "    .then(data => {"
        "      if (data.connected) {"
        "        let modeEl = document.getElementById('wifi-mode');"
        "        if (modeEl) {"
        "          let modeHtml = data.mode;"
        "          if (data.is_lr) {"
        "            modeHtml = '<span style=\"color:#17a2b8;font-weight:bold;\">🚀 ' + data.mode + '</span>';"
        "          } else {"
        "            modeHtml = '<span style=\"color:#28a745;\">📡 ' + data.mode + '</span>';"
        "          }"
        "          modeEl.innerHTML = modeHtml;"
        "        }"
        "        let ssidEl = document.getElementById('wifi-ssid');"
        "        if (ssidEl) ssidEl.textContent = data.ssid || 'Unknown';"
        "        let rssiEl = document.getElementById('wifi-rssi');"
        "        if (rssiEl && data.rssi) {"
        "          let rssiColor = '#28a745';"
        "          if (data.rssi < -80) rssiColor = '#dc3545';"
        "          else if (data.rssi < -70) rssiColor = '#ffc107';"
        "          rssiEl.innerHTML = '<span style=\"color:' + rssiColor + ';\">' + data.rssi + ' dBm</span>';"
        "        }"
        "        let qualityEl = document.getElementById('wifi-quality');"
        "        if (qualityEl && data.quality) {"
        "          let barColor = '#28a745';"
        "          if (data.quality < 30) barColor = '#dc3545';"
        "          else if (data.quality < 60) barColor = '#ffc107';"
        "          let bars = '▁▂▃▄▅';"
        "          let barCount = Math.ceil(data.quality / 20);"
        "          let barDisplay = bars.substr(0, barCount);"
        "          qualityEl.innerHTML = '<span style=\"color:' + barColor + ';\">' + barDisplay + ' ' + data.quality + '%</span>';"
        "        }"
        "        if (data.channel) {"
        "          document.getElementById('wifi-channel').textContent = data.channel;"
        "        }"
        "        if (data.phy_mode) {"
        "          document.getElementById('wifi-phy').textContent = data.phy_mode;"
        "        }"
        "        if (data.gateway) {"
        "          document.getElementById('wifi-gateway').textContent = data.gateway;"
        "        }"
        "        if (data.ip) {"
        "          document.getElementById('device-ip').textContent = data.ip;"
        "        }"
        "      } else {"
        "        document.getElementById('wifi-mode').innerHTML = '<span style=\"color:#dc3545;\">Disconnected</span>';"
        "        document.getElementById('wifi-ssid').textContent = 'N/A';"
        "        document.getElementById('wifi-rssi').textContent = 'N/A';"
        "        document.getElementById('wifi-quality').textContent = 'N/A';"
        "        document.getElementById('wifi-channel').textContent = 'N/A';"
        "        document.getElementById('wifi-phy').textContent = 'N/A';"
        "        document.getElementById('wifi-gateway').textContent = 'N/A';"
        "      }"
        "    })"
        "    .catch(error => {"
        "      console.error('WiFi status error:', error);"
        "      document.getElementById('wifi-mode').innerHTML = '<span style=\"color:#dc3545;\">Error</span>';"
        "    });"
        "}"
        ""
        "function powerOn() {"
        "  document.getElementById('powerOperationStatus').textContent = 'Turning on...';"
        "  fetch('/power_on', {method: 'POST'})"
        "    .then(r => r.json())"
        "    .then(data => {"
        "      document.getElementById('powerOperationStatus').textContent = data.message || 'Power on';"
        "      setTimeout(checkPowerStatus, 500);"
        "    });"
        "}"
        ""
        "function powerOff() {"
        "  document.getElementById('powerOperationStatus').textContent = 'Turning off...';"
        "  fetch('/power_off', {method: 'POST'})"
        "    .then(r => r.json())"
        "    .then(data => {"
        "      document.getElementById('powerOperationStatus').textContent = data.message || 'Power off';"
        "      setTimeout(checkPowerStatus, 500);"
        "    });"
        "}"
        ""
        "function powerReboot() {"
        "  document.getElementById('powerOperationStatus').textContent = 'Rebooting...';"
        "  fetch('/power_reboot', {method: 'POST'})"
        "    .then(r => r.json())"
        "    .then(data => {"
        "      document.getElementById('powerOperationStatus').textContent = data.message || 'Rebooting';"
        "      setTimeout(checkPowerStatus, 16000);"
        "    });"
        "}"
        ""
        "function updateProgress() {"
        "  fetch('/progress')"
        "    .then(r => r.json())"
        "    .then(data => {"
        "      if (data.in_progress) {"
        "        let pct = 0;"
        "        if (data.total > 0) {"
        "          if (data.flashed > 0) {"
        "            pct = Math.round((data.flashed * 100) / data.total);"
        "          } else if (data.received > 0) {"
        "            pct = Math.round((data.received * 50) / data.total);"
        "          }"
        "        }"
        "        document.getElementById('progressBar').style.width = pct + '%';"
        "        document.getElementById('status').innerText = 'Progress: ' + pct + '%';"
        "      } else {"
        "        if (progressTimer) {"
        "          clearInterval(progressTimer);"
        "          progressTimer = null;"
        "        }"
        "        document.getElementById('progressBar').style.width = '100%';"
        "        document.getElementById('status').innerText = data.message || 'Complete';"
        "        document.querySelector('#uploadBtn').disabled = false;"
        "      }"
        "    });"
        "}"
        ""
        "function uploadFirmware() {"
        "  const file = document.getElementById('hexFile').files[0];"
        "  if (!file) { alert('Please select a hex file'); return; }"
        "  "
        "  document.querySelector('#uploadBtn').disabled = true;"
        "  document.getElementById('status').innerText = 'Uploading...';"
        "  document.getElementById('progressBar').style.width = '0%';"
        "  "
        "  const xhr = new XMLHttpRequest();"
        "  xhr.upload.onprogress = function(e) {"
        "    if (e.lengthComputable) {"
        "      const pct = Math.round((e.loaded / e.total) * 100);"
        "      document.getElementById('progressBar').style.width = pct + '%';"
        "      document.getElementById('status').innerText = 'Progress: ' + pct + '%';"
        "    }"
        "  };"
        "  "
        "  xhr.onload = function() {"
        "    if (xhr.status === 200) {"
        "      document.getElementById('progressBar').style.width = '100%';"
        "      document.getElementById('status').innerText = '✓ Upload complete!';"
        "    } else {"
        "      document.getElementById('status').innerText = '✗ Failed (HTTP ' + xhr.status + ')';"
        "    }"
        "    document.querySelector('#uploadBtn').disabled = false;"
        "  };"
        "  "
        "  xhr.onerror = function() {"
        "    document.getElementById('status').innerText = '✗ Upload error';"
        "    document.querySelector('#uploadBtn').disabled = false;"
        "  };"
        "  "
        "  xhr.timeout = 300000;"
        "  xhr.open('POST', '/upload');"
        "  xhr.send(file);"
        "}"
        ""
        "// Call functions immediately when script loads (don't wait for DOMContentLoaded)"
        "window.addEventListener('load', function() {"
        "  // Call all status functions immediately"
        "  setTimeout(function() {"
        "    refreshStatus();"
        "    checkPowerStatus();"
        "    updateBatteryStatus();"
        "    updateWiFiStatus();"
        "  }, 100);"
        "  "
        "  // Set up periodic updates"
        "  setInterval(refreshStatus, 10000);"
        "  "
        "  setInterval(checkPowerStatus, 5000);"
        "  setInterval(updateBatteryStatus, 5000);"
        "  setInterval(updateWiFiStatus, 5000);"
        "});"
        ""
        "// Also try to initialize immediately if page is already loaded"
        "if (document.readyState === 'complete' || document.readyState === 'interactive') {"
        "  console.log('Page already loaded - initializing immediately...');"
        "  setTimeout(function() {"
        "    refreshStatus();"
        "    checkPowerStatus();"
        "    updateBatteryStatus();"
        "    updateWiFiStatus();"
        "  }, 100);"
        "}"
        ""
        "console.log('=== SCRIPT END ===');"
        ""
        "// Initialize everything on page load"
        "console.log('Initializing status on page load...');"
        ""
        "// Use a short timeout to ensure DOM is ready"
        "setTimeout(function() {"
        "  console.log('Running initial status checks...');"
        "  if (typeof refreshStatus === 'function') refreshStatus();"
        "  if (typeof checkPowerStatus === 'function') checkPowerStatus();"
        "  if (typeof updateBatteryStatus === 'function') updateBatteryStatus();"
        "  if (typeof updateWiFiStatus === 'function') updateWiFiStatus();"
        "}, 500);"
        ""
        "// Set up periodic updates"
        "setInterval(refreshStatus, 10000);"
        "setInterval(checkPowerStatus, 5000);"
        "setInterval(updateBatteryStatus, 5000);"
        "setInterval(updateWiFiStatus, 5000);"
        ""
        "</script>"
        "</body></html>";

    // Send the main HTML page
    httpd_resp_send_chunk(req, js_start, strlen(js_start));

    // The connection functionality has been moved to the main BLE script above
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

// Failsafe status endpoint
static esp_err_t failsafe_status_handler(httpd_req_t *req) {
    char resp[256];

    bool is_armed = failsafe_armed;
    uint32_t remaining = 0;

    if (is_armed && failsafe_start_time > 0) {
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
        uint32_t elapsed = current_time - failsafe_start_time;

        if (elapsed < MAX_UPTIME_AFTER_WIFI_SEC) {
            remaining = MAX_UPTIME_AFTER_WIFI_SEC - elapsed;
        }
    }

    snprintf(resp, sizeof(resp),
        "{\"armed\":%s,\"remaining_sec\":%lu,\"limit_sec\":%d}",
        is_armed ? "true" : "false",
        remaining,
        MAX_UPTIME_AFTER_WIFI_SEC);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 30;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.stack_size = 10240;

    if (httpd_start(&web_server, &config) == ESP_OK) {
        // Register handlers
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_handler,
            .user_ctx = NULL
        };

        httpd_uri_t release_uri = {
            .uri = "/release_swd",
            .method = HTTP_GET,
            .handler = release_swd_handler,
            .user_ctx = NULL
        };

        httpd_uri_t failsafe_uri = {
            .uri = "/failsafe_status",
            .method = HTTP_GET,
            .handler = failsafe_status_handler,
            .user_ctx = NULL
        };

        httpd_register_uri_handler(web_server, &root_uri);
        httpd_register_uri_handler(web_server, &release_uri);
        httpd_register_uri_handler(web_server, &failsafe_uri);
        register_upload_handlers(web_server);
        register_power_handlers(web_server);

        ESP_LOGI(TAG, "Web server started successfully");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to start web server");
    return ESP_FAIL;
}

// Stop web server
// Stop web server (made global for wifi_manager cleanup)
void stop_webserver(void) {
    if (web_server) {
        ESP_LOGI(TAG, "Stopping web server");
        httpd_stop(web_server);
        web_server = NULL;
        device_ip = "Not connected";  // Reset IP string
    }
}

// SWD connection function removed - SWD initializes on-demand when web interface requests it
// This reduces boot time and prevents unnecessary target interference

// System health monitoring task - simplified to only monitor heap
static void system_health_task(void *arg) {
    ESP_LOGI(TAG, "System health monitoring started");

    size_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "Initial free heap: %d bytes", free_heap);

    // Monitor heap every 5 seconds
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        free_heap = esp_get_free_heap_size();

        // Only log if memory is getting low
        if (free_heap < 20000) {
            ESP_LOGW(TAG, "Low memory warning: %d bytes free", free_heap);
        }

        // Log periodically even if OK (every minute)
        static int check_count = 0;
        check_count++;
        if (check_count >= 12) {  // 12 * 5 seconds = 60 seconds
            ESP_LOGI(TAG, "Heap: %d bytes free", free_heap);
            check_count = 0;
        }
    }
}

// System initialization
static void init_system(void) {
    // CRITICAL: NVS must be initialized before other systems
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS Flash initialized");

    power_config_t power_cfg = {
        .target_power_gpio = TARGET_POWER_GPIO,
        .power_on_delay_ms = 100,
        .reset_hold_ms = 50,
        .sleep_duration_sec = 0,  // Not used with adaptive sleep
        .wifi_check_interval_ms = 5000,
        .wifi_timeout_ms = (WIFI_LR_CONNECT_TIMEOUT_SEC + WIFI_CONNECT_TIMEOUT_SEC) * 1000,
        .wake_ssid = WIFI_SSID,
        .watchdog_timeout_sec = 0,
        .enable_brownout_detect = true,
        .error_cooldown_ms = 1000,
        .absolute_reboot_interval_sec = ABSOLUTE_REBOOT_INTERVAL_SEC,
        .enable_absolute_timer = ENABLE_ABSOLUTE_REBOOT_TIMER
    };
    ESP_ERROR_CHECK(power_mgmt_init(&power_cfg));

    wake_reason_t wake_reason = power_get_wake_reason();
    ESP_LOGI(TAG, "Wake reason: %d", wake_reason);

    // SWD initialization removed - only initializes when web interface requests it
    ESP_LOGI(TAG, "SWD will initialize on-demand when needed");

    xTaskCreate(system_health_task, "health", 4096, NULL, 5, NULL);

    // ========================================================================
    // Initialize hardware watchdog last (after all other initialization)
    // This ensures we don't get false triggers during startup
    // ========================================================================
    // ESP_LOGI(TAG, "Initializing hardware watchdog...");
    // esp_err_t wdt_ret = power_init_hardware_watchdog();
    // if (wdt_ret == ESP_OK) {
    //     ESP_LOGI(TAG, "Hardware watchdog initialized successfully");
    // } else {
    //     ESP_LOGW(TAG, "Hardware watchdog initialization failed - continuing without it");
    // }
}

static esp_err_t release_swd_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Manual SWD release requested");
    if (swd_is_connected()) {
        swd_release_target();
        swd_shutdown();
    }
    httpd_resp_send(req, "Released", 8);
    return ESP_OK;
}

// =============================================================================
// FAILSAFE TIMER IMPLEMENTATION
// =============================================================================

/**
 * @brief High-priority task that forces reboot after configured timeout
 */
static void failsafe_reboot_task(void *arg) {
    uint32_t timeout_sec = *((uint32_t*)arg);
    uint32_t elapsed_sec = 0;

    ESP_LOGW(TAG, "╔════════════════════════════════════════════════════════════╗");
    ESP_LOGW(TAG, "║  FAILSAFE TIMER ARMED: Device will reboot in %4lu seconds  ║", timeout_sec);
    ESP_LOGW(TAG, "║  This ensures device returns to sleep/wake cycle          ║");
    ESP_LOGW(TAG, "╚════════════════════════════════════════════════════════════╝");

    // Main countdown loop - check every 10 seconds
    while (elapsed_sec < timeout_sec) {
        uint32_t remaining = timeout_sec - elapsed_sec;

        // Log warnings in final 5 minutes
        if (remaining <= 300 && remaining % 60 == 0) {
            ESP_LOGW(TAG, "⏰ FAILSAFE: %lu minutes until automatic reboot", remaining / 60);
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
        elapsed_sec += 10;
    }

    // TIMEOUT REACHED - FORCE REBOOT
    ESP_LOGW(TAG, "");
    ESP_LOGW(TAG, "╔════════════════════════════════════════════════════════════╗");
    ESP_LOGW(TAG, "║           FAILSAFE TRIGGERED - FORCING REBOOT              ║");
    ESP_LOGW(TAG, "╚════════════════════════════════════════════════════════════╝");
    ESP_LOGW(TAG, "Reason: Maximum uptime (%lu sec) reached", timeout_sec);

    // Get final battery status
    battery_status_t battery;
    if (power_get_battery_status(&battery) == ESP_OK) {
        ESP_LOGI(TAG, "Final battery: %.2fV (%.0f%%)", battery.voltage, battery.percentage);
    }

    // Prepare GPIO states
    ESP_LOGI(TAG, "Preparing GPIO states for reboot...");
    power_prepare_for_sleep();

    // Stop web server if running
    extern httpd_handle_t web_server;
    if (web_server != NULL) {
        ESP_LOGI(TAG, "Stopping web server...");
        extern void stop_webserver(void);
        stop_webserver();
    }

    // Stop WiFi
    ESP_LOGI(TAG, "Stopping WiFi...");
    esp_wifi_stop();

    // Allow logs to flush
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGW(TAG, "=== REBOOTING NOW ===");
    vTaskDelay(pdMS_TO_TICKS(100));

    // Force restart
    esp_restart();
}

/**
 * @brief Start the failsafe reboot timer
 */
static void start_failsafe_timer(void) {
    #if !ENABLE_FAILSAFE_REBOOT
    ESP_LOGI(TAG, "Failsafe timer disabled in config");
    return;
    #endif

    if (failsafe_armed) {
        ESP_LOGW(TAG, "Failsafe timer already armed - ignoring duplicate start");
        return;
    }

    static uint32_t timeout = MAX_UPTIME_AFTER_WIFI_SEC;

    BaseType_t result = xTaskCreate(
        failsafe_reboot_task,
        "failsafe",
        4096,                           // Stack size
        &timeout,                       // Pass timeout as parameter
        configMAX_PRIORITIES - 1,       // HIGHEST priority
        &failsafe_task_handle
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "CRITICAL: Failed to create failsafe task!");
        return;
    }

    failsafe_armed = true;
    failsafe_start_time = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;

    ESP_LOGW(TAG, "");
    ESP_LOGW(TAG, "⏰ FAILSAFE ENABLED: Device will auto-reboot in %lu seconds (%.1f hours)",
            timeout, timeout / 3600.0f);
    ESP_LOGW(TAG, "   This prevents battery drain from extended wake periods");
    ESP_LOGW(TAG, "");
}

/**
 * @brief Get failsafe status for web interface
 */
void get_failsafe_status(bool *is_armed, uint32_t *remaining_sec) {
    if (!is_armed || !remaining_sec) return;

    *is_armed = failsafe_armed;

    if (failsafe_armed) {
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
        uint32_t elapsed = current_time - failsafe_start_time;

        if (elapsed < MAX_UPTIME_AFTER_WIFI_SEC) {
            *remaining_sec = MAX_UPTIME_AFTER_WIFI_SEC - elapsed;
        } else {
            *remaining_sec = 0;
        }
    } else {
        *remaining_sec = 0;
    }
}

// Main application entry
void app_main(void) {
    // =========================================================================
    // BROWNOUT LOOP PROTECTION - Hardware BMS System
    // =========================================================================
    // Note: ESP32 bootloader brownout detector is DISABLED in sdkconfig
    //
    // Protection Strategy:
    // 1. Application checks battery voltage (3.4V-3.6V thresholds)
    // 2. Hardware BMS cuts power at ~3.0V (external protection)
    // 3. ESP32 brownout (2.4V) is disabled - never reached due to BMS
    //
    // The code below handles bootloader brownout IF it were enabled,
    // but with BMS protection this should never trigger.
    // Keeping the code for systems without BMS or for diagnostics.
    // =========================================================================

    esp_reset_reason_t reset_reason = esp_reset_reason();
    uint64_t current_time_sec = esp_timer_get_time() / 1000000ULL;

    if (reset_reason == ESP_RST_BROWNOUT) {
        // Brownout should not happen with BMS, but handle it anyway
        rtc_brownout_count++;
        rtc_last_brownout_time = current_time_sec;

        // Use printf instead of ESP_LOG (not initialized yet)
        printf("\n");
        printf("╔════════════════════════════════════════════════════════════╗\n");
        printf("║  ⚠️  UNEXPECTED BROWNOUT RESET (#%lu)                     ║\n",
               (unsigned long)rtc_brownout_count);
        printf("║  This should not occur with BMS protection!               ║\n");
        printf("║  Check BMS configuration and wiring.                      ║\n");
        printf("╚════════════════════════════════════════════════════════════╝\n");
        printf("\n");

        // Still handle brownout loop for safety
        if (rtc_brownout_count >= BROWNOUT_LOOP_THRESHOLD) {
            printf("\n");
            printf("╔════════════════════════════════════════════════════════════╗\n");
            printf("║  🚨 BROWNOUT LOOP - EMERGENCY SLEEP                       ║\n");
            printf("║  BMS may be malfunctioning - investigate immediately!     ║\n");
            printf("╚════════════════════════════════════════════════════════════╝\n");
            printf("║                                                            ║\n");
            printf("║  Multiple brownout resets detected (%lu times)            ║\n",
                   (unsigned long)rtc_brownout_count);
            printf("║  ESP32 brownout detector should be DISABLED with BMS!     ║\n");
            printf("║                                                            ║\n");
            printf("║  ENTERING 24-HOUR RECOVERY SLEEP                          ║\n");
            printf("║  Sleep duration: %lu hours                                ║\n",
                   (unsigned long)(BROWNOUT_RECOVERY_SLEEP_SEC / 3600));
            printf("╚════════════════════════════════════════════════════════════╝\n");
            printf("\n");

            // Minimal GPIO setup to turn off nRF52 (save maximum power)
            // Do this with minimal initialization to avoid brownout
            printf("Turning off nRF52 radio for maximum power savings...\n");
            gpio_reset_pin(GPIO_NUM_10);  // TARGET_POWER_GPIO
            gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT);
            gpio_set_level(GPIO_NUM_10, 1);  // 1 = OFF

            // Hold GPIO state during deep sleep
            gpio_hold_en(GPIO_NUM_10);
            gpio_deep_sleep_hold_en();

            printf("nRF52 powered off.\n");
            printf("\n");

            // Configure wake timer for 24 hours
            uint64_t sleep_time_us = (uint64_t)BROWNOUT_RECOVERY_SLEEP_SEC * 1000000ULL;
            esp_sleep_enable_timer_wakeup(sleep_time_us);

            printf("Entering deep sleep NOW...\n");
            printf("════════════════════════════════════════════════════════════\n");
            printf("\n");

            // Small delay to allow UART to flush
            for (volatile int i = 0; i < 1000000; i++) {
                __asm__ __volatile__("nop");
            }

            // Enter deep sleep - execution never returns
            esp_deep_sleep_start();
            // NEVER RETURNS
        }

        // First or second brownout - allow retry but log it
        printf("Brownout count: %lu (threshold: %d)\n",
               (unsigned long)rtc_brownout_count, BROWNOUT_LOOP_THRESHOLD);
        printf("Will enter recovery sleep if %d consecutive brownouts occur.\n",
               BROWNOUT_LOOP_THRESHOLD);
        printf("Attempting to continue boot...\n\n");

    } else {
        // Not a brownout reset - check if we recovered from previous brownouts
        if (rtc_brownout_count > 0) {
            // If last brownout was more than BROWNOUT_RESET_WINDOW_SEC ago, reset counter
            if (current_time_sec > rtc_last_brownout_time &&
                (current_time_sec - rtc_last_brownout_time) > BROWNOUT_RESET_WINDOW_SEC) {
                printf("\n");
                printf("✓ Brownout recovery successful!\n");
                printf("  Previous brownout count: %lu\n", (unsigned long)rtc_brownout_count);
                printf("  Time since last brownout: %llu seconds\n",
                       (unsigned long long)(current_time_sec - rtc_last_brownout_time));
                printf("  Resetting brownout counter.\n");
                printf("\n");
                rtc_brownout_count = 0;
            } else {
                printf("⚠️  Previous brownout count: %lu (monitoring for loop)\n",
                       (unsigned long)rtc_brownout_count);
            }
        }
    }

    // =========================================================================
    // Normal startup begins here
    // =========================================================================

    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "Mesh Radio Flasher v%s", DEVICE_VERSION);
    ESP_LOGI(TAG, "Build: %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "=================================");

    // =========================================================================
    // STATE: INIT
    // =========================================================================
    wake_context_t wake_ctx = {
        .state = WAKE_STATE_INIT,
        .wifi_connected = false,
        .nrf52_should_be_on = true,
        .wake_count = 0
    };

    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    if (wake_cause == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "=== Woke from deep sleep (count: %lu) ===",
                 power_get_wake_count());
    } else {
        ESP_LOGI(TAG, "=== Fresh boot (power on) ===");
    }

    // Initialize system
    init_system();

    // ========================================================================
    // CRITICAL: Check absolute uptime timer immediately after init
    // This MUST be called before any operations that could hang or take time
    // Will force reboot if accumulated awake time exceeds configured interval
    // This is a scheduled maintenance mechanism for long-term stability
    // ========================================================================
    ESP_LOGI(TAG, "Checking absolute uptime timer...");
    power_check_absolute_timer();  // Will reboot if exceeded, otherwise continues
    ESP_LOGI(TAG, "Absolute timer check complete");

    // Restore state if waking from deep sleep
    if (wake_cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "Restoring state from deep sleep...");
        power_restore_from_deep_sleep();
    }

    wake_ctx.wake_count = power_get_wake_count();

    // =========================================================================
    // STATE: BATTERY CHECK (ONCE)
    // =========================================================================
    wake_ctx.state = WAKE_STATE_BATTERY_CHECK;
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔════════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  STATE: BATTERY CHECK                                      ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════════════════╝");

    power_get_battery_status(&wake_ctx.battery);
    ESP_LOGI(TAG, "Battery: %.2fV (%.0f%%) - %s",
            wake_ctx.battery.voltage,
            wake_ctx.battery.percentage,
            wake_ctx.battery.is_critical ? "CRITICAL" :
            wake_ctx.battery.is_low ? "LOW" :
            wake_ctx.battery.voltage > BATTERY_HIGH_THRESHOLD ? "HIGH" : "NORMAL");

    // CRITICAL BATTERY - Skip everything, sleep immediately
    if (wake_ctx.battery.voltage < BATTERY_CRITICAL_THRESHOLD &&
        ENABLE_BATTERY_PROTECTION) {
        ESP_LOGW(TAG, "");
        ESP_LOGW(TAG, "╔════════════════════════════════════════════════════════════╗");
        ESP_LOGW(TAG, "║  CRITICAL BATTERY - IMMEDIATE SLEEP                        ║");
        ESP_LOGW(TAG, "║  Voltage: %.2fV (threshold: %.2fV)                        ║",
                wake_ctx.battery.voltage, BATTERY_CRITICAL_THRESHOLD);
        ESP_LOGW(TAG, "║  Skipping WiFi scan to conserve power                      ║");
        ESP_LOGW(TAG, "║  Sleep duration: %d seconds (%.1f hours)                   ║",
                DEEP_SLEEP_CRITICAL_SEC, DEEP_SLEEP_CRITICAL_SEC / 3600.0f);
        ESP_LOGW(TAG, "╚════════════════════════════════════════════════════════════╝");
        ESP_LOGW(TAG, "");

        power_target_off();  // Turn off nRF52
        power_enter_adaptive_deep_sleep();
        // Never returns
    }

    // =========================================================================
    // STATE: NRF52 POWER DECISION (ONCE)
    // =========================================================================
    wake_ctx.state = WAKE_STATE_NRF52_DECISION;
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔════════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  STATE: NRF52 POWER DECISION                               ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════════════════╝");

    // Determine if NRF52 should be on based on battery voltage and recovery logic
    // Uses threshold from config.h (NRF52_POWER_OFF_VOLTAGE = 3.6V)
    if (wake_ctx.battery.voltage < NRF52_POWER_OFF_VOLTAGE &&
        ENABLE_BATTERY_PROTECTION) {
        wake_ctx.nrf52_should_be_on = false;

        if (power_target_is_on()) {
            ESP_LOGW(TAG, "Battery (%.2fV) below NRF52 threshold (%.2fV) - turning OFF",
                    wake_ctx.battery.voltage, NRF52_POWER_OFF_VOLTAGE);
            power_target_off();
            ESP_LOGI(TAG, "  Recovery requirements:");
            ESP_LOGI(TAG, "  - Voltage must reach %.2fV (threshold + %.2fV hysteresis)",
                    NRF52_POWER_OFF_VOLTAGE + NRF52_POWER_ON_HYSTERESIS,
                    NRF52_POWER_ON_HYSTERESIS);
            ESP_LOGI(TAG, "  - Must stay off for minimum %d seconds (%.1f min)",
                    NRF52_MIN_OFF_TIME_SEC, NRF52_MIN_OFF_TIME_SEC / 60.0f);
            ESP_LOGI(TAG, "  - Will force turn-on after %d seconds (%.1f hours) if voltage marginal",
                    NRF52_MAX_OFF_TIME_SEC, NRF52_MAX_OFF_TIME_SEC / 3600.0f);
        } else {
            ESP_LOGI(TAG, "NRF52 already OFF (battery protection active)");
            ESP_LOGI(TAG, "  Current voltage: %.2fV", wake_ctx.battery.voltage);
            ESP_LOGI(TAG, "  Turn-on threshold: %.2fV (%.2fV threshold + %.2fV hysteresis)",
                    NRF52_POWER_OFF_VOLTAGE + NRF52_POWER_ON_HYSTERESIS,
                    NRF52_POWER_OFF_VOLTAGE,
                    NRF52_POWER_ON_HYSTERESIS);
        }
    } else {
        wake_ctx.nrf52_should_be_on = true;

        if (!power_target_is_on()) {
            ESP_LOGI(TAG, "Battery recovered to %.2fV (above %.2fV threshold)",
                    wake_ctx.battery.voltage, NRF52_POWER_OFF_VOLTAGE);
            ESP_LOGI(TAG, "  Turn-on decision made by power_restore_from_deep_sleep():");
            ESP_LOGI(TAG, "  - Checks voltage >= %.2fV (threshold + hysteresis)",
                    NRF52_POWER_OFF_VOLTAGE + NRF52_POWER_ON_HYSTERESIS);
            ESP_LOGI(TAG, "  - Checks minimum off time >= %d sec elapsed",
                    NRF52_MIN_OFF_TIME_SEC);
            ESP_LOGI(TAG, "  - OR forces turn-on if off time >= %d sec (%.1f hours)",
                    NRF52_MAX_OFF_TIME_SEC, NRF52_MAX_OFF_TIME_SEC / 3600.0f);

            if (!power_target_is_on()) {
                ESP_LOGI(TAG, "  Result: NRF52 remains OFF (recovery conditions not yet met)");
            }
        } else {
            ESP_LOGI(TAG, "NRF52 power: ON (battery sufficient: %.2fV)",
                    wake_ctx.battery.voltage);
        }
    }

    // =========================================================================
    // STATE: WIFI SCAN
    // =========================================================================
    wake_ctx.state = WAKE_STATE_WIFI_SCAN;
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔════════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  STATE: WIFI SCAN                                          ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "Battery OK (%.2fV) - attempting WiFi connection",
             wake_ctx.battery.voltage);

    // Initialize WiFi manager
    ESP_LOGI(TAG, "Initializing WiFi manager...");
    ESP_ERROR_CHECK(wifi_manager_init());

    // Single WiFi connection attempt (tries LR first, then normal)
    ESP_LOGI(TAG, "Attempting WiFi connection...");
    if (wifi_manager_connect() != ESP_OK) {
        ESP_LOGW(TAG, "");
        ESP_LOGW(TAG, "╔════════════════════════════════════════════════════════════╗");
        ESP_LOGW(TAG, "║  WIFI CONNECTION FAILED - ENTERING DEEP SLEEP              ║");
        ESP_LOGW(TAG, "╚════════════════════════════════════════════════════════════╝");
        ESP_LOGI(TAG, "Next wake: %s interval",
                wake_ctx.battery.is_low ? "Low battery" :
                wake_ctx.battery.is_critical ? "Critical" : "Normal");

        wake_ctx.state = WAKE_STATE_SLEEP;
        power_enter_adaptive_deep_sleep();
        // Never returns
    }

    // =========================================================================
    // STATE: ACTIVE (WiFi Connected)
    // =========================================================================
    wake_ctx.state = WAKE_STATE_ACTIVE;
    wake_ctx.wifi_connected = true;

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔════════════════════════════════════════════════════════════╗");
    ESP_LOGW(TAG, "║  WIFI CONNECTED - ENTERING ACTIVE MODE                     ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");

    const char* ip = wifi_manager_get_ip();
    ESP_LOGI(TAG, "✓ WiFi connected successfully");
    ESP_LOGI(TAG, "  IP Address: %s", ip);
    ESP_LOGI(TAG, "  Mode: %s", power_get_wifi_is_lr() ? "ESP-LR (Long Range)" : "Normal WiFi");
    ESP_LOGI(TAG, "  SSID: %s", power_get_wifi_ssid());
    ESP_LOGI(TAG, "");

    device_ip = (char*)ip;

    // Start failsafe timer
    ESP_LOGI(TAG, "Starting failsafe timer...");
    start_failsafe_timer();

    // Initialize SPIFFS storage
    ESP_LOGI(TAG, "Initializing storage...");
    init_storage();

    // Start web server
    ESP_LOGI(TAG, "Starting web server...");
    esp_err_t server_result = start_webserver();
    if (server_result == ESP_OK) {
        ESP_LOGI(TAG, "✓ Web server started on port 80");
    } else {
        ESP_LOGE(TAG, "✗ Failed to start web server: %s",
                 esp_err_to_name(server_result));
    }

    // SWD testing removed - triggers automatically from web interface

    // System ready
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔════════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  ACTIVE MODE - SYSTEM READY                                ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "  Web interface: http://%s", device_ip);
    ESP_LOGI(TAG, "  nRF52 Power: %s", power_target_is_on() ? "ON" : "OFF");
    ESP_LOGI(TAG, "  WiFi Mode: %s", power_get_wifi_is_lr() ? "ESP-LR" : "Normal");
    ESP_LOGI(TAG, "  Battery: %.2fV (%.0f%%)",
             wake_ctx.battery.voltage, wake_ctx.battery.percentage);
    ESP_LOGI(TAG, "  Wake Count: %lu", wake_ctx.wake_count);
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");

    // =========================================================================
    // BACKGROUND MONITORING (Active Mode Only)
    // =========================================================================
    ESP_LOGI(TAG, "Starting background monitoring (60 second interval)...");
    TickType_t last_battery_check = xTaskGetTickCount();
    TickType_t last_absolute_timer_check = xTaskGetTickCount();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));  // Check every 5 seconds

        // ====================================================================
        // CRITICAL: Check absolute uptime timer (every 60 seconds)
        // Ensures scheduled reboot happens even during long WiFi sessions
        // This is independent of WiFi failsafe timer
        // ====================================================================
        if ((xTaskGetTickCount() - last_absolute_timer_check) > pdMS_TO_TICKS(60000)) {
            power_check_absolute_timer();  // Will reboot if threshold exceeded
            last_absolute_timer_check = xTaskGetTickCount();
        }

        // ====================================================================
        // CRITICAL: Feed hardware watchdog to prevent reboot
        // Must be called at least every 60 seconds
        // ====================================================================
        // power_feed_watchdog();

        // Battery monitoring every 60 seconds
        if ((xTaskGetTickCount() - last_battery_check) > pdMS_TO_TICKS(60000)) {
            battery_status_t batt;
            power_get_battery_status(&batt);

            // Get absolute timer status if enabled
            uint64_t accumulated, limit, remaining;
            if (power_get_absolute_timer_status(&accumulated, &limit, &remaining) == ESP_OK) {
                uint64_t acc_min = accumulated / 60;
                uint64_t lim_min = limit / 60;

                ESP_LOGI(TAG, "Active: Battery=%.2fV (%.0f%%) | WiFi=%s | Wake=%lu | nRF52=%s | Uptime=%llu/%llum",
                        batt.voltage,
                        batt.percentage,
                        wifi_manager_is_connected() ? "Connected" : "Disconnected",
                        wake_ctx.wake_count,
                        power_target_is_on() ? "ON" : "OFF",
                        acc_min,
                        lim_min);
            } else {
                ESP_LOGI(TAG, "Active: Battery=%.2fV (%.0f%%) | WiFi=%s | Wake=%lu | nRF52=%s",
                        batt.voltage,
                        batt.percentage,
                        wifi_manager_is_connected() ? "Connected" : "Disconnected",
                        wake_ctx.wake_count,
                        power_target_is_on() ? "ON" : "OFF");
            }

            // Check if battery dropped below threshold during active mode
            if (batt.voltage < NRF52_POWER_OFF_VOLTAGE &&
                power_target_is_on() &&
                ENABLE_BATTERY_PROTECTION) {
                ESP_LOGW(TAG, "Battery dropped below %.2fV during active mode - turning off nRF52",
                        NRF52_POWER_OFF_VOLTAGE);
                power_target_off();
            }

            last_battery_check = xTaskGetTickCount();
        }
    }
}