#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <esp_err.h>
#include <esp_task_wdt.h>
#include <freertos/task.h>
#include <time.h>
#include <sntp.h>

// GPIO Pin Definitions (Athom ESP32-C3 garage door opener)
#define CONTACT_PIN 18      // Door contact sensor
#define RELAY_PIN 7         // Relay to trigger garage door
#define LED_PIN 4           // Status LED
#define BUTTON_PIN 3        // Physical button

// Configuration
#define AP_SSID "GarageDoor-Setup"
#define AP_PASSWORD ""  // No password for easy setup
#define CONFIG_NAMESPACE "garage"
#define DEBOUNCE_TIME 20
#define RELAY_PULSE_TIME 1000  // 1 second relay pulse
#define LOG_BUFFER_SIZE 100    // Number of log messages to keep
#define WATCHDOG_TIMEOUT_SECONDS 15
#define NTP_SERVER "pool.ntp.org"
#define TIMEZONE "AEST-10AEDT,M10.1.0,M4.1.0/3" // Australia/Sydney

// Log buffer (circular buffer)
struct LogEntry {
  String timestamp;
  String message;
  String level;
};

LogEntry logBuffer[LOG_BUFFER_SIZE];
int logIndex = 0;
int logCount = 0;

// Global objects
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer dnsServer;
Preferences preferences;
bool watchdogEnabled = false;

// State variables
String wifiSSID = "";
String wifiPassword = "";
bool apMode = false;
bool doorOpen = false;
unsigned long lastButtonPress = 0;
unsigned long buttonPressStart = 0;
bool buttonPressed = false;
unsigned long relayStartTime = 0;
bool relayActive = false;
bool statusInverted = true;  // From YAML config
String doorStatusTransition = "";  // "opening", "closing", or ""
unsigned long statusTransitionStartTime = 0;
unsigned long lastWiFiRetryTime = 0;
#define STATUS_TRANSITION_DURATION 15000  // 15 seconds
#define REGISTRATION_INTERVAL_MS (5 * 60 * 1000)  // 5 minutes
#define WIFI_RETRY_INTERVAL_MS 60000 // 60 seconds

// Status update tracking
unsigned long lastStatusUpdateTime = 0;
const unsigned long STATUS_UPDATE_INTERVAL = 1000;  // Send status every 1 second
bool lastDoorOpenState = false;

// Device Registration class
class DeviceRegistration {
private:
  Preferences* prefs;
  String serverUrl;
  String deviceName;
  String deviceType;
  String deviceDescription;
  bool registrationEnabled;
  unsigned long lastRegistrationTime;
  bool lastRegistrationSuccess;
  String lastRegistrationError;

public:
  DeviceRegistration(Preferences* preferences) : prefs(preferences),
                                                lastRegistrationTime(0),
                                                lastRegistrationSuccess(false),
                                                registrationEnabled(true) {}

  void loadSettings() {
    serverUrl = prefs->getString("reg_server", "http://192.168.1.225:3004");
    deviceName = prefs->getString("reg_name", "Garage-Door");
    deviceType = prefs->getString("reg_type", "esp32_garage_door");
    deviceDescription = prefs->getString("reg_desc", "ESP32-C3 Garage Door Opener");
    registrationEnabled = prefs->getBool("reg_enabled", true);
  }

  void saveSettings() {
    prefs->putString("reg_server", serverUrl);
    prefs->putString("reg_name", deviceName);
    prefs->putString("reg_type", deviceType);
    prefs->putString("reg_desc", deviceDescription);
    prefs->putBool("reg_enabled", registrationEnabled);
  }

  void updateSettings(const String& url, const String& name, const String& type,
                     const String& description, bool enabled) {
    serverUrl = url;
    deviceName = name;
    deviceType = type;
    deviceDescription = description;
    registrationEnabled = enabled;
    saveSettings();
  }

  String getSettingsJson() {
    DynamicJsonDocument doc(512);
    doc["server_url"] = serverUrl;
    doc["device_name"] = deviceName;
    doc["device_type"] = deviceType;
    doc["device_description"] = deviceDescription;
    doc["enabled"] = registrationEnabled;
    doc["last_success"] = lastRegistrationSuccess;
    doc["last_error"] = lastRegistrationError;

    if (lastRegistrationTime > 0) {
      unsigned long secondsAgo = (millis() - lastRegistrationTime) / 1000;
      doc["last_registration_seconds_ago"] = secondsAgo;
    } else {
      doc["last_registration_seconds_ago"] = -1;
    }

    String json;
    serializeJson(doc, json);
    return json;
  }

  bool registerDevice() {
    if (!registrationEnabled) {
      return false;
    }

    if (WiFi.status() != WL_CONNECTED) {
      lastRegistrationSuccess = false;
      lastRegistrationError = "WiFi not connected";
      return false;
    }

    HTTPClient http;
    String registrationUrl = serverUrl;
    if (!registrationUrl.endsWith("/")) {
      registrationUrl += "/";
    }
    registrationUrl += "api/smart_devices/register";

    http.begin(registrationUrl);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(10000);

    DynamicJsonDocument doc(1536);
    
    // Basic device info
    doc["name"] = deviceName;
    doc["ip"] = WiFi.localIP().toString();
    doc["mac"] = WiFi.macAddress();
    doc["hostname"] = deviceName;
    doc["type"] = deviceType;
    doc["description"] = deviceDescription;
    
    // Device capabilities
    JsonArray capabilities = doc.createNestedArray("capabilities");
    
    // Door status sensor (binary_sensor)
    JsonObject doorCap = capabilities.createNestedObject();
    doorCap["identifier"] = "door";
    doorCap["name"] = "Door";
    doorCap["type"] = "binary_sensor";
    doorCap["valueType"] = "boolean";
    doorCap["description"] = "Door open/closed status";
    
    // Trigger capability (switch)
    JsonObject triggerCap = capabilities.createNestedObject();
    triggerCap["identifier"] = "trigger";
    triggerCap["name"] = "Trigger";
    triggerCap["type"] = "switch";
    triggerCap["valueType"] = "boolean";
    triggerCap["description"] = "Trigger garage door opener";
    
    // Control API for trigger
    JsonObject triggerApi = triggerCap.createNestedObject("controlApi");
    triggerApi["method"] = "POST";
    triggerApi["endpoint"] = "/api/trigger";
    JsonArray triggerActions = triggerApi.createNestedArray("actions");
    triggerActions.add("on");

    String jsonPayload;
    serializeJson(doc, jsonPayload);

    Serial.println("📡 Attempting device registration...");
    Serial.print("URL: ");
    Serial.println(registrationUrl);
    Serial.print("Payload size: ");
    Serial.println(jsonPayload.length());

    int httpResponseCode = http.POST(jsonPayload);

    if (httpResponseCode > 0) {
      // Check response size to prevent memory exhaustion
      int responseSize = http.getSize();
      if (responseSize > 4096) {  // Maximum 4KB response
        lastRegistrationSuccess = false;
        lastRegistrationError = "Response too large";
        Serial.print("❌ Response too large: ");
        Serial.print(responseSize);
        Serial.println(" bytes");
        http.end();
        return false;
      }
      
      String response = http.getString();
      Serial.print("✅ HTTP Response Code: ");
      Serial.println(httpResponseCode);
      Serial.print("Response: ");
      Serial.println(response);
      
      DynamicJsonDocument responseDoc(1024);
      DeserializationError error = deserializeJson(responseDoc, response);

      if (!error) {
        bool success = responseDoc["success"] | false;
        String message = responseDoc["message"] | "";

        if (success) {
          lastRegistrationSuccess = true;
          lastRegistrationError = "";
          lastRegistrationTime = millis();
          Serial.println("✅ Device registered successfully!");
          http.end();
          return true;
        } else {
          lastRegistrationSuccess = false;
          lastRegistrationError = message;
          Serial.print("❌ Registration failed: ");
          Serial.println(message);
        }
      } else {
        lastRegistrationSuccess = false;
        lastRegistrationError = "Invalid JSON response";
        Serial.println("❌ Invalid JSON response from server");
      }
    } else {
      lastRegistrationSuccess = false;
      // Build error message without concatenation
      char errorBuf[64];
      snprintf(errorBuf, sizeof(errorBuf), "HTTP error: %d", httpResponseCode);
      lastRegistrationError = errorBuf;
      Serial.print("❌ HTTP error: ");
      Serial.println(httpResponseCode);
    }

    lastRegistrationTime = millis();
    http.end();
    return false;
  }

