// main.c - Resilient Field Flasher Main Application
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_mac.h"
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
#include "flash_safety.h"
#include "wifi_manager.h"


static const char *TAG = "FLASHER";

// Event group for system state
static EventGroupHandle_t system_events;
#define WIFI_CONNECTED_BIT  BIT0
#define SWD_CONNECTED_BIT   BIT1
#define FLASH_BUSY_BIT      BIT2
#define ERROR_STATE_BIT     BIT3
#define RECOVERY_MODE_BIT   BIT4

// Global variables
static char* device_ip = "Not connected";
static httpd_handle_t web_server = NULL;
static uint32_t wake_count = 0;

// System configuration
typedef struct {
    char wifi_ssid[32];
    char wifi_password[64];
    uint32_t sleep_timeout_sec;
    uint32_t watchdog_timeout_sec;
    bool auto_recovery;
    bool deep_sleep_enabled;
} system_config_t;

static system_config_t sys_config = {
    .wifi_ssid = "",
    .wifi_password = "",
    .sleep_timeout_sec = 300,
    .watchdog_timeout_sec = 0,
    .auto_recovery = true,
    .deep_sleep_enabled = false
};

// Global state
static bool swd_initialized = false;
static esp_timer_handle_t watchdog_timer __attribute__((unused)) = NULL;
static esp_timer_handle_t sleep_timer __attribute__((unused)) = NULL;
static uint32_t error_count = 0;
static uint32_t recovery_count = 0;

// Function declarations
static void init_system(void);
static void init_storage(void);
static void system_health_task(void *arg);
static esp_err_t try_swd_connection(void);
static void handle_critical_error(const char *context, esp_err_t error);
static void test_swd_functions(void);
static void test_memory_regions(void);
static esp_err_t start_webserver(void);
void stop_webserver(void);  // Made global for wifi_manager cleanup
static esp_err_t release_swd_handler(httpd_req_t *req);

// Initialize configuration from config.h
static void init_config(void) {
    strcpy(sys_config.wifi_ssid, WIFI_SSID);
    strcpy(sys_config.wifi_password, WIFI_PASSWORD);
}

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
        "<select id='fwType' style='padding:8px;border:1px solid #ddd;border-radius:4px;width:250px;margin-right:10px;'>"
        "<option value='app'>Application (0x26000)</option>"
        "<option value='softdevice'>SoftDevice (0x1000)</option>"
        "<option value='bootloader'>Bootloader (0xF4000)</option>"
        "<option value='full'>Full Image (from hex)</option>"
        "</select><br><br>"
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
        "  if (!file) {"
        "    alert('Please select a hex file');"
        "    return;"
        "  }"
        "  document.querySelector('#uploadBtn').disabled = true;"
        "  document.getElementById('status').innerText = 'Uploading firmware...';"
        "  document.getElementById('progressBar').style.width = '0%';"
        "  "
        "  const xhr = new XMLHttpRequest();"
        "  "
        "  xhr.upload.onprogress = function(e) {"
        "    if (e.lengthComputable) {"
        "      const percent = Math.round((e.loaded / e.total) * 100);"
        "      document.getElementById('progressBar').style.width = percent + '%';"
        "      document.getElementById('status').innerText = 'Uploading: ' + percent + '%';"
        "    }"
        "  };"
        "  "
        "  xhr.onload = async function() {"
        "    if (xhr.status === 200) {"
        "      document.getElementById('status').innerText = 'Upload complete, starting flash...';"
        "      "
        "      await fetch('/flash_stored', {method: 'POST'});"
        "      "
        "      const pollInterval = setInterval(async function() {"
        "        const statusResp = await fetch('/flash_status');"
        "        const status = await statusResp.json();"
        "        "
        "        document.getElementById('progressBar').style.width = status.percent + '%';"
        "        document.getElementById('status').innerText = status.status;"
        "        "
        "        if (!status.in_progress) {"
        "          clearInterval(pollInterval);"
        "          if (status.status.includes('complete')) {"
        "            document.getElementById('status').innerText = '✓ ' + status.status;"
        "          }"
        "          document.querySelector('#uploadBtn').disabled = false;"
        "        }"
        "      }, 1000);"
        "    } else {"
        "      document.getElementById('status').innerText = 'Upload failed';"
        "      document.querySelector('#uploadBtn').disabled = false;"
        "    }"
        "  };"
        "  "
        "  xhr.onerror = function() {"
        "    document.getElementById('status').innerText = 'Upload error';"
        "    document.querySelector('#uploadBtn').disabled = false;"
        "  };"
        "  "
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

        httpd_register_uri_handler(web_server, &root_uri);
        httpd_register_uri_handler(web_server, &release_uri);
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

