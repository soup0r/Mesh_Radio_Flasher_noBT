# Mesh Radio Flasher (No Bluetooth)

A resilient, production-grade SWD flasher for mesh radio devices (nRF52840/nRF52833), running on Seeed XIAO ESP32C3. This version has all Bluetooth functionality removed for simplified operation and reduced memory usage.

## Features

- **SWD Programming**: Full read/write/erase capability for nRF52 series flash
- **Web Interface**: Modern UI with Power Control and Flashing tabs
- **Power Management**: Battery monitoring and target power control
- **Safety Features**: APPROTECT handling and secure flashing operations
- **No Bluetooth**: Simplified codebase with BLE functionality completely removed

## Quick Start

1. **Configure WiFi**: Copy `main/config.h.template` to `main/config.h` and update your WiFi credentials:
```bash
cp main/config.h.template main/config.h
# Edit main/config.h with your WiFi SSID and password
```

2. **Install PlatformIO**:
```bash
pip install platformio
```

3. **Build and upload**:
```bash
pio run -t upload
```

4. **Monitor output**:
```bash
pio run -t monitor
```

5. **Access Web Interface**: Connect to the IP address shown in the serial output

## Documentation

See [docs/](docs/) for detailed documentation.

## License

MIT License