  bool isRegistrationDue() {
    if (!registrationEnabled) {
      return false;
    }
    if (lastRegistrationTime == 0) {
      return true;
    }
    unsigned long timeSinceLastRegistration = millis() - lastRegistrationTime;
    return timeSinceLastRegistration >= REGISTRATION_INTERVAL_MS;
  }

  void checkAndRegister() {
    if (isRegistrationDue()) {
      registerDevice();
    }
  }

  void forceRegister() {
    lastRegistrationTime = 0;
    registerDevice();
  }

  // Send status update to server
  bool sendStatusUpdate(bool doorOpen, String transition) {
    if (!registrationEnabled) {
      return false;
    }

    if (WiFi.status() != WL_CONNECTED) {
      return false;
    }

    HTTPClient http;

    // Construct status update URL
    String statusUrl = serverUrl;
    if (!statusUrl.endsWith("/")) {
      statusUrl += "/";
    }
    statusUrl += "api/smart_devices/status_update";

    // Begin HTTP connection
    http.begin(statusUrl);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000); // 5 second timeout

    // Create status JSON payload
    DynamicJsonDocument doc(512);
    doc["mac"] = WiFi.macAddress();
    doc["door"] = doorOpen;
    doc["door_transition"] = transition;
    doc["timestamp"] = millis();

    String jsonPayload;
    serializeJson(doc, jsonPayload);

    // Send POST request
    int httpResponseCode = http.POST(jsonPayload);

    // Check response
    if (httpResponseCode > 0) {
      http.end();
      return true;
    }

    http.end();
    return false;
  }

  String getServerUrl() const { return serverUrl; }
  String getDeviceName() const { return deviceName; }
  String getDeviceType() const { return deviceType; }
  String getDeviceDescription() const { return deviceDescription; }
  bool isEnabled() const { return registrationEnabled; }
  bool getLastSuccess() const { return lastRegistrationSuccess; }
  String getLastError() const { return lastRegistrationError; }
};

// Global registration instance
DeviceRegistration* deviceRegistration = nullptr;

// Function declarations
void setupWiFi();
void setupWebServer();
void setupGPIO();
void setupOTA();
void loadConfiguration();
void saveConfiguration();
void handleButton();
void handleRelay();
void updateDoorStatus();
void triggerRelay();
void handleStatusTransition();
void logMessage(String level, String message);
void sendLogToWebSocket(String level, String message);
void broadcastStatusUpdate();
String getFormattedTime();
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
void configureWatchdog(uint32_t timeoutSeconds);
inline void feedWatchdog();
void disableWatchdog();
void checkWiFiConnection();

void setup() {
  Serial.begin(115200);
  delay(1000);

  disableWatchdog();

  logMessage("INFO", "=== Athom Garage Door Opener ===");
  logMessage("INFO", "Version: 1.1.0 (OTA + Logs)");
  logMessage("INFO", "Starting initialization...");
  yield();
  feedWatchdog();

  logMessage("INFO", "Initializing preferences...");
  preferences.begin(CONFIG_NAMESPACE, false);
  yield();
  feedWatchdog();

  logMessage("INFO", "Loading configuration...");
  loadConfiguration();
  delay(100);
  yield();
  feedWatchdog();

  // Setup GPIO
  logMessage("INFO", "Setting up GPIO...");
  yield();
  setupGPIO();
  delay(100);
  yield();
  feedWatchdog();
  logMessage("INFO", "GPIO setup complete");

  // Setup WiFi
  logMessage("INFO", "Setting up WiFi...");
  yield();
  setupWiFi();
  delay(100);
  yield();
  feedWatchdog();
  logMessage("INFO", "WiFi setup complete");

  // Setup OTA
  logMessage("INFO", "Setting up OTA...");
  yield();
  setupOTA();
  delay(100);
  yield();
  feedWatchdog();
  logMessage("INFO", "OTA setup complete");

  // Setup web server
  logMessage("INFO", "Starting web server...");
  yield();
  setupWebServer();
  delay(100);
  yield();
  feedWatchdog();
  logMessage("INFO", "Web server setup complete");

  // Initialize Device Registration
  if (!apMode) {
    logMessage("INFO", "Initializing device registration...");
    deviceRegistration = new DeviceRegistration(&preferences);
    deviceRegistration->loadSettings();
    logMessage("INFO", "Device registration initialized");
    
    // Register device on startup
    if (deviceRegistration->isEnabled()) {
      logMessage("INFO", "Registering device with control server...");
      deviceRegistration->registerDevice();
    }
  }

  configureWatchdog(WATCHDOG_TIMEOUT_SECONDS);

  logMessage("INFO", "Setup complete!");
  // Build IP message without concatenation
  char ipMsg[64];
  IPAddress ip = apMode ? WiFi.softAPIP() : WiFi.localIP();
  snprintf(ipMsg, sizeof(ipMsg), "IP Address: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  logMessage("INFO", ipMsg);
}

void loop() {
  feedWatchdog();

  if (apMode) {
    dnsServer.processNextRequest();
    checkWiFiConnection();
  } else {
    ArduinoOTA.handle();
  }

  ws.cleanupClients();
  
  // Aggressive WebSocket cleanup to prevent memory leaks from dead clients
  static unsigned long lastWsCleanup = 0;
  if (millis() - lastWsCleanup > 30000) {  // Every 30 seconds
    ws.cleanupClients(0);  // More aggressive cleanup with 0 timeout
    lastWsCleanup = millis();
  }

  handleButton();
  handleRelay();
  updateDoorStatus();
  handleStatusTransition();

  // Send status update to server periodically or when state changes (only in station mode and after successful registration)
  if (deviceRegistration != nullptr && !apMode && WiFi.status() == WL_CONNECTED && deviceRegistration->getLastSuccess()) {
    unsigned long currentTime = millis();
    bool stateChanged = (doorOpen != lastDoorOpenState);
    
    if (stateChanged || (currentTime - lastStatusUpdateTime >= STATUS_UPDATE_INTERVAL)) {
      deviceRegistration->sendStatusUpdate(doorOpen, doorStatusTransition);
      lastStatusUpdateTime = currentTime;
      lastDoorOpenState = doorOpen;
    }
  }

  // Check and perform device registration if due (every 5 minutes)
  if (!apMode && deviceRegistration != nullptr) {
    deviceRegistration->checkAndRegister();
  }

  yield();
  delay(10);
}

void setupGPIO() {
  // Contact sensor
  pinMode(CONTACT_PIN, INPUT);
  delay(10);
  yield();

  // Relay (active high, pulse mode)
  pinMode(RELAY_PIN, OUTPUT);
  delay(10);
  digitalWrite(RELAY_PIN, LOW);
  delay(10);
  yield();

  // Status LED (inverted - LOW = ON)
  pinMode(LED_PIN, OUTPUT);
  delay(10);
  digitalWrite(LED_PIN, HIGH);  // OFF initially
  delay(10);
  yield();

  // Button (internal pullup)
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  delay(10);
  yield();

  logMessage("INFO", "GPIO initialized");
}

void loadConfiguration() {
  wifiSSID = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");

  logMessage("INFO", "Configuration loaded");
  if (wifiSSID.length() > 0) {
    // Build message without concatenation
    char ssidMsg[128];
    snprintf(ssidMsg, sizeof(ssidMsg), "Saved SSID: %s", wifiSSID.c_str());
    logMessage("INFO", ssidMsg);
  } else {
    logMessage("WARN", "No WiFi credentials saved");
  }
}

void saveConfiguration() {
  preferences.putString("ssid", wifiSSID);
  preferences.putString("password", wifiPassword);
  logMessage("INFO", "Configuration saved");
}

void setupWiFi() {
  // Load device name from preferences for hostname
  String savedDeviceName = preferences.getString("reg_name", "Garage-Door");
  
  // Sanitize device name for hostname (remove spaces and special chars)
  String sanitizedHostname = savedDeviceName;
  sanitizedHostname.replace(" ", "-");
  sanitizedHostname.replace("_", "-");
  sanitizedHostname.toLowerCase();
  
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(sanitizedHostname.c_str());

  if (wifiSSID.length() > 0) {
    char connectMsg[128];
    snprintf(connectMsg, sizeof(connectMsg), "Connecting to WiFi: %s", wifiSSID.c_str());
    logMessage("INFO", connectMsg);

    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

    // Try to connect for 10 seconds (reduced from 20)
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      yield();
      feedWatchdog();
      Serial.print(".");
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));  // Blink LED
      attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      logMessage("INFO", "WiFi connected!");
      // Build messages without concatenation
      char ipMsg[64];
      IPAddress ip = WiFi.localIP();
      snprintf(ipMsg, sizeof(ipMsg), "IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
      logMessage("INFO", ipMsg);
      
      char rssiMsg[64];
      snprintf(rssiMsg, sizeof(rssiMsg), "Signal: %d dBm", WiFi.RSSI());
      logMessage("INFO", rssiMsg);
      digitalWrite(LED_PIN, HIGH);  // LED OFF (inverted)
      apMode = false;
      
      // Init time
      configTzTime(TIMEZONE, NTP_SERVER);
      
      return;
    } else {
      logMessage("ERROR", "WiFi connection failed");
    }
  }

  // Start AP mode
  logMessage("INFO", "Starting AP mode...");
  
  // Ensure we are disconnected from any previous STA connection attempt
  WiFi.disconnect(); 
  delay(100);

  WiFi.mode(WIFI_AP);
  delay(100);  // Give WiFi time to switch modes
  yield();
  feedWatchdog();

  // Create dynamic SSID with MAC address suffix
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String dynamicSSID = "HarryGarage-" + mac.substring(mac.length() - 6); // Last 6 chars

  if (WiFi.softAP(dynamicSSID.c_str(), AP_PASSWORD)) {
    logMessage("INFO", "AP Started successfully");
  } else {
    logMessage("ERROR", "AP Start Failed");
  }
  
  delay(500);  // Wait for AP to stabilize
  yield();
  feedWatchdog();

  // Build messages without concatenation
  char apIpMsg[64];
  IPAddress apIp = WiFi.softAPIP();
  snprintf(apIpMsg, sizeof(apIpMsg), "AP IP: %d.%d.%d.%d", apIp[0], apIp[1], apIp[2], apIp[3]);
  logMessage("INFO", apIpMsg);
  
  char ssidMsg[128];
  snprintf(ssidMsg, sizeof(ssidMsg), "AP SSID: %s", dynamicSSID.c_str());
  logMessage("INFO", ssidMsg);

  // Start DNS server for captive portal
  dnsServer.start(53, "*", WiFi.softAPIP());
  delay(100);
  yield();
  feedWatchdog();

  apMode = true;
  digitalWrite(LED_PIN, LOW);  // LED ON (inverted) to indicate AP mode
  logMessage("INFO", "AP mode ready");
}

void setupOTA() {
  if (apMode) {
    logMessage("INFO", "OTA disabled in AP mode");
    return;
  }

  // Use WiFi hostname for OTA
  ArduinoOTA.setHostname(WiFi.getHostname());

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else {
      type = "filesystem";
    }
    logMessage("INFO", "OTA Update Start: " + type);
  });

  ArduinoOTA.onEnd([]() {
    logMessage("INFO", "OTA Update Complete");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static unsigned int lastPercent = 0;
    unsigned int percent = (progress / (total / 100));
    if (percent != lastPercent && percent % 10 == 0) {
      logMessage("INFO", "OTA Progress: " + String(percent) + "%");
      lastPercent = percent;
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    String errorMsg = "OTA Error[" + String(error) + "]: ";
    if (error == OTA_AUTH_ERROR) errorMsg += "Auth Failed";
    else if (error == OTA_BEGIN_ERROR) errorMsg += "Begin Failed";
    else if (error == OTA_CONNECT_ERROR) errorMsg += "Connect Failed";
    else if (error == OTA_RECEIVE_ERROR) errorMsg += "Receive Failed";
    else if (error == OTA_END_ERROR) errorMsg += "End Failed";
    logMessage("ERROR", errorMsg);
  });

  ArduinoOTA.begin();
  logMessage("INFO", "ArduinoOTA ready");
}