// Comprehensive memory testing
static void test_memory_regions(void) {
    if (!swd_is_connected()) {
        ESP_LOGW(TAG, "SWD not connected for memory testing");
        return;
    }
    
    ESP_LOGI(TAG, "=== Comprehensive Memory Test ===");
    
    uint32_t data;
    
    // Test Flash regions
    ESP_LOGI(TAG, "--- Flash Memory Test ---");
    uint32_t flash_addrs[] = {
        0x00000000,  // Start of flash (reset vector)
        0x00001000,  // Typical bootloader location
        0x00010000,  // Application start
        0x000FC000,  // Near end of 1MB flash
    };
    
    for (int i = 0; i < 4; i++) {
        if (swd_mem_read32(flash_addrs[i], &data) == ESP_OK) {
            ESP_LOGI(TAG, "Flash[0x%08lX] = 0x%08lX", flash_addrs[i], data);
        } else {
            ESP_LOGE(TAG, "Failed to read Flash[0x%08lX]", flash_addrs[i]);
        }
    }
    
    // Test RAM regions
    ESP_LOGI(TAG, "--- RAM Memory Test ---");
    uint32_t ram_addrs[] = {
        0x20000000,  // Start of RAM
        0x20000100,  // Safe test area
        0x20001000,  // 4KB into RAM
        0x2003FF00,  // Near end of 256KB RAM
    };
    
    for (int i = 0; i < 4; i++) {
        if (swd_mem_read32(ram_addrs[i], &data) == ESP_OK) {
            ESP_LOGI(TAG, "RAM[0x%08lX] = 0x%08lX", ram_addrs[i], data);
            
            // Try write test on safe area only
            if (ram_addrs[i] == 0x20000100) {
                uint32_t test_patterns[] = {0xDEADBEEF, 0x12345678, 0xAAAA5555};
                for (int j = 0; j < 3; j++) {
                    if (swd_mem_write32(ram_addrs[i], test_patterns[j]) == ESP_OK) {
                        uint32_t readback;
                        if (swd_mem_read32(ram_addrs[i], &readback) == ESP_OK) {
                            if (readback == test_patterns[j]) {
                                ESP_LOGI(TAG, "  ✓ Pattern 0x%08lX verified", test_patterns[j]);
                            } else {
                                ESP_LOGE(TAG, "  ✗ Pattern failed: wrote 0x%08lX, read 0x%08lX", 
                                        test_patterns[j], readback);
                            }
                        }
                    }
                }
                // Restore original
                swd_mem_write32(ram_addrs[i], data);
            }
        } else {
            ESP_LOGE(TAG, "Failed to read RAM[0x%08lX]", ram_addrs[i]);
        }
    }
    
    // Test Peripheral regions
    ESP_LOGI(TAG, "--- Peripheral Memory Test ---");
    struct {
        uint32_t addr;
        const char *name;
    } periph_regs[] = {
        {0x40000000, "CLOCK"},
        {0x40001000, "RADIO"},
        {0x40002000, "UARTE0"},
        {0x40003000, "SPIM0/SPIS0/TWIM0/TWIS0"},
        {0x4001E000, "NVMC"},
        {0x40024000, "SPIM2/SPIS2"},
        {0x4002D000, "USBD"},
        {0x50000000, "GPIO P0"},
        {0x50000300, "GPIO P1"},
    };
    
    for (int i = 0; i < 9; i++) {
        if (swd_mem_read32(periph_regs[i].addr, &data) == ESP_OK) {
            ESP_LOGI(TAG, "%s[0x%08lX] = 0x%08lX", 
                    periph_regs[i].name, periph_regs[i].addr, data);
        }
    }
    
    // Read Device ID and info
    ESP_LOGI(TAG, "--- Device Information ---");
    uint32_t deviceid[2];
    if (swd_mem_read32(FICR_DEVICEID0, &deviceid[0]) == ESP_OK &&
        swd_mem_read32(FICR_DEVICEID1, &deviceid[1]) == ESP_OK) {
        ESP_LOGI(TAG, "Device ID: 0x%08lX%08lX", deviceid[1], deviceid[0]);
    }
    
    // Read MAC address
    uint32_t mac[2];
    if (swd_mem_read32(FICR_DEVICEADDR0, &mac[0]) == ESP_OK &&
        swd_mem_read32(FICR_DEVICEADDR1, &mac[1]) == ESP_OK) {
        ESP_LOGI(TAG, "BLE MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                (uint8_t)(mac[1] >> 8), (uint8_t)mac[1],
                (uint8_t)(mac[0] >> 24), (uint8_t)(mac[0] >> 16),
                (uint8_t)(mac[0] >> 8), (uint8_t)mac[0]);
    }
    
    ESP_LOGI(TAG, "=== Memory Test Complete ===");
}

