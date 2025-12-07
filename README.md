# ESP32-C3 Garage Door Opener

Custom PlatformIO firmware for ESP32-C3 based garage door opener, providing functionality similar to ESPHome with built-in WiFi configuration, web UI, OTA updates, and REST API.

## Features

### Core Functionality
- **WiFi Manager**: Automatically connects to saved WiFi network, falls back to AP mode when disconnected
- **Captive Portal**: Easy WiFi configuration through captive portal in AP mode
- **Web Interface**: Beautiful, responsive web UI with tabbed interface for monitoring and control
- **OTA Updates**: Web-based firmware upload with progress tracking (both HTTP and ArduinoOTA)
- **Real-time Logging**: WebSocket-based serial log viewer in web interface
- **REST API**: RESTful API for integration with home automation systems
- **GPIO Control**: Relay control, contact sensor, status LED, and physical button support
- **Persistent Storage**: Configuration saved to NVMe (Preferences API)

### Hardware Support
- **Contact Sensor** (GPIO4): Monitors garage door open/closed status
- **Relay Output** (GPIO5): Triggers garage door opener (1-second pulse)
- **Status LED** (GPIO12): Visual feedback (inverted logic)
- **Physical Button** (GPIO14): Manual control with multi-click support
  - Short press: Trigger door
  - Long press (4+ seconds): Factory reset

### Web Interface Features
- **Control Tab**:
  - Real-time door status display
  - Manual door trigger button
  - Device information (IP, WiFi signal, uptime)
  - Auto-refresh status every 2 seconds
- **Logs Tab**:
  - Real-time serial log viewer (WebSocket streaming)
  - Color-coded log levels (INFO, WARN, ERROR, DEBUG)
  - Download logs to file
  - Clear log display
- **OTA Update Tab**:
  - Drag-and-drop firmware upload
  - File selection dialog
  - Real-time upload progress bar
  - Automatic device restart after upload
- **Settings Tab**:
  - WiFi configuration interface
  - Device restart button
- Responsive design (mobile-friendly)

## Hardware Requirements

- **ESP32-C3** development board (e.g., ESP32-C3-DevKitM-1)
- **Relay module** (connected to GPIO5)
- **Contact sensor** / magnetic reed switch (connected to GPIO4)
- **LED** (optional, connected to GPIO12 with resistor)
- **Push button** (optional, connected to GPIO14 with internal pullup)

## Pin Configuration

| Pin | Function | Notes |
|-----|----------|-------|
| GPIO4 | Contact Sensor | Input, inverted logic |
| GPIO5 | Relay Control | Output, active HIGH, 1s pulse |
| GPIO12 | Status LED | Output, inverted (LOW=ON) |
| GPIO14 | Button | Input with internal pullup |

## Installation