void setupWebServer() {
  // WebSocket event handler
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // Serve root page with enhanced UI
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Garage Door Opener</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            padding: 20px;
        }
        .container {
            background: white;
            border-radius: 20px;
            box-shadow: 0 20px 60px rgba(0,0,0,0.3);
            padding: 0;
            max-width: 800px;
            margin: 0 auto;
            overflow: hidden;
        }
        .header {
            padding: 30px 40px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
        }
        h1 {
            font-size: 28px;
            margin-bottom: 5px;
        }
        .subtitle {
            opacity: 0.9;
            font-size: 14px;
        }
        .tabs {
            display: flex;
            background: #f8f9fa;
            border-bottom: 2px solid #dee2e6;
        }
        .tab {
            flex: 1;
            padding: 15px;
            text-align: center;
            cursor: pointer;
            font-weight: 600;
            color: #666;
            transition: all 0.3s;
            border: none;
            background: none;
        }
        .tab:hover {
            background: #e9ecef;
        }
        .tab.active {
            color: #667eea;
            border-bottom: 3px solid #667eea;
            margin-bottom: -2px;
        }
        .tab-content {
            display: none;
            padding: 30px 40px;
        }
        .tab-content.active {
            display: block;
        }
        .status-card {
            background: #f8f9fa;
            border-radius: 15px;
            padding: 25px;
            margin-bottom: 25px;
            text-align: center;
        }
        .door-status {
            font-size: 96px;
            margin-bottom: 10px;
            transition: transform 0.3s ease;
        }
        .status-text {
            font-size: 48px;
            font-weight: 600;
            margin-bottom: 5px;
            transition: all 0.3s ease;
        }
        .status-open { color: #e74c3c; }
        .status-closed { color: #27ae60; }
        .status-transitioning {
            animation: pulse 1.5s ease-in-out infinite;
        }
        .status-text-transitioning {
            animation: colorPulse 1.5s ease-in-out infinite;
        }
        @keyframes pulse {
            0%, 100% {
                transform: scale(1);
                opacity: 1;
            }
            50% {
                transform: scale(1.2);
                opacity: 0.8;
            }
        }
        @keyframes colorPulse {
            0%, 100% {
                opacity: 1;
                transform: scale(1);
            }
            50% {
                opacity: 0.7;
                transform: scale(1.05);
            }
        }
        .info-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 15px;
            margin-bottom: 25px;
        }
        .info-item {
            background: #f8f9fa;
            padding: 15px;
            border-radius: 10px;
        }
        .info-label {
            color: #666;
            font-size: 12px;
            margin-bottom: 5px;
        }
        .info-value {
            color: #333;
            font-weight: 600;
            font-size: 16px;
        }
        .btn {
            width: 100%;
            padding: 15px;
            border: none;
            border-radius: 10px;
            font-size: 16px;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.3s;
            margin-bottom: 10px;
        }
        .btn-primary {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
        }
        .btn-primary:hover:not(:disabled) {
            transform: translateY(-2px);
            box-shadow: 0 5px 20px rgba(102, 126, 234, 0.4);
        }
        .btn-secondary {
            background: #6c757d;
            color: white;
        }
        .btn-secondary:hover {
            background: #5a6268;
        }
        .btn-danger {
            background: #dc3545;
            color: white;
        }
        .btn-danger:hover {
            background: #c82333;
        }
        .btn:disabled {
            opacity: 0.6;
            cursor: not-allowed;
        }
        #triggerBtn {
            font-size: 32px;
            padding: 25px;
        }
        .wifi-config {
            display: none;
            margin-top: 20px;
        }
        .wifi-config.show {
            display: block;
        }
        .form-group {
            margin-bottom: 20px;
        }
        .form-group label {
            display: block;
            margin-bottom: 8px;
            color: #333;
            font-weight: 600;
            font-size: 14px;
        }
        .form-group input,
        .form-group textarea {
            width: 100%;
            padding: 12px;
            border: 2px solid #e0e0e0;
            border-radius: 8px;
            font-size: 14px;
            transition: border-color 0.3s;
            font-family: inherit;
        }
        .form-group input:focus,
        .form-group textarea:focus {
            outline: none;
            border-color: #667eea;
        }
        .form-group textarea {
            resize: vertical;
            min-height: 80px;
        }
        .message {
            padding: 12px;
            border-radius: 8px;
            margin-bottom: 15px;
            display: none;
        }
        .message.show {
            display: block;
        }
        .message.success {
            background: #d4edda;
            color: #155724;
            border: 1px solid #c3e6cb;
        }
        .message.error {
            background: #f8d7da;
            color: #721c24;
            border: 1px solid #f5c6cb;
        }
        .status-box {
            padding: 15px;
            border-radius: 8px;
            margin-bottom: 20px;
            font-size: 14px;
        }
        .status-success {
            background: #d4edda;
            color: #155724;
            border: 1px solid #c3e6cb;
        }
        .status-error {
            background: #f8d7da;
            color: #721c24;
            border: 1px solid #f5c6cb;
        }
        .status-info {
            background: #d1ecf1;
            color: #0c5460;
            border: 1px solid #bee5eb;
        }
        .btn-warning {
            background: #ffc107;
            color: #333;
        }
        .btn-warning:hover {
            background: #e0a800;
        }
        .btn-success {
            background: #28a745;
            color: white;
        }
        .btn-success:hover {
            background: #218838;
        }
        .helper-text {
            font-size: 12px;
            color: #6c757d;
            margin-top: 5px;
        }
        h3 {
            color: #333;
            margin-bottom: 15px;
            font-size: 18px;
        }
        .log-container {
            background: #1e1e1e;
            border-radius: 10px;
            padding: 15px;
            height: 400px;
            overflow-y: auto;
            font-family: 'Courier New', monospace;
            font-size: 12px;
        }
        .log-entry {
            margin-bottom: 5px;
            word-wrap: break-word;
        }
        .log-timestamp {
            color: #888;
            margin-right: 10px;
        }
        .log-level {
            font-weight: bold;
            margin-right: 10px;
        }
        .log-level-INFO { color: #4CAF50; }
        .log-level-WARN { color: #FF9800; }
        .log-level-ERROR { color: #f44336; }
        .log-level-DEBUG { color: #2196F3; }
        .log-message {
            color: #e0e0e0;
        }
        .log-controls {
            margin-bottom: 15px;
            display: flex;
            gap: 10px;
        }
        .log-controls button {
            flex: 1;
        }
        .upload-area {
            border: 2px dashed #667eea;
            border-radius: 10px;
            padding: 40px;
            text-align: center;
            cursor: pointer;
            transition: all 0.3s;
            margin-bottom: 20px;
        }
        .upload-area:hover {
            background: #f8f9fa;
        }
        .upload-area.dragover {
            background: #e7e9fd;
            border-color: #764ba2;
        }
        .file-input {
            display: none;
        }
        .progress-bar {
            width: 100%;
            height: 30px;
            background: #e0e0e0;
            border-radius: 15px;
            overflow: hidden;
            margin-top: 20px;
            display: none;
        }
        .progress-fill {
            height: 100%;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            width: 0%;
            transition: width 0.3s;
            display: flex;
            align-items: center;
            justify-content: center;
            color: white;
            font-weight: bold;
            font-size: 14px;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>🚪 Garage Door Opener</h1>
        </div>

        <div class="tabs">
            <button class="tab active" onclick="switchTab('control')">Control</button>
            <button class="tab" onclick="switchTab('logs')">Logs</button>
            <button class="tab" onclick="switchTab('ota')">OTA Update</button>
            <button class="tab" onclick="switchTab('settings')">Settings</button>
            <button class="tab" onclick="switchTab('registration')">Device Registration</button>
        </div>

        <!-- Control Tab -->
        <div id="control-tab" class="tab-content active">
            <div id="message" class="message"></div>

            <div class="status-card">
                <div class="door-status" id="doorIcon">🚪</div>
                <div class="status-text" id="doorStatus">Loading...</div>
                <div class="info-label" id="lastUpdate">Checking status...</div>
            </div>

            <button id="triggerBtn" class="btn btn-primary" onclick="triggerDoor()">Trigger Door</button>

            <div class="info-grid">
                <div class="info-item">
                    <div class="info-label">WiFi Status</div>
                    <div class="info-value" id="wifiStatus">Loading...</div>
                </div>
                <div class="info-item">
                    <div class="info-label">IP Address</div>
                    <div class="info-value" id="ipAddress">Loading...</div>
                </div>
                <div class="info-item">
                    <div class="info-label">Signal Strength</div>
                    <div class="info-value" id="rssi">Loading...</div>
                </div>
                <div class="info-item">
                    <div class="info-label">Uptime</div>
                    <div class="info-value" id="uptime">Loading...</div>
                </div>
            </div>
        </div>

        <!-- Logs Tab -->
        <div id="logs-tab" class="tab-content">
            <div class="log-controls">
                <button class="btn btn-secondary" onclick="clearLogs()">Clear Display</button>
                <button class="btn btn-secondary" onclick="downloadLogs()">Download Logs</button>
            </div>
            <div class="log-container" id="logContainer">
                <div class="log-entry">
                    <span class="log-timestamp">--:--:--</span>
                    <span class="log-level log-level-INFO">INFO</span>
                    <span class="log-message">Connecting to log stream...</span>
                </div>
            </div>
        </div>

        <!-- OTA Update Tab -->
        <div id="ota-tab" class="tab-content">
            <div id="ota-message" class="message"></div>

            <div class="upload-area" id="uploadArea" onclick="document.getElementById('firmwareFile').click()">
                <div style="font-size: 48px; margin-bottom: 15px;">📦</div>
                <div style="font-size: 18px; font-weight: 600; margin-bottom: 10px;">Upload Firmware</div>
                <div style="color: #666;">Click to select or drag and drop .bin file</div>
                <input type="file" id="firmwareFile" class="file-input" accept=".bin" onchange="uploadFirmware(this.files[0])">
            </div>

            <div class="progress-bar" id="progressBar">
                <div class="progress-fill" id="progressFill">0%</div>
            </div>

            <div style="background: #fff3cd; padding: 15px; border-radius: 10px; border-left: 4px solid #ffc107; margin-top: 20px;">
                <strong>⚠️ Warning:</strong> Device will restart after successful upload. Make sure you have the correct firmware file (.bin).
            </div>
        </div>

        <!-- Settings Tab -->
        <div id="settings-tab" class="tab-content">
            <div id="settings-message" class="message"></div>

            <button class="btn btn-secondary" onclick="toggleWifiConfig()">Configure WiFi</button>

            <div id="wifiConfig" class="wifi-config">
                <div class="form-group">
                    <label>WiFi Network (SSID)</label>
                    <input type="text" id="ssid" placeholder="Enter WiFi network name">
                </div>
                <div class="form-group">
                    <label>WiFi Password</label>
                    <input type="password" id="password" placeholder="Enter WiFi password">
                </div>
                <button class="btn btn-primary" onclick="saveWifi()">Save & Restart</button>
                <button class="btn btn-secondary" onclick="toggleWifiConfig()">Cancel</button>
            </div>

            <button class="btn btn-danger" onclick="restart()" style="margin-top: 20px;">Restart Device</button>
        </div>

        <!-- Device Registration Tab -->
        <div id="registration-tab" class="tab-content">
            <div id="registration-message" class="message"></div>

            <div class="status-card">
                <h3 style="margin-bottom: 15px;">Registration Status</h3>
                <div id="registrationStatusBox" class="status-box status-info">
                    Loading registration status...
                </div>
                <button class="btn btn-warning" onclick="forceRegister()" style="background: #ffc107; color: #333;">Register Now</button>
            </div>

            <div class="status-card">
                <h3 style="margin-bottom: 15px;">Registration Settings</h3>
                <form id="registrationForm">
                    <div class="checkbox-wrapper" style="display: flex; align-items: center; margin-bottom: 20px;">
                        <input type="checkbox" id="regEnabled" checked style="width: 18px; height: 18px; margin-right: 10px;">
                        <label for="regEnabled" style="font-weight: 600; cursor: pointer;">Enable automatic registration</label>
                    </div>

                    <div class="form-group">
                        <label for="regServerUrl">Control Server URL</label>
                        <input type="text" id="regServerUrl" placeholder="http://192.168.1.225:3000" required>
                        <div class="helper-text">URL of your control server (including http:// or https://)</div>
                    </div>

                    <div class="form-group">
                        <label for="regDeviceName">Device Name</label>
                        <input type="text" id="regDeviceName" placeholder="Garage-Door" required>
                        <div class="helper-text">Friendly name for this device</div>
                    </div>

                    <div class="form-group">
                        <label for="regDeviceType">Device Type</label>
                        <input type="text" id="regDeviceType" placeholder="esp32_garage_door" required>
                        <div class="helper-text">Device type identifier</div>
                    </div>

                    <div class="form-group">
                        <label for="regDeviceDescription">Description</label>
                        <textarea id="regDeviceDescription" placeholder="ESP32-C3 Garage Door Opener"></textarea>
                        <div class="helper-text">Optional description of this device</div>
                    </div>

                    <button type="submit" class="btn btn-success" style="background: #28a745; color: white;">Save Settings</button>
                </form>
            </div>

            <div class="status-card">
                <h3 style="margin-bottom: 15px;">Current Device Information</h3>
                <div class="info-grid">
                    <div class="info-label">IP Address:</div>
                    <div class="info-value" id="regDeviceIp">-</div>
                    <div class="info-label">MAC Address:</div>
                    <div class="info-value" id="regDeviceMac">-</div>
                    <div class="info-label">Hostname:</div>
                    <div class="info-value" id="regDeviceHostname">-</div>
                </div>
            </div>
        </div>
    </div>

    <script>
        let statusInterval;
        let ws;
        let logs = [];

        // WebSocket for real-time logs
        function connectWebSocket() {
            const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
            ws = new WebSocket(protocol + '//' + window.location.host + '/ws');

            ws.onopen = function() {
                console.log('WebSocket connected');
                addLogEntry('INFO', 'WebSocket connected', getCurrentTime());
            };

            ws.onmessage = function(event) {
                try {
                    const data = JSON.parse(event.data);
                    if (data.type === 'log') {
                        addLogEntry(data.level, data.message, data.timestamp);
                    } else if (data.type === 'status') {
                        // Update status immediately when received via WebSocket
                        updateStatus();
                    }
                } catch (e) {
                    console.error('Error parsing WebSocket message:', e);
                }
            };

            ws.onclose = function() {
                console.log('WebSocket disconnected');
                addLogEntry('WARN', 'WebSocket disconnected. Reconnecting...', getCurrentTime());
                setTimeout(connectWebSocket, 3000);
            };

            ws.onerror = function(error) {
                console.error('WebSocket error:', error);
            };
        }

        function getCurrentTime() {
            const now = new Date();
            return now.toLocaleTimeString();
        }

        function addLogEntry(level, message, timestamp) {
            logs.push({level, message, timestamp});
            if (logs.length > 200) {
                logs.shift();
            }

            const container = document.getElementById('logContainer');
            const entry = document.createElement('div');
            entry.className = 'log-entry';
            entry.innerHTML = `
                <span class="log-timestamp">${timestamp}</span>
                <span class="log-level log-level-${level}">${level}</span>
                <span class="log-message">${message}</span>
            `;
            container.appendChild(entry);
            container.scrollTop = container.scrollHeight;
        }

        function clearLogs() {
            document.getElementById('logContainer').innerHTML = '';
            logs = [];
        }

        function downloadLogs() {
            const logText = logs.map(log => `[${log.timestamp}] [${log.level}] ${log.message}`).join('\n');
            const blob = new Blob([logText], { type: 'text/plain' });
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = 'garage-door-logs-' + new Date().toISOString() + '.txt';
            a.click();
            URL.revokeObjectURL(url);
        }

        let registrationTabLoaded = false;
        function switchTab(tabName) {
            // Update tab buttons
            document.querySelectorAll('.tab').forEach(tab => tab.classList.remove('active'));
            event.target.classList.add('active');

            // Update tab content
            document.querySelectorAll('.tab-content').forEach(content => content.classList.remove('active'));
            document.getElementById(tabName + '-tab').classList.add('active');
            
            // Load registration data if registration tab is shown
            if (tabName === 'registration' && !registrationTabLoaded) {
                loadRegistrationSettings();
                loadDeviceInfo();
                registrationTabLoaded = true;
                // Refresh status every 30 seconds
                setInterval(() => {
                    if (document.getElementById('registration-tab').classList.contains('active')) {
                        loadRegistrationSettings();
                    }
                }, 30000);
            }
        }

        function showMessage(msg, isError = false, targetId = 'message') {
            const msgEl = document.getElementById(targetId);
            msgEl.textContent = msg;
            msgEl.className = 'message show ' + (isError ? 'error' : 'success');
            setTimeout(() => {
                msgEl.className = 'message';
            }, 5000);
        }

        async function updateStatus() {
            try {
                const response = await fetch('/api/status');
                const data = await response.json();

                const isOpen = data.door_open;
                const statusTransition = data.status_transition || "";
                
                const doorIcon = document.getElementById('doorIcon');
                const doorStatus = document.getElementById('doorStatus');
                
                // Check if we're in transition status mode (from backend)
                if (statusTransition && statusTransition.length > 0) {
                    // Show temporary status with animation
                    if (statusTransition === 'opening') {
                        doorIcon.textContent = '🟡';
                        doorIcon.className = 'door-status status-transitioning';
                        doorStatus.textContent = 'OPENING';
                        doorStatus.className = 'status-text status-open status-text-transitioning';
                    } else if (statusTransition === 'closing') {
                        doorIcon.textContent = '🟡';
                        doorIcon.className = 'door-status status-transitioning';
                        doorStatus.textContent = 'CLOSING';
                        doorStatus.className = 'status-text status-closed status-text-transitioning';
                    }
                } else {
                    // Show actual status (remove animations)
                    doorIcon.textContent = isOpen ? '🟢' : '🔴';
                    doorIcon.className = 'door-status';
                    doorStatus.textContent = isOpen ? 'OPEN' : 'CLOSED';
                    doorStatus.className = 'status-text ' + (isOpen ? 'status-open' : 'status-closed');
                }
                
                document.getElementById('lastUpdate').textContent = 'Last update: ' + new Date().toLocaleTimeString();

                document.getElementById('wifiStatus').textContent = data.wifi_connected ? 'Connected' : 'AP Mode';
                document.getElementById('ipAddress').textContent = data.ip_address;
                document.getElementById('rssi').textContent = data.wifi_connected ? data.rssi + ' dBm' : 'N/A';
                document.getElementById('uptime').textContent = formatUptime(data.uptime);
            } catch (error) {
                console.error('Failed to update status:', error);
            }
        }

        function formatUptime(seconds) {
            const days = Math.floor(seconds / 86400);
            const hours = Math.floor((seconds % 86400) / 3600);
            const minutes = Math.floor((seconds % 3600) / 60);
            const secs = seconds % 60;

            if (days > 0) return `${days}d ${hours}h ${minutes}m`;
            if (hours > 0) return `${hours}h ${minutes}m ${secs}s`;
            if (minutes > 0) return `${minutes}m ${secs}s`;
            return `${secs}s`;
        }

        async function triggerDoor() {
            try {
                const response = await fetch('/api/trigger', { method: 'POST' });
                const data = await response.json();
                
                // Update display immediately to show transition status
                updateStatus();
            } catch (error) {
                showMessage('Failed to trigger door', true);
            }
        }

        async function toggleWifiConfig() {
            const configDiv = document.getElementById('wifiConfig');
            const isShowing = configDiv.classList.toggle('show');
            
            // If showing the config form, populate it with saved values
            if (isShowing) {
                try {
                    const response = await fetch('/api/status');
                    const data = await response.json();
                    
                    // Populate SSID field if saved SSID exists
                    if (data.saved_ssid && data.saved_ssid.length > 0) {
                        document.getElementById('ssid').value = data.saved_ssid;
                    } else {
                        document.getElementById('ssid').value = '';
                    }
                    
                    // Populate password field if saved password exists
                    if (data.saved_password && data.saved_password.length > 0) {
                        document.getElementById('password').value = data.saved_password;
                    } else {
                        document.getElementById('password').value = '';
                    }
                } catch (error) {
                    console.error('Failed to load WiFi config:', error);
                }
            }
        }

        async function saveWifi() {
            const ssid = document.getElementById('ssid').value;
            const password = document.getElementById('password').value;

            if (!ssid) {
                showMessage('Please enter WiFi SSID', true, 'settings-message');
                return;
            }

            try {
                const response = await fetch('/api/config', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ ssid, password })
                });

                const data = await response.json();
                showMessage('WiFi configured! Restarting...', false, 'settings-message');
                setTimeout(() => {
                    window.location.href = '/';
                }, 3000);
            } catch (error) {
                showMessage('Failed to save configuration', true, 'settings-message');
            }
        }

        async function restart() {
            if (confirm('Are you sure you want to restart the device?')) {
                try {
                    await fetch('/api/restart', { method: 'POST' });
                    showMessage('Device restarting...', false, 'settings-message');
                } catch (error) {
                    showMessage('Restart initiated', false, 'settings-message');
                }
            }
        }

        // OTA Upload functionality
        const uploadArea = document.getElementById('uploadArea');

        ['dragenter', 'dragover', 'dragleave', 'drop'].forEach(eventName => {
            uploadArea.addEventListener(eventName, preventDefaults, false);
        });

        function preventDefaults(e) {
            e.preventDefault();
            e.stopPropagation();
        }

        ['dragenter', 'dragover'].forEach(eventName => {
            uploadArea.addEventListener(eventName, () => {
                uploadArea.classList.add('dragover');
            });
        });

        ['dragleave', 'drop'].forEach(eventName => {
            uploadArea.addEventListener(eventName, () => {
                uploadArea.classList.remove('dragover');
            });
        });

        uploadArea.addEventListener('drop', (e) => {
            const files = e.dataTransfer.files;
            if (files.length > 0) {
                uploadFirmware(files[0]);
            }
        });

        async function uploadFirmware(file) {
            if (!file) return;

            if (!file.name.endsWith('.bin')) {
                showMessage('Please select a .bin file', true, 'ota-message');
                return;
            }

            const progressBar = document.getElementById('progressBar');
            const progressFill = document.getElementById('progressFill');

            progressBar.style.display = 'block';
            progressFill.style.width = '0%';
            progressFill.textContent = '0%';

            const formData = new FormData();
            formData.append('firmware', file);

            try {
                const xhr = new XMLHttpRequest();

                xhr.upload.addEventListener('progress', (e) => {
                    if (e.lengthComputable) {
                        const percent = Math.round((e.loaded / e.total) * 100);
                        progressFill.style.width = percent + '%';
                        progressFill.textContent = percent + '%';
                    }
                });

                xhr.addEventListener('load', () => {
                    if (xhr.status === 200) {
                        showMessage('Firmware uploaded successfully! Device is restarting...', false, 'ota-message');
                        progressFill.style.width = '100%';
                        progressFill.textContent = 'Complete!';
                        setTimeout(() => {
                            window.location.reload();
                        }, 5000);
                    } else {
                        showMessage('Upload failed: ' + xhr.responseText, true, 'ota-message');
                        progressBar.style.display = 'none';
                    }
                });

                xhr.addEventListener('error', () => {
                    showMessage('Upload error occurred', true, 'ota-message');
                    progressBar.style.display = 'none';
                });

                xhr.open('POST', '/update');
                xhr.send(formData);

            } catch (error) {
                showMessage('Upload failed: ' + error.message, true, 'ota-message');
                progressBar.style.display = 'none';
            }
        }

        // Device Registration functions
        async function loadRegistrationSettings() {
            try {
                const response = await fetch('/api/registration');
                const data = await response.json();

                document.getElementById('regEnabled').checked = data.enabled;
                document.getElementById('regServerUrl').value = data.server_url || '';
                document.getElementById('regDeviceName').value = data.device_name || '';
                document.getElementById('regDeviceType').value = data.device_type || '';
                document.getElementById('regDeviceDescription').value = data.device_description || '';

                updateRegistrationStatus(data);
            } catch (error) {
                console.error('Error loading registration settings:', error);
                showMessage('Error loading registration settings', true, 'registration-message');
            }
        }

        async function loadDeviceInfo() {
            try {
                const response = await fetch('/api/status');
                const data = await response.json();

                // Debug: log the data to see what we're getting
                console.log('Device info data:', data);
                console.log('IP:', data.ip_address, 'MAC:', data.mac_address, 'Hostname:', data.hostname);

                // Ensure we're using the correct field names
                const ipAddr = data.ip_address || '-';
                const macAddr = data.mac_address || '-';
                const hostname = data.hostname || '-';

                document.getElementById('regDeviceIp').textContent = ipAddr;
                document.getElementById('regDeviceMac').textContent = macAddr;
                document.getElementById('regDeviceHostname').textContent = hostname;
            } catch (error) {
                console.error('Error loading device info:', error);
            }
        }

        function updateRegistrationStatus(data) {
            const statusBox = document.getElementById('registrationStatusBox');

            if (!data.enabled) {
                statusBox.className = 'status-box status-info';
                statusBox.innerHTML = '<strong>Registration Disabled</strong><br>Automatic registration is turned off.';
                return;
            }

            if (data.last_success) {
                const secondsAgo = data.last_registration_seconds_ago || 0;
                const minutesAgo = Math.floor(secondsAgo / 60);
                const timeStr = minutesAgo > 0 ? `${minutesAgo} minute(s) ago` : `${secondsAgo} second(s) ago`;

                statusBox.className = 'status-box status-success';
                statusBox.style.background = '#d4edda';
                statusBox.style.color = '#155724';
                statusBox.style.border = '1px solid #c3e6cb';
                statusBox.innerHTML = `
                    <strong>Last Registration: Successful</strong><br>
                    Registered ${timeStr}<br>
                    Next registration in ${Math.max(0, 5 - minutesAgo)} minute(s)
                `;
            } else {
                statusBox.className = 'status-box status-error';
                statusBox.style.background = '#f8d7da';
                statusBox.style.color = '#721c24';
                statusBox.style.border = '1px solid #f5c6cb';
                const errorMsg = data.last_error || 'Unknown error';
                statusBox.innerHTML = `
                    <strong>Last Registration: Failed</strong><br>
                    Error: ${errorMsg}<br>
                    Will retry automatically
                `;
            }
        }

        document.getElementById('registrationForm').addEventListener('submit', async (e) => {
            e.preventDefault();

            const settings = {
                enabled: document.getElementById('regEnabled').checked,
                server_url: document.getElementById('regServerUrl').value,
                device_name: document.getElementById('regDeviceName').value,
                device_type: document.getElementById('regDeviceType').value,
                device_description: document.getElementById('regDeviceDescription').value
            };

            try {
                const response = await fetch('/api/registration', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(settings)
                });

                const data = await response.json();

                if (data.success) {
                    showMessage('Settings saved successfully!', false, 'registration-message');
                    setTimeout(() => loadRegistrationSettings(), 1000);
                } else {
                    showMessage('Error saving settings', true, 'registration-message');
                }
            } catch (error) {
                showMessage('Error: ' + error.message, true, 'registration-message');
            }
        });

        async function forceRegister() {
            try {
                const response = await fetch('/api/registration/register', { method: 'POST' });
                const data = await response.json();

                if (data.success) {
                    showMessage('Device registered successfully!', false, 'registration-message');
                    setTimeout(() => loadRegistrationSettings(), 1000);
                } else {
                    showMessage('Registration failed: ' + (data.error || 'Unknown error'), true, 'registration-message');
                }
            } catch (error) {
                showMessage('Error: ' + error.message, true, 'registration-message');
            }
        }


        // Initialize
        connectWebSocket();
        updateStatus();
        statusInterval = setInterval(updateStatus, 2000);
    </script>
</body>
</html>
)rawliteral";
    request->send(200, "text/html", html);
  });

  // API: Get status
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<1024> doc;

    doc["door_open"] = doorOpen;
    doc["wifi_connected"] = !apMode;
    
    // Get IP address
    IPAddress ip = apMode ? WiFi.softAPIP() : WiFi.localIP();
    doc["ip_address"] = ip.toString();
    
    doc["rssi"] = WiFi.RSSI();
    doc["uptime"] = millis() / 1000;
    doc["ssid"] = apMode ? AP_SSID : WiFi.SSID();
    doc["saved_ssid"] = wifiSSID;  // Saved WiFi SSID for configuration form
    doc["saved_password"] = wifiPassword;  // Saved WiFi password for configuration form
    
    // Get MAC address
    doc["mac_address"] = WiFi.macAddress();
    
    // Get hostname
    doc["hostname"] = WiFi.getHostname();
    
    // Include transition status if active
    if (doorStatusTransition.length() > 0) {
      doc["status_transition"] = doorStatusTransition;
    } else {
      doc["status_transition"] = "";
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // API: Get all logs
  server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(8192);
    JsonArray logsArray = doc.createNestedArray("logs");

    int count = min(logCount, LOG_BUFFER_SIZE);
    int startIdx = (logIndex - count + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;

    for (int i = 0; i < count; i++) {
      int idx = (startIdx + i) % LOG_BUFFER_SIZE;
      JsonObject logObj = logsArray.createNestedObject();
      logObj["timestamp"] = logBuffer[idx].timestamp;
      logObj["level"] = logBuffer[idx].level;
      logObj["message"] = logBuffer[idx].message;
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // API: Trigger door
  server.on("/api/trigger", HTTP_POST, [](AsyncWebServerRequest *request) {
    triggerRelay();
    
    // Set transition status based on current door state
    doorStatusTransition = doorOpen ? "closing" : "opening";
    statusTransitionStartTime = millis();
    
    // Broadcast status update to all WebSocket clients
    broadcastStatusUpdate();

    StaticJsonDocument<128> doc;
    doc["success"] = true;
    doc["message"] = "Door triggered";

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // API: Save WiFi config
  server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<256> doc;
      DeserializationError error = deserializeJson(doc, data);

      if (error) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      wifiSSID = doc["ssid"].as<String>();
      wifiPassword = doc["password"].as<String>();
      saveConfiguration();

      request->send(200, "application/json", "{\"success\":true}");

      logMessage("INFO", "WiFi config updated, restarting...");
      delay(1000);
      ESP.restart();
    });

  // API: Restart
  server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"success\":true}");
    logMessage("INFO", "Restart requested");
    delay(1000);
    ESP.restart();
  });

  // API: Get registration settings
  server.on("/api/registration", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (deviceRegistration == nullptr) {
      request->send(503, "application/json", "{\"error\":\"Registration manager not available\"}");
      return;
    }

    String json = deviceRegistration->getSettingsJson();
    request->send(200, "application/json", json);
  });

  // API: Set registration settings
  server.on("/api/registration", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (deviceRegistration == nullptr) {
        request->send(503, "application/json", "{\"error\":\"Registration manager not available\"}");
        return;
      }

      StaticJsonDocument<512> doc;
      DeserializationError error = deserializeJson(doc, data);

      if (error) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      String serverUrl = doc["server_url"] | deviceRegistration->getServerUrl();
      String deviceName = doc["device_name"] | deviceRegistration->getDeviceName();
      String deviceType = doc["device_type"] | deviceRegistration->getDeviceType();
      String deviceDescription = doc["device_description"] | deviceRegistration->getDeviceDescription();
      bool enabled = doc["enabled"] | deviceRegistration->isEnabled();

      deviceRegistration->updateSettings(serverUrl, deviceName, deviceType, deviceDescription, enabled);

      request->send(200, "application/json", "{\"success\":true}");
    });

  // API: Force registration
  server.on("/api/registration/register", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (deviceRegistration == nullptr) {
      request->send(503, "application/json", "{\"error\":\"Registration manager not available\"}");
      return;
    }

    bool success = deviceRegistration->registerDevice();

    StaticJsonDocument<256> doc;
    doc["success"] = success;
    if (!success) {
      doc["error"] = deviceRegistration->getLastError();
    }

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // OTA Update handler
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request) {
    bool shouldReboot = !Update.hasError();
    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain",
      shouldReboot ? "OK" : "FAIL");
    response->addHeader("Connection", "close");
    request->send(response);

    if (shouldReboot) {
      logMessage("INFO", "OTA update successful, restarting...");
      delay(1000);
      ESP.restart();
    } else {
      logMessage("ERROR", "OTA update failed");
    }
  }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!index) {
      logMessage("INFO", "OTA Update Start: " + filename);
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
        logMessage("ERROR", "OTA begin failed");
      }
    }

    if (Update.write(data, len) != len) {
      Update.printError(Serial);
      logMessage("ERROR", "OTA write failed");
    }

    if (final) {
      if (Update.end(true)) {
        logMessage("INFO", "OTA Update Success: " + String(index + len) + " bytes");
      } else {
        Update.printError(Serial);
        logMessage("ERROR", "OTA end failed");
      }
    }
  });

  // Captive portal redirect
  server.onNotFound([](AsyncWebServerRequest *request) {
    if (apMode) {
      request->redirect("/");
    } else {
      request->send(404, "text/plain", "Not found");
    }
  });

  server.begin();
  delay(200);  // Allow server to initialize
  yield();
  logMessage("INFO", "Web server started on port 80");
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    logMessage("INFO", "WebSocket client connected: " + client->remoteIP().toString());

    // Send existing logs to newly connected client
    int count = min(logCount, LOG_BUFFER_SIZE);
    int startIdx = (logIndex - count + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;

    for (int i = 0; i < count; i++) {
      int idx = (startIdx + i) % LOG_BUFFER_SIZE;
      StaticJsonDocument<512> doc;
      doc["type"] = "log";
      doc["timestamp"] = logBuffer[idx].timestamp;
      doc["level"] = logBuffer[idx].level;
      doc["message"] = logBuffer[idx].message;

      String msg;
      serializeJson(doc, msg);
      client->text(msg);
    }
  } else if (type == WS_EVT_DISCONNECT) {
    logMessage("INFO", "WebSocket client disconnected");
  }
}