// Enhanced WiFi connection with LR and normal fallback


// Test SWD functions
static void test_swd_functions(void) {
    if (!swd_is_connected()) {
        ESP_LOGW(TAG, "SWD not connected for testing");
        return;
    }
    
    ESP_LOGI(TAG, "=== SWD Function Test ===");
    
    uint32_t data;
    
    // Read device info
    if (swd_mem_read32(FICR_INFO_PART, &data) == ESP_OK) {
        ESP_LOGI(TAG, "Part Number: 0x%08lX (nRF52840)", data);
    }
    
    if (swd_mem_read32(FICR_INFO_RAM, &data) == ESP_OK) {
        ESP_LOGI(TAG, "RAM Size: %lu KB", data);
    }
    
    if (swd_mem_read32(FICR_INFO_FLASH, &data) == ESP_OK) {
        ESP_LOGI(TAG, "Flash Size: %lu KB", data);
    }
    
    // Check protection
    if (swd_mem_read32(UICR_APPROTECT, &data) == ESP_OK) {
        if (data == 0xFFFFFF5A) {
            ESP_LOGI(TAG, "APPROTECT: 0x%08lX (DISABLED - Good!)", data);
        } else {
            ESP_LOGW(TAG, "APPROTECT: 0x%08lX (ENABLED - Flash operations restricted)", data);
        }
    }
    
    ESP_LOGI(TAG, "=== SWD Test Complete ===");
}

// SWD connection with retry logic
static esp_err_t try_swd_connection(void) {
    ESP_LOGI(TAG, "=== Starting SWD Connection Attempt ===");
    
    if (swd_initialized && swd_is_connected()) {
        ESP_LOGI(TAG, "SWD already connected");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Attempting SWD connection...");
    
    if (!swd_initialized) {
        ESP_LOGI(TAG, "Initializing SWD interface...");
        
        swd_config_t swd_cfg = {
            .pin_swclk = SWD_PIN_SWCLK,
            .pin_swdio = SWD_PIN_SWDIO,
            .pin_reset = SWD_PIN_RESET,
            .delay_cycles = 0
        };
        
        esp_err_t ret = swd_init(&swd_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SWD init failed: %s (0x%x)", esp_err_to_name(ret), ret);
            return ret;
        }
        ESP_LOGI(TAG, "SWD interface initialized");
        swd_initialized = true;
    }
    
    ESP_LOGI(TAG, "Trying direct connection...");
    esp_err_t ret = swd_connect();
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Direct connect failed, trying reset...");
        ret = swd_reset_target();
        if (ret == ESP_OK) {
            ret = swd_connect();
        }
    }
    
    if (ret == ESP_OK) {
        xEventGroupSetBits(system_events, SWD_CONNECTED_BIT);
        ESP_LOGI(TAG, "✓ SWD connected successfully!");
        
        uint32_t idcode = swd_get_idcode();
        ESP_LOGI(TAG, "Target IDCODE: 0x%08lX", idcode);
        
        ret = swd_flash_init();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Flash init failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Flash interface initialized");
        }
        
        test_swd_functions();
        test_memory_regions();  // Run comprehensive memory test on connection
        ESP_LOGI(TAG, "Initial test complete, shutting down SWD to release target...");
        swd_shutdown();
        xEventGroupClearBits(system_events, SWD_CONNECTED_BIT);
        ESP_LOGI(TAG, "SWD shutdown - target released for normal operation");
    } else {
        xEventGroupClearBits(system_events, SWD_CONNECTED_BIT);
        ESP_LOGE(TAG, "✗ SWD connection failed with error: 0x%x", ret);
        swd_shutdown();
    }

    if (ret == ESP_OK) {
    // Check APPROTECT status
        uint32_t approtect;
        if (swd_mem_read32(UICR_APPROTECT, &approtect) == ESP_OK) {
            if (approtect == 0xFFFFFFFF) {
                ESP_LOGW(TAG, "APPROTECT is in erased state (protected on nRF52840)");
                ESP_LOGI(TAG, "Consider using 'Disable APPROTECT' before flashing");
            } else if (approtect == 0xFFFFFF5A) {
                ESP_LOGI(TAG, "APPROTECT is disabled (good for flashing)");
            } else {
                ESP_LOGW(TAG, "APPROTECT has unexpected value: 0x%08lX", approtect);
            }
        }
    }



    ESP_LOGI(TAG, "=== SWD Connection Attempt Complete ===");
    return ret;
}