### Prerequisites
- [PlatformIO](https://platformio.org/) installed
- USB cable to connect ESP32-C3 to computer

### Build and Upload

1. Clone or navigate to this directory:
```bash
cd C:\Arduino\AthomGarageOpener
```

2. Build the project:
```bash
pio run
```

3. Upload to ESP32-C3:
```bash
pio run --target upload
```

4. Monitor serial output (optional):
```bash
pio device monitor
```

## First Time Setup

1. **Power on the device** - It will start in AP mode (no WiFi configured)

2. **Connect to AP**:
   - SSID: `GarageDoor-Setup`
   - Password: None (open network)

3. **Configure WiFi**:
   - Your device should automatically open the captive portal
   - If not, navigate to `http://192.168.4.1`
   - Click "Configure WiFi"
   - Enter your WiFi credentials
   - Click "Save & Restart"

4. **Access the device** on your network:
   - Find the IP address in your router's DHCP table
   - Or check serial monitor output
   - Navigate to `http://<device-ip>`

## OTA Firmware Updates

The device supports two methods for OTA (Over-The-Air) firmware updates:

### Method 1: Web Interface Upload (Recommended)

1. Build your firmware:
   ```bash
   pio run
   ```

2. Locate the compiled firmware file:
   - Path: `.pio/build/esp32-c3/firmware.bin`

3. Open the web interface and navigate to the **OTA Update** tab

4. Upload the firmware:
   - Click the upload area or drag and drop the `.bin` file
   - Monitor the upload progress
   - Device will automatically restart after successful upload

### Method 2: ArduinoOTA (For Development)

When connected to WiFi (not in AP mode), the device advertises itself for ArduinoOTA updates:

```bash
# Using PlatformIO
pio run --target upload --upload-port garage-door.local

# Or specify IP address
pio run --target upload --upload-port 192.168.1.100
```

**Note**: ArduinoOTA is disabled when device is in AP mode for security reasons.

### Building New Firmware

After making code changes:

```bash
# Build only
pio run

# Build and upload via serial
pio run --target upload

# Build and upload via OTA (when on network)
pio run --target upload --upload-port <device-ip>
```

## Viewing Logs

### Web Interface (Real-time)

1. Navigate to the **Logs** tab in the web interface
2. View real-time serial output with color-coded log levels:
   - 🟢 **INFO**: Green - General information
   - 🟠 **WARN**: Orange - Warnings
   - 🔴 **ERROR**: Red - Errors
   - 🔵 **DEBUG**: Blue - Debug messages
3. Use **Clear Display** to clear the log view (doesn't affect buffered logs)
4. Use **Download Logs** to save logs to a text file

### Serial Monitor

Traditional serial monitor access (115200 baud):

```bash
# PlatformIO
pio device monitor

# Or specify port
pio device monitor --port COM3  # Windows
pio device monitor --port /dev/ttyUSB0  # Linux
```

### Log Buffer

- Device maintains a circular buffer of last 100 log messages
- Logs are stored in RAM and cleared on restart
- New WebSocket connections receive all buffered logs
- All log messages are also sent to serial output

## REST API

### Get Status
```http
GET /api/status
```

**Response:**
```json
{
  "door_open": true,
  "wifi_connected": true,
  "ip_address": "192.168.1.100",
  "rssi": -45,
  "uptime": 3600,
  "ssid": "MyWiFi"
}
```

### Trigger Door
```http
POST /api/trigger
```

**Response:**
```json
{
  "success": true,
  "message": "Door triggered"
}
```

### Configure WiFi
```http
POST /api/config
Content-Type: application/json

{
  "ssid": "MyWiFi",
  "password": "MyPassword"
}
```

**Response:**
```json
{
  "success": true
}
```

### Restart Device
```http
POST /api/restart
```

**Response:**
```json
{
  "success": true
}
```

### Get Logs
```http
GET /api/logs
```

**Response:**
```json
{
  "logs": [
    {
      "timestamp": "00:05:23",
      "level": "INFO",
      "message": "WiFi connected!"
    },
    {
      "timestamp": "00:05:24",
      "level": "INFO",
      "message": "Web server started on port 80"
    }
  ]
}
```

### OTA Firmware Upload
```http
POST /update
Content-Type: multipart/form-data

(Binary firmware file)
```

**Response:**
- `200 OK` - Firmware uploaded successfully, device will restart
- `400 Bad Request` - Invalid firmware file
- `500 Internal Server Error` - Upload failed

### WebSocket Endpoint

Real-time log streaming:

```
ws://device-ip/ws
```

**Message Format:**
```json
{
  "type": "log",
  "timestamp": "00:05:23",
  "level": "INFO",
  "message": "Door status: OPEN"
}
```

## Usage Examples

### Home Assistant Integration

Add to `configuration.yaml`:

```yaml
rest_command:
  garage_door_trigger:
    url: "http://192.168.1.100/api/trigger"
    method: POST

sensor:
  - platform: rest
    name: "Garage Door Status"
    resource: "http://192.168.1.100/api/status"
    value_template: "{{ 'Open' if value_json.door_open else 'Closed' }}"
    json_attributes:
      - wifi_connected
      - rssi
      - uptime
    scan_interval: 5

cover:
  - platform: template
    covers:
      garage_door:
        friendly_name: "Garage Door"
        value_template: "{{ states('sensor.garage_door_status') == 'Closed' }}"
        open_cover:
          service: rest_command.garage_door_trigger
        close_cover:
          service: rest_command.garage_door_trigger
        stop_cover:
          service: rest_command.garage_door_trigger
```

### cURL Examples

```bash
# Get status
curl http://192.168.1.100/api/status

# Trigger door
curl -X POST http://192.168.1.100/api/trigger

# Configure WiFi
curl -X POST http://192.168.1.100/api/config \
  -H "Content-Type: application/json" \
  -d '{"ssid":"MyWiFi","password":"MyPassword"}'

# Restart device
curl -X POST http://192.168.1.100/api/restart
```

## Troubleshooting

### Cannot connect to WiFi
- Ensure WiFi credentials are correct
- Check if your WiFi network is 2.4GHz (ESP32-C3 doesn't support 5GHz)
- Try factory reset: Hold button for 4+ seconds

### Device not responding
- Check power supply (ESP32-C3 needs stable 3.3V)
- Check serial monitor for error messages
- Try restarting the device

### Contact sensor not working
- Verify wiring to GPIO4
- Check sensor polarity
- Sensor logic is inverted by default (adjust `statusInverted` in code if needed)

### Cannot access web interface
- Check if device is in AP mode (look for `GarageDoor-Setup` network)
- Verify IP address (check serial monitor or router DHCP)
- Try accessing via mDNS: `http://garage-door.local` (may not work on all networks)

## Factory Reset

Two methods:

1. **Physical button**: Hold button on GPIO14 for 4+ seconds
2. **Web interface**: Not available (for security)

After factory reset, device will:
- Clear all saved WiFi credentials
- Restart in AP mode
- Return to default settings

## Development

### Project Structure
```
AthomGarageOpener/
├── platformio.ini          # PlatformIO configuration
├── src/
│   └── main.cpp            # Main firmware code
├── data/                   # (Empty - web UI embedded in code)
├── athom-garage-door.yaml  # Original ESPHome config (reference)
└── README.md               # This file
```

### Customization

#### Change AP SSID/Password
Edit in `src/main.cpp`:
```cpp
#define AP_SSID "GarageDoor-Setup"
#define AP_PASSWORD ""
```

#### Adjust GPIO Pins
Edit pin definitions in `src/main.cpp`:
```cpp
#define CONTACT_PIN 4
#define RELAY_PIN 5
#define LED_PIN 12
#define BUTTON_PIN 14
```

#### Change Relay Pulse Duration
Edit in `src/main.cpp`:
```cpp
#define RELAY_PULSE_TIME 1000  // milliseconds
```

#### Disable Status Inversion
Edit in `src/main.cpp`:
```cpp
bool statusInverted = false;  // Set to false
```

## Differences from ESPHome Version

This custom firmware provides similar functionality to the ESPHome version but with these differences:

### Advantages
- **Faster boot time**: No Python overhead
- **More control**: Full access to code and customization
- **Smaller binary**: More efficient compiled code
- **Standalone**: No dependency on ESPHome ecosystem
- **Web-based OTA updates**: Upload firmware directly through browser
- **Real-time logging**: WebSocket-based log viewer in web interface
- **Better UI**: Modern tabbed interface with responsive design

### Missing Features (vs ESPHome)
- No Home Assistant native API integration (use REST API instead)
- No OTA updates via ESPHome dashboard (but has web-based OTA and ArduinoOTA)
- No MQTT support (can be added if needed)
- No NTP time sync (uses uptime-based timestamps)
- No remote syslog (but has real-time web-based log viewer)

## Security Considerations

- **AP mode has no password** by default for easy setup (change if needed)
- **No authentication** on web interface or API (add if exposed to internet)
- **No HTTPS** (add if transmitting sensitive data)
- **OTA updates are unencrypted** - anyone on the network can upload firmware
- **No firmware signature verification** - device will accept any valid ESP32 firmware
- For production use, consider:
  - Adding HTTP authentication for web interface and OTA updates
  - Enabling HTTPS/TLS
  - Setting AP password
  - Implementing rate limiting
  - Adding OTA password (ArduinoOTA supports this)
  - Restricting OTA to specific IP addresses
  - Only expose device on trusted networks

## License

This project is provided as-is for personal and educational use.

## Credits

Based on the original ESPHome configuration by Athom Technology.
Custom firmware implementation for ESP32-C3 with PlatformIO.

## Support

For issues or questions:
1. Check the Troubleshooting section
2. Review serial monitor output
3. Verify hardware connections
4. Check PlatformIO documentation

## Version History

- **v1.1.0** (2024-11-12): Added OTA and logging features
  - Web-based OTA firmware upload with progress tracking
  - ArduinoOTA support for development
  - Real-time log viewer with WebSocket streaming
  - Circular log buffer (100 messages)
  - Color-coded log levels (INFO, WARN, ERROR, DEBUG)
  - Download logs to file
  - Tabbed web interface (Control, Logs, OTA Update, Settings)
  - Enhanced UI with better organization

- **v1.0.0** (2024-11-12): Initial release
  - WiFi manager with AP fallback
  - Captive portal
  - Web UI
  - REST API
  - GPIO control
  - Persistent configuration
#   G a r a g e D o o r O p e n e r  
 