void triggerRelay() {
  logMessage("INFO", "Triggering relay");
  digitalWrite(RELAY_PIN, HIGH);
  digitalWrite(LED_PIN, LOW);  // LED ON (inverted)
  relayActive = true;
  relayStartTime = millis();
}

void handleRelay() {
  if (relayActive && (millis() - relayStartTime >= RELAY_PULSE_TIME)) {
    digitalWrite(RELAY_PIN, LOW);
    digitalWrite(LED_PIN, HIGH);  // LED OFF (inverted) in normal mode
    if (apMode) {
      digitalWrite(LED_PIN, LOW);  // Keep LED ON in AP mode
    }
    relayActive = false;
    logMessage("DEBUG", "Relay pulse complete");
  }
}

void updateDoorStatus() {
  static bool lastDoorState = false;
  bool currentState = digitalRead(CONTACT_PIN);

  // Apply inversion if configured
  if (statusInverted) {
    currentState = !currentState;
  }

  if (currentState != lastDoorState) {
    doorOpen = currentState;
    lastDoorState = currentState;
    // Build message without concatenation
    logMessage("INFO", doorOpen ? "Door status: OPEN" : "Door status: CLOSED");
  }
}

void handleStatusTransition() {
  // Clear transition status after duration expires
  if (doorStatusTransition.length() > 0) {
    unsigned long elapsed = millis() - statusTransitionStartTime;
    if (elapsed >= STATUS_TRANSITION_DURATION) {
      doorStatusTransition = "";
      logMessage("DEBUG", "Status transition cleared");
      // Broadcast status update to all WebSocket clients
      broadcastStatusUpdate();
    }
  }
}

void broadcastStatusUpdate() {
  if (ws.count() > 0) {
    StaticJsonDocument<512> doc;
    doc["type"] = "status";
    doc["door_open"] = doorOpen;
    doc["status_transition"] = doorStatusTransition;

    String msg;
    serializeJson(doc, msg);
    ws.textAll(msg);
  }
}

void handleButton() {
  bool buttonState = digitalRead(BUTTON_PIN);
  unsigned long currentTime = millis();

  // Button pressed (LOW due to INPUT_PULLUP)
  if (buttonState == LOW && !buttonPressed) {
    if (currentTime - lastButtonPress > DEBOUNCE_TIME) {
      buttonPressed = true;
      buttonPressStart = currentTime;
      logMessage("DEBUG", "Button pressed");
    }
  }

  // Button released
  if (buttonState == HIGH && buttonPressed) {
    unsigned long pressDuration = currentTime - buttonPressStart;
    buttonPressed = false;
    lastButtonPress = currentTime;

    // Long press (4+ seconds) = Factory reset
    if (pressDuration >= 4000) {
      logMessage("WARN", "Factory reset triggered!");
      preferences.clear();
      delay(500);
      ESP.restart();
    }
    // Short press = Trigger relay
    else if (pressDuration < 1000) {
      logMessage("INFO", "Button short press - triggering relay");
      triggerRelay();
    }
  }
}

void logMessage(String level, String message) {
  String timestamp = getFormattedTime();

  // Print to serial
  Serial.print("[");
  Serial.print(timestamp);
  Serial.print("] [");
  Serial.print(level);
  Serial.print("] ");
  Serial.println(message);

  // Store in circular buffer
  logBuffer[logIndex].timestamp = timestamp;
  logBuffer[logIndex].level = level;
  logBuffer[logIndex].message = message;

  logIndex = (logIndex + 1) % LOG_BUFFER_SIZE;
  if (logCount < LOG_BUFFER_SIZE) {
    logCount++;
  }

  // Send to WebSocket clients
  sendLogToWebSocket(level, message);
}

void sendLogToWebSocket(String level, String message) {
  if (ws.count() > 0) {
    StaticJsonDocument<512> doc;
    doc["type"] = "log";
    doc["timestamp"] = getFormattedTime();
    doc["level"] = level;
    doc["message"] = message;

    String msg;
    serializeJson(doc, msg);
    ws.textAll(msg);
  }
}

String getFormattedTime() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    // Fallback to uptime if time not set
    unsigned long ms = millis();
    unsigned long seconds = ms / 1000;
    unsigned long minutes = seconds / 60;
    unsigned long hours = minutes / 60;

    seconds %= 60;
    minutes %= 60;
    hours %= 24;

    char timeStr[20];
    sprintf(timeStr, "[UP] %02lu:%02lu:%02lu", hours, minutes, seconds);
    return String(timeStr);
  }

  char timeStr[20];
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  return String(timeStr);
}

void configureWatchdog(uint32_t timeoutSeconds) {
  esp_err_t initResult = esp_task_wdt_init(timeoutSeconds, true);
  if (initResult != ESP_OK && initResult != ESP_ERR_INVALID_STATE) {
    Serial.printf("[WARN] Failed to initialize watchdog (%d)\n", initResult);
    watchdogEnabled = false;
    return;
  }

  esp_err_t addResult = esp_task_wdt_add(NULL);
  if (addResult != ESP_OK && addResult != ESP_ERR_INVALID_STATE) {
    Serial.printf("[WARN] Failed to subscribe loop task to watchdog (%d)\n", addResult);
    watchdogEnabled = false;
    return;
  }

  TaskHandle_t idleTask = xTaskGetIdleTaskHandleForCPU(0);
  if (idleTask != NULL) {
    esp_err_t idleResult = esp_task_wdt_add(idleTask);
    if (idleResult != ESP_OK && idleResult != ESP_ERR_INVALID_STATE) {
      Serial.printf("[WARN] Failed to subscribe idle task (CPU0) to watchdog (%d)\n", idleResult);
    }
  }
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
  TaskHandle_t idleTask1 = xTaskGetIdleTaskHandleForCPU(1);
  if (idleTask1 != NULL) {
    esp_err_t idleResult1 = esp_task_wdt_add(idleTask1);
    if (idleResult1 != ESP_OK && idleResult1 != ESP_ERR_INVALID_STATE) {
      Serial.printf("[WARN] Failed to subscribe idle task (CPU1) to watchdog (%d)\n", idleResult1);
    }
  }
#endif

  watchdogEnabled = true;
  feedWatchdog();
}

inline void feedWatchdog() {
  if (!watchdogEnabled) {
    return;
  }
  esp_task_wdt_reset();
}

void disableWatchdog() {
  logMessage("DEBUG", "Disabling task watchdog");

  esp_err_t deleteResult = esp_task_wdt_delete(NULL);
  if (deleteResult != ESP_OK && deleteResult != ESP_ERR_INVALID_STATE) {
    Serial.printf("[WARN] Failed to remove loop task from watchdog (%d)\n", deleteResult);
  }

  TaskHandle_t idleTask = xTaskGetIdleTaskHandleForCPU(0);
  if (idleTask != NULL) {
    esp_err_t idleDelete = esp_task_wdt_delete(idleTask);
    if (idleDelete != ESP_OK && idleDelete != ESP_ERR_INVALID_STATE) {
      Serial.printf("[WARN] Failed to remove idle task (CPU0) from watchdog (%d)\n", idleDelete);
    }
  }
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
  TaskHandle_t idleTask1 = xTaskGetIdleTaskHandleForCPU(1);
  if (idleTask1 != NULL) {
    esp_err_t idleDelete1 = esp_task_wdt_delete(idleTask1);
    if (idleDelete1 != ESP_OK && idleDelete1 != ESP_ERR_INVALID_STATE) {
      Serial.printf("[WARN] Failed to remove idle task (CPU1) from watchdog (%d)\n", idleDelete1);
    }
  }
#endif

  esp_err_t deinitResult = esp_task_wdt_deinit();
  if (deinitResult != ESP_OK && deinitResult != ESP_ERR_INVALID_STATE) {
    Serial.printf("[WARN] Failed to deinitialize watchdog (%d)\n", deinitResult);
  }

  watchdogEnabled = false;
}

void checkWiFiConnection() {
  unsigned long currentMillis = millis();
  
  // Check if it's time to retry WiFi connection
  if (currentMillis - lastWiFiRetryTime >= WIFI_RETRY_INTERVAL_MS) {
    lastWiFiRetryTime = currentMillis;
    
    if (wifiSSID.length() > 0) {
      char retryMsg[128];
      snprintf(retryMsg, sizeof(retryMsg), "Periodic WiFi retry: Attempting to connect to %s", wifiSSID.c_str());
      logMessage("INFO", retryMsg);
      
      // Ensure we are in AP+STA mode to allow connection attempt while keeping AP alive
      if (WiFi.getMode() != WIFI_AP_STA) {
        WiFi.mode(WIFI_AP_STA);
      }
      
      WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
    }
  }

  // Check if we managed to connect
  if (WiFi.status() == WL_CONNECTED) {
    logMessage("INFO", "WiFi reconnected successfully!");
    // Build messages without concatenation
    char ipMsg[64];
    IPAddress ip = WiFi.localIP();
    snprintf(ipMsg, sizeof(ipMsg), "IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    logMessage("INFO", ipMsg);
    
    char rssiMsg[64];
    snprintf(rssiMsg, sizeof(rssiMsg), "Signal: %d dBm", WiFi.RSSI());
    logMessage("INFO", rssiMsg);
    
    // Switch to STA mode (disable AP)
    WiFi.mode(WIFI_STA);
    apMode = false;
    digitalWrite(LED_PIN, HIGH);  // LED OFF (inverted)
    
    // Re-init time
    configTzTime(TIMEZONE, NTP_SERVER);
    
    // Initialize registration if needed
    if (deviceRegistration == nullptr) {
        logMessage("INFO", "Initializing device registration...");
        deviceRegistration = new DeviceRegistration(&preferences);
        deviceRegistration->loadSettings();
    }
    
    // Register device
    if (deviceRegistration->isEnabled()) {
      logMessage("INFO", "Registering device with control server...");
      deviceRegistration->registerDevice();
    }
  }
}