// System health monitoring task
static void system_health_task(void *arg) {
    ESP_LOGI(TAG, "System health task started");
    
    system_health_t health;
    
    // Do initial system check
    power_get_health_status(&health);
    ESP_LOGI(TAG, "Initial Health: SWD=%d Flash=%d Net=%d Errors=%lu",
            health.swd_failures, health.flash_failures, 
            health.network_failures, error_count);
    
    size_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "Heap: free=%d", free_heap);
    
    // Now just monitor for critical issues
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));  // Check every 5 seconds
        
        // Only check for critical heap issues
        free_heap = esp_get_free_heap_size();
        if (free_heap < 20000) {
            ESP_LOGW(TAG, "Low memory warning: %d bytes", free_heap);
        }
    }
}

// Critical error handler
__attribute__((unused))
static void handle_critical_error(const char *context, esp_err_t error) {
    ESP_LOGE(TAG, "Critical error in %s: %s", context, esp_err_to_name(error));
    recovery_count++;
    
    char error_msg[128];
    snprintf(error_msg, sizeof(error_msg), "%s: %s", context, esp_err_to_name(error));
    power_log_error(error_msg);
    
    if (recovery_count > 3) {
        ESP_LOGE(TAG, "Too many recovery attempts");
        xEventGroupSetBits(system_events, RECOVERY_MODE_BIT);
    }
}


// System initialization
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

    init_config();
    system_events = xEventGroupCreate();

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
        // .max_retry_count removed - no longer used
        .error_cooldown_ms = 1000
    };
    ESP_ERROR_CHECK(power_mgmt_init(&power_cfg));

    wake_reason_t wake_reason = power_get_wake_reason();
    ESP_LOGI(TAG, "Wake reason: %d", wake_reason);

    ESP_LOGI(TAG, "Initializing SWD connection...");
    try_swd_connection();

    xTaskCreate(system_health_task, "health", 4096, NULL, 5, NULL);
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


// Main application entry
void app_main(void) {
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "Mesh Radio Flasher v%s", DEVICE_VERSION);
    ESP_LOGI(TAG, "Build: %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "=================================");

    // Check and log wake reason
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    if (wake_cause == ESP_SLEEP_WAKEUP_TIMER) {
        wake_count = power_get_wake_count() + 1;
        ESP_LOGI(TAG, "=== Woke from deep sleep (count: %lu) ===", wake_count);
    } else {
        wake_count = 0;
        ESP_LOGI(TAG, "=== Fresh boot (power on) ===");
    }

    // Initialize system
    init_system();

    // Restore state if waking from deep sleep
    if (wake_cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "Restoring state from deep sleep...");
        power_restore_from_deep_sleep();
    }

    // Check battery status
    battery_status_t battery;
    power_get_battery_status(&battery);
    ESP_LOGI(TAG, "Battery: %.2fV (%.0f%%) - %s",
            battery.voltage, battery.percentage,
            battery.is_critical ? "CRITICAL" :
            battery.is_low ? "LOW" :
            battery.voltage > BATTERY_HIGH_THRESHOLD ? "HIGH" : "NORMAL");

    // Critical battery protection
    if (battery.voltage < BATTERY_CRITICAL_THRESHOLD && ENABLE_BATTERY_PROTECTION) {
        ESP_LOGW(TAG, "!!! Critical battery (%.2fV) - entering extended deep sleep (%d sec) !!!",
                battery.voltage, DEEP_SLEEP_CRITICAL_SEC);
        power_target_off();  // Turn off nRF52 to save power
        power_enter_adaptive_deep_sleep();
        // Never returns
    }

    // Low battery nRF52 protection
    if (battery.voltage < NRF52_POWER_OFF_VOLTAGE && ENABLE_BATTERY_PROTECTION) {
        if (power_target_is_on()) {
            ESP_LOGW(TAG, "Low battery (%.2fV) - turning off nRF52 radio", battery.voltage);
            power_target_off();
        }
    }

    // Initialize WiFi manager
    ESP_LOGI(TAG, "Initializing WiFi manager...");
    ESP_ERROR_CHECK(wifi_manager_init());

    // Attempt WiFi connection (tries LR first, then normal)
    ESP_LOGI(TAG, "Attempting WiFi connection...");
    if (wifi_manager_connect() != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connection failed - entering deep sleep");
        power_enter_adaptive_deep_sleep();
        // Never returns
    }

    // WiFi connected successfully
    ESP_LOGI(TAG, "✓ WiFi connected successfully");
    const char* ip = wifi_manager_get_ip();
    ESP_LOGI(TAG, "IP Address: %s", ip);
    ESP_LOGI(TAG, "Mode: %s", power_get_wifi_is_lr() ? "ESP-LR" : "Normal");
    ESP_LOGI(TAG, "SSID: %s", power_get_wifi_ssid());

    // Update global IP
    device_ip = (char*)ip;

    // Initialize SPIFFS storage
    ESP_LOGI(TAG, "Initializing storage...");
    init_storage();

    // Start web server
    ESP_LOGI(TAG, "Starting web server...");
    esp_err_t server_result = start_webserver();
    if (server_result == ESP_OK) {
        ESP_LOGI(TAG, "✓ Web server started on port 80");
        ESP_LOGI(TAG, "=================================");
        ESP_LOGI(TAG, "Web interface: http://%s", device_ip);
        ESP_LOGI(TAG, "=================================");
    } else {
        ESP_LOGE(TAG, "✗ Failed to start web server: %s", esp_err_to_name(server_result));
    }

    // Initialize SWD interface (optional - for initial testing)
    ESP_LOGI(TAG, "Testing SWD connection...");
    try_swd_connection();

    // System ready
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "=== System Ready ===");
    ESP_LOGI(TAG, "Web interface: http://%s", device_ip);
    ESP_LOGI(TAG, "Power: nRF52 is %s", power_target_is_on() ? "ON" : "OFF");
    ESP_LOGI(TAG, "WiFi: %s mode", power_get_wifi_is_lr() ? "ESP-LR" : "Normal");
    ESP_LOGI(TAG, "=================================");

    // Main monitoring loop
    TickType_t last_status = xTaskGetTickCount();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));  // Check every second

        // Log status every 10 seconds
        if ((xTaskGetTickCount() - last_status) > pdMS_TO_TICKS(10000)) {
            battery_status_t batt;
            power_get_battery_status(&batt);

            ESP_LOGI(TAG, "Status: Battery=%.2fV (%.0f%%) | WiFi=%s | Wake=%lu | nRF52=%s",
                    batt.voltage,
                    batt.percentage,
                    wifi_manager_is_connected() ? "Connected" : "Disconnected",
                    power_get_wake_count(),
                    power_target_is_on() ? "ON" : "OFF");

            // Check for low battery condition
            if (batt.voltage < NRF52_POWER_OFF_VOLTAGE &&
                power_target_is_on() &&
                ENABLE_BATTERY_PROTECTION) {
                ESP_LOGW(TAG, "Battery dropped below %.2fV - turning off nRF52",
                        NRF52_POWER_OFF_VOLTAGE);
                power_target_off();
            }

            last_status = xTaskGetTickCount();
        }
    }
}