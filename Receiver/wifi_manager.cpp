/**
 * ============================================================================
 * WiFi and Web Server Implementation
 * ============================================================================
 */

#include "wifi_manager.h"
#include "tank_data.h"
#include "lora_comm.h"
#include <Preferences.h>

extern Preferences preferences;
extern bool loraProcessing;  // From lora_comm.cpp

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
WiFiState wifiState = WIFI_STATE_DISCONNECTED;
String savedSSID = "";
String savedPassword = "";
unsigned long lastWiFiRetry = 0;
int wifiRetryCount = 0;
unsigned long wifiConnectStartTime = 0;

// Alert settings
bool alerts_enabled = DEFAULT_ALERTS_ENABLED;
int alert_low_water = DEFAULT_ALERT_LOW_WATER;
int alert_low_battery = DEFAULT_ALERT_LOW_BATTERY;
String alert_email = DEFAULT_ALERT_EMAIL;

// ============================================================================
// LOAD WIFI CREDENTIALS
// ============================================================================
void loadWiFiCredentials() {
  savedSSID = preferences.getString("wifi_ssid", "");
  savedPassword = preferences.getString("wifi_pass", "");
}

// ============================================================================
// SAVE WIFI CREDENTIALS
// ============================================================================
void saveWiFiCredentials() {
  preferences.putString("wifi_ssid", savedSSID);
  preferences.putString("wifi_pass", savedPassword);
}

// ============================================================================
// CLEAR WIFI CREDENTIALS
// ============================================================================
void clearWiFiCredentials() {
  preferences.remove("wifi_ssid");
  preferences.remove("wifi_pass");
  savedSSID = "";
  savedPassword = "";
}

// ============================================================================
// LOAD ALERT SETTINGS
// ============================================================================
void loadAlertSettings() {
  alerts_enabled = preferences.getBool("alert_enabled", DEFAULT_ALERTS_ENABLED);
  alert_low_water = preferences.getInt("alert_water", DEFAULT_ALERT_LOW_WATER);
  alert_low_battery = preferences.getInt("alert_battery", DEFAULT_ALERT_LOW_BATTERY);
  alert_email = preferences.getString("alert_email", DEFAULT_ALERT_EMAIL);
}

// ============================================================================
// SAVE ALERT SETTINGS
// ============================================================================
void saveAlertSettings() {
  preferences.putBool("alert_enabled", alerts_enabled);
  preferences.putInt("alert_water", alert_low_water);
  preferences.putInt("alert_battery", alert_low_battery);
  preferences.putString("alert_email", alert_email);
}

// ============================================================================
// START WIFI CONNECTION (BLOCKING - for setup)
// ============================================================================
void startWiFiConnectionBlocking(Adafruit_SSD1306& display, Adafruit_NeoPixel& leds) {
  wifiState = WIFI_STATE_CONNECTING;
  wifiRetryCount++;

#if DEBUG_VERBOSE
  Serial.printf("WiFi connecting to %s (attempt #%d)...\n", savedSSID.c_str(), wifiRetryCount);
#else
  Serial.printf("WiFi: %s...\n", savedSSID.c_str());
#endif

  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSSID.c_str(), savedPassword.c_str());

  unsigned long start = millis();
  int dotCount = 0;
  bool ledState = false;

  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT) {
    delay(500);
    yield();  // CRITICAL: Feed watchdog

    // Blink LED 1 (Status) BLUE during connection
    ledState = !ledState;
    if (ledState) {
      leds.setPixelColor(LED_STATUS, 0, 0, 255);  // Blue = WiFi connecting
    } else {
      leds.setPixelColor(LED_STATUS, 0, 0, 0);    // Off
    }
    leds.show();

    // Update display with dots to show progress
    dotCount = (dotCount + 1) % 4;
    display.fillRect(0, 50, 128, 14, SSD1306_BLACK);  // Clear bottom line
    display.setCursor(0, 50);
    for (int i = 0; i < dotCount; i++) {
      display.print(".");
    }
    display.display();
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiState = WIFI_STATE_CONNECTED;
    wifiRetryCount = 0;
    WiFi.setSleep(false);  // CRITICAL: Disable WiFi sleep to prevent WDT crashes
    Serial.printf("WiFi OK: %s\n", WiFi.localIP().toString().c_str());

    // Show success on display
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("WiFi Connected!");
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.print(WiFi.localIP().toString());
    display.display();

    // LED 1 solid GREEN for success
    leds.setPixelColor(LED_STATUS, 0, 255, 0);  // Green = connected
    leds.show();
    delay(1500);  // Show success for 1.5 seconds

  } else {
    Serial.println("WiFi failed, starting AP mode");

    // Show failure on display
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("WiFi Failed");
    display.setTextSize(2);
    display.setCursor(0, 20);
    display.print("AP Mode");
    display.setTextSize(1);
    display.setCursor(0, 45);
    display.print("192.168.4.1");
    display.display();

    // LED 1 BLUE (blink) for AP mode
    for (int i = 0; i < 3; i++) {
      leds.setPixelColor(LED_STATUS, 0, 0, 255);  // Blue
      leds.show();
      delay(200);
      leds.setPixelColor(LED_STATUS, 0, 0, 0);
      leds.show();
      delay(200);
    }

    startAPMode();
  }
}

// ============================================================================
// START WIFI CONNECTION (NON-BLOCKING - for runtime)
// ============================================================================
void startWiFiConnectionNonBlocking() {
  wifiState = WIFI_STATE_CONNECTING;
  wifiRetryCount++;
  wifiConnectStartTime = millis();

#if DEBUG_VERBOSE
  Serial.printf("WiFi reconnecting to %s...\n", savedSSID.c_str());
#endif

  // CRITICAL: Disconnect and wait before mode switch
  WiFi.disconnect(true);
  delay(100);
  yield();
  delay(100);
  yield();

  // Switch to STA mode with delay (split into smaller chunks for better WDT feeding)
  WiFi.mode(WIFI_STA);
  delay(100);
  yield();
  delay(100);
  yield();

  // Begin connection
  WiFi.begin(savedSSID.c_str(), savedPassword.c_str());
  delay(50);
  yield();
  delay(50);
  yield();
}

// ============================================================================
// UPDATE WIFI CONNECTION (Check progress of non-blocking connection)
// ============================================================================
void updateWiFiConnection() {
  if (wifiState != WIFI_STATE_CONNECTING) return;

  unsigned long elapsed = millis() - wifiConnectStartTime;

  if (WiFi.status() == WL_CONNECTED) {
    wifiState = WIFI_STATE_CONNECTED;
    wifiRetryCount = 0;
    WiFi.setSleep(false);  // CRITICAL: Disable WiFi sleep to prevent WDT crashes
    Serial.printf("WiFi OK: %s\n", WiFi.localIP().toString().c_str());
  }
  else if (elapsed > WIFI_CONNECT_TIMEOUT) {
#if DEBUG_VERBOSE
    Serial.printf("WiFi failed (%lums), switching to AP\n", elapsed);
#else
    Serial.println("WiFi failed, starting AP");
#endif
    startAPMode();
  }
}

// ============================================================================
// START AP MODE
// ============================================================================
void startAPMode() {
  WiFi.disconnect(true);
  delay(100);
  yield();

  WiFi.mode(WIFI_AP);
  delay(100);
  yield();

  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  delay(100);
  yield();

  if (WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS)) {
    wifiState = WIFI_STATE_AP_MODE;
    Serial.printf("AP: %s @ 192.168.4.1\n", AP_SSID);
#if DEBUG_VERBOSE
    if (savedSSID.length() > 0) {
      Serial.println("Will retry WiFi every 5 min");
    }
#endif
  } else {
    Serial.println("AP mode failed!");
    wifiState = WIFI_STATE_DISCONNECTED;
  }

  lastWiFiRetry = millis();
}

// ============================================================================
// CHECK WIFI STATUS (Reconnect if dropped, retry from AP mode)
// ============================================================================
void checkWiFiStatus() {
  // Check if connected WiFi dropped
  if (wifiState == WIFI_STATE_CONNECTED && WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    if (savedSSID.length() > 0) {
      startWiFiConnectionNonBlocking();
    } else {
      startAPMode();
    }
  }

  // Retry WiFi from AP mode if credentials are saved
  if (wifiState == WIFI_STATE_AP_MODE && savedSSID.length() > 0) {
    unsigned long elapsed = millis() - lastWiFiRetry;

    // Time to retry (every 5 minutes)
    if (elapsed > WIFI_RETRY_INTERVAL) {
      // CRITICAL: Defer WiFi mode switch if LoRa is processing to prevent WDT crash
      if (loraProcessing || Serial1.available()) {
#if DEBUG_VERBOSE
        Serial.println("WiFi retry deferred (LoRa active)");
#endif
        lastWiFiRetry = millis() - WIFI_RETRY_INTERVAL + 30000;  // Retry in 30 seconds
        return;
      }

      Serial.println("Retrying WiFi...");
      WiFi.softAPdisconnect(true);
      delay(100);  // Reduced delay
      yield();
      delay(100);
      yield();
      startWiFiConnectionNonBlocking();
    }
  }
}

// ============================================================================
// GET WIFI STATE
// ============================================================================
WiFiState getWiFiState() {
  return wifiState;
}

// ============================================================================
// SETUP WEB SERVER
// ============================================================================
void setupWebServer(WebServer& server) {
  server.on("/", [&server]() { handleRoot(server); });
  server.on("/setup", [&server]() { handleSetup(server); });
  server.on("/wifi", [&server]() { handleWifi(server); });
  server.on("/mqtt", [&server]() { handleMqtt(server); });
  server.on("/mqtt_save", HTTP_GET, [&server]() { handleMqttSave(server); });
  server.on("/lora", [&server]() { handleLora(server); });
  server.on("/lora_save", HTTP_GET, [&server]() { handleLoraSave(server); });
  server.on("/alerts", [&server]() { handleAlerts(server); });
  server.on("/alerts_save", HTTP_GET, [&server]() { handleAlertsSave(server); });
  server.on("/s", [&server]() { handleScan(server); });
  server.on("/c", HTTP_GET, [&server]() { handleConnect(server); });
  server.on("/d", [&server]() { handleData(server); });
  server.on("/save", HTTP_GET, [&server]() { handleSaveSettings(server); });
  server.on("/r", [&server]() { handleReset(server); });
  server.begin();
#if DEBUG_VERBOSE
  Serial.println("Web server started");
#endif
}

// ============================================================================
// WEB SERVER HANDLERS
// ============================================================================

void handleRoot(WebServer& server) {
  // Use chunked response to prevent memory fragmentation
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  yield();

  server.sendContent_P(PSTR("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>TankSync</title>"));
  yield();

  server.sendContent_P(PSTR("<style>body{font-family:monospace;background:#000;color:#0f0;padding:15px;font-size:14px;max-width:500px;margin:auto}"));
  server.sendContent_P(PSTR("table{width:100%;border-collapse:collapse}td{padding:8px 0;border-bottom:1px solid #333}"));
  server.sendContent_P(PSTR(".v{color:#0ff;font-size:22px;font-weight:bold;text-align:right}.big{color:#0ff;font-size:48px;text-align:center;margin:15px 0;font-weight:bold}"));
  yield();

  server.sendContent_P(PSTR("input{width:100%;padding:8px;background:#111;color:#0f0;border:1px solid#333}"));
  server.sendContent_P(PSTR("button{width:100%;padding:12px;background:#0f0;color:#000;border:none;margin:8px 0;cursor:pointer;font-weight:bold}"));
  server.sendContent_P(PSTR("</style></head><body>"));
  yield();

  server.sendContent_P(PSTR("<h3 style='text-align:center'>TankSync</h3>"));
  server.sendContent_P(PSTR("<div class='big' id='p'>--%</div>"));
  server.sendContent_P(PSTR("<table><tr><td>Water</td><td class='v'><span id='w'>--</span> cm</td></tr>"));
  server.sendContent_P(PSTR("<tr><td>Capacity</td><td class='v'><span id='cap'>--</span> L</td></tr>"));
  server.sendContent_P(PSTR("<tr><td>Battery</td><td class='v'><span id='b'>--%</span> (<span id='v'>--</span>V)</td></tr>"));
  server.sendContent_P(PSTR("<tr><td>Signal</td><td class='v'><span id='r'>--</span>dBm</td></tr>"));
  server.sendContent_P(PSTR("<tr><td>SNR</td><td class='v'><span id='snr'>--</span></td></tr></table>"));

  server.sendContent_P(PSTR("<div style='margin-top:15px'>"));
  server.sendContent_P(PSTR("<button onclick=\"location.href='/wifi'\">WiFi</button>"));
  server.sendContent_P(PSTR("<button onclick=\"location.href='/setup'\">Tank Setup</button>"));
  server.sendContent_P(PSTR("<button onclick=\"location.href='/mqtt'\">MQTT</button>"));
  server.sendContent_P(PSTR("<button onclick=\"location.href='/lora'\">LoRa</button>"));
  server.sendContent_P(PSTR("<button onclick=\"location.href='/alerts'\">Alerts</button>"));
  server.sendContent_P(PSTR("</div>"));
  yield();

  server.sendContent_P(PSTR("<script>function up(){fetch('/d').then(r=>r.json()).then(d=>{"));
  server.sendContent_P(PSTR("document.getElementById('p').textContent=d.p+'%';document.getElementById('w').textContent=d.w;"));
  server.sendContent_P(PSTR("document.getElementById('cap').textContent=d.cap;"));
  server.sendContent_P(PSTR("document.getElementById('b').textContent=d.b+'%';document.getElementById('v').textContent=d.volt;"));
  server.sendContent_P(PSTR("document.getElementById('r').textContent=d.rssi;document.getElementById('snr').textContent=d.snr})}"));
  server.sendContent_P(PSTR("up();setInterval(up,2000)</script></body></html>"));
  yield();

  server.sendContent("");  // Terminate chunked response
}

void handleScan(WebServer& server) {
#if DEBUG_VERBOSE
  Serial.println("WiFi scan requested");
#endif
  delay(5);

  // If in AP-only mode, temporarily enable STA mode for scanning
  wifi_mode_t originalMode = WiFi.getMode();
  bool needModeRestore = false;

  if (originalMode == WIFI_MODE_AP) {
    WiFi.mode(WIFI_AP_STA);
    delay(50);
    yield();
    delay(50);
    yield();
    needModeRestore = true;
  }

  // Perform blocking scan
  int n = WiFi.scanNetworks(false, false);  // Blocking, show hidden networks
  delay(5);

  if (n == WIFI_SCAN_FAILED) {
    n = 0;
  }
#if DEBUG_VERBOSE
  else {
    Serial.printf("Found %d networks\n", n);
  }
#endif

  // Build JSON response
  String j = "{\"n\":[";
  int networksAdded = 0;
  for (int i = 0; i < n && networksAdded < 15; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;  // Skip empty SSIDs

    if (networksAdded > 0) j += ",";

    // Escape special characters in SSID
    ssid.replace("\\", "\\\\");
    ssid.replace("\"", "\\\"");

    j += "{\"s\":\"" + ssid + "\",\"r\":" + String(WiFi.RSSI(i)) + "}";
    networksAdded++;
    yield();
  }
  j += "]}";

  // Clean up
  WiFi.scanDelete();
  yield();

  // Restore original WiFi mode if needed
  if (needModeRestore) {
    delay(100);
    WiFi.mode(WIFI_MODE_AP);
    delay(100);
    yield();
  }

  // Send response
  server.send(200, "application/json; charset=utf-8", j);
}

void handleConnect(WebServer& server) {
  yield();  // Feed watchdog

  String ssid = server.arg("s");
  String pass = server.arg("p");

  if (ssid.length() == 0) {
    server.send(200, "application/json", F("{\"ok\":false,\"m\":\"SSID required\"}"));
    return;
  }

  savedSSID = ssid;
  savedPassword = pass;

  yield();  // Feed watchdog before flash write
  saveWiFiCredentials();
  yield();  // Feed watchdog after flash write

  server.send(200, "application/json", F("{\"ok\":true,\"m\":\"Saved! Restarting...\"}"));
  delay(500);
  yield();
  ESP.restart();
}

void handleData(WebServer& server) {
  yield();  // CRITICAL: Feed watchdog before processing

  const char* loraStatus = !loraHardwareConnected ? "HW ERROR" : (tank.dataValid ? "OK" : "Wait");

  // Build JSON with snprintf to avoid String concatenation and memory fragmentation
  char json[256];
  snprintf(json, sizeof(json),
           "{\"p\":%d,\"w\":%d,\"cap\":%.1f,\"raw\":%d,\"b\":%d,\"volt\":%.2f,\"rssi\":%d,\"snr\":%d,\"s\":\"%s\"}",
           tank.waterPercent,
           tank.waterLevel,
           tank.tankCapacity,
           tank.rawDistance,
           tank.batteryPercent,
           tank.batteryVoltage,
           tank.rssi,
           tank.snr,
           loraStatus);

  yield();  // Feed watchdog before sending
  server.send(200, "application/json", json);
  yield();  // Feed watchdog after sending
}

void handleSetup(WebServer& server) {
  // Use chunked response to prevent memory fragmentation
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  yield();

  server.sendContent_P(PSTR("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Setup - TankSync</title>"));
  yield();

  server.sendContent_P(PSTR("<style>body{font-family:monospace;background:#000;color:#0f0;padding:15px;font-size:14px;max-width:500px;margin:auto}"));
  server.sendContent_P(PSTR("h3{text-align:center}.section{background:#111;border:1px solid#333;padding:15px;margin:15px 0;border-radius:5px}"));
  server.sendContent_P(PSTR(".label-row{display:flex;align-items:center;margin:15px 0 5px}.label-row label{flex:1}"));
  server.sendContent_P(PSTR(".help{width:20px;height:20px;background:#0ff;color:#000;border-radius:50%;text-align:center;line-height:20px;cursor:help;margin-left:8px;font-weight:bold}"));
  server.sendContent_P(PSTR("input{width:100%;padding:8px;background:#111;color:#0f0;border:1px solid#333;box-sizing:border-box}"));
  server.sendContent_P(PSTR("button{width:48%;padding:12px;border:none;margin:8px 1%;cursor:pointer;font-weight:bold}"));
  server.sendContent_P(PSTR(".save{background:#0f0;color:#000}.back{background:#333;color:#0f0}"));
  server.sendContent_P(PSTR(".info{color:#0ff;font-size:12px;margin:5px 0;line-height:1.4}"));
  server.sendContent_P(PSTR(".current{background:#0f0;color:#000;padding:10px;text-align:center;font-size:18px;font-weight:bold;border-radius:5px;margin:10px 0}"));
  server.sendContent_P(PSTR(".tip{color:#ff0;font-size:11px;font-style:italic;margin-top:3px}</style></head><body>"));
  yield();

  server.sendContent_P(PSTR("<h3>Tank Calibration</h3>"));
  server.sendContent_P(PSTR("<div class='section'><div style='color:#0ff;margin-bottom:10px'>Current Sensor Reading:</div>"));
  server.sendContent_P(PSTR("<div class='current' id='live'>--</div>"));
  server.sendContent_P(PSTR("<div class='tip'>This updates every 2 seconds. Use this to calibrate your tank.</div></div>"));
  yield();

  server.sendContent_P(PSTR("<form id='f'><div class='section'>"));
  server.sendContent_P(PSTR("<div class='label-row'><label>Distance When FULL (cm):</label>"));
  server.sendContent_P(PSTR("<span class='help' title='Sensor reading when tank is completely full'>i</span></div>"));

  char buf[64];
  snprintf(buf, sizeof(buf), "<input type='number' id='min' value='%d' required>", MIN_DISTANCE_CM);
  server.sendContent(buf);

  server.sendContent_P(PSTR("<div class='info'>Fill your tank completely, note the sensor reading above, and enter it here.</div>"));
  yield();

  server.sendContent_P(PSTR("<div class='label-row'><label>Distance When EMPTY (cm):</label>"));
  server.sendContent_P(PSTR("<span class='help' title='Sensor reading when tank is completely empty'>i</span></div>"));

  snprintf(buf, sizeof(buf), "<input type='number' id='max' value='%d' required>", MAX_DISTANCE_CM);
  server.sendContent(buf);

  server.sendContent_P(PSTR("<div class='info'>Empty your tank completely, note the sensor reading above, and enter it here.</div>"));
  yield();

  server.sendContent_P(PSTR("<div class='label-row'><label>Tank Capacity (Liters):</label>"));
  server.sendContent_P(PSTR("<span class='help' title='Total volume your tank can hold'>i</span></div>"));

  snprintf(buf, sizeof(buf), "<input type='number' step='0.1' id='cap' value='%.1f' required>", tank.tankCapacity);
  server.sendContent(buf);

  server.sendContent_P(PSTR("<div class='info'>Enter the total capacity of your tank in liters (e.g., 1000 for a 1000L tank).</div>"));
  yield();

  server.sendContent_P(PSTR("</div><div style='margin-top:20px'>"));
  server.sendContent_P(PSTR("<button type='button' class='back' onclick=\"location.href='/'\">Back</button>"));
  server.sendContent_P(PSTR("<button type='submit' class='save'>Save</button>"));
  server.sendContent_P(PSTR("</div></form>"));
  yield();

  server.sendContent_P(PSTR("<script>function up(){fetch('/d').then(r=>r.json()).then(d=>{"));
  server.sendContent_P(PSTR("document.getElementById('live').textContent=d.raw+' cm'})}"));
  server.sendContent_P(PSTR("up();setInterval(up,2000);"));
  server.sendContent_P(PSTR("document.getElementById('f').onsubmit=function(e){e.preventDefault();"));
  server.sendContent_P(PSTR("let min=document.getElementById('min').value;"));
  server.sendContent_P(PSTR("let max=document.getElementById('max').value;"));
  server.sendContent_P(PSTR("let cap=document.getElementById('cap').value;"));
  server.sendContent_P(PSTR("if(parseInt(min)>=parseInt(max)){alert('Error: Distance when FULL must be less than when EMPTY!');return}"));
  server.sendContent_P(PSTR("fetch('/save?min='+min+'&max='+max+'&cap='+cap).then(r=>r.json()).then(d=>{"));
  server.sendContent_P(PSTR("alert(d.m);if(d.ok)location.href='/'})}</script></body></html>"));
  yield();

  server.sendContent("");  // Terminate chunked response
}

void handleSaveSettings(WebServer& server) {
  yield();  // Feed watchdog

  int minDist = server.arg("min").toInt();
  int maxDist = server.arg("max").toInt();
  float capacity = server.arg("cap").toFloat();

  // Validation
  if (minDist >= maxDist) {
    server.send(200, "application/json", "{\"ok\":false,\"m\":\"Error: Min must be less than Max!\"}");
    return;
  }

  yield();  // Feed watchdog before flash write
  saveTankSettings(minDist, maxDist, capacity);
  yield();  // Feed watchdog after flash write

  server.send(200, "application/json", "{\"ok\":true,\"m\":\"Calibration saved!\"}");
}

void handleWifi(WebServer& server) {
  // Use chunked response to prevent memory fragmentation
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  yield();

  server.sendContent_P(PSTR("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>WiFi - TankSync</title>"));
  yield();

  server.sendContent_P(PSTR("<style>body{font-family:monospace;background:#000;color:#0f0;padding:15px;font-size:14px;max-width:500px;margin:auto}"));
  server.sendContent_P(PSTR("h3{text-align:center}.section{background:#111;border:1px solid#333;padding:15px;margin:15px 0;border-radius:5px}"));
  server.sendContent_P(PSTR(".status{color:#0ff;margin:10px 0;padding:10px;background:#112;border-radius:5px}"));
  server.sendContent_P(PSTR("input,select{width:100%;padding:8px;background:#111;color:#0f0;border:1px solid#333;box-sizing:border-box;margin:5px 0}"));
  server.sendContent_P(PSTR("button{width:48%;padding:12px;border:none;margin:8px 1%;cursor:pointer;font-weight:bold}"));
  server.sendContent_P(PSTR(".save{background:#0f0;color:#000}.back{background:#333;color:#0f0}.scan{background:#00f;color:#fff;width:100%}"));
  server.sendContent_P(PSTR(".danger{background:#f00;color:#fff;width:100%;margin-top:20px}"));
  server.sendContent_P(PSTR(".spinner{display:inline-block;width:12px;height:12px;border:2px solid #fff;border-top:2px solid transparent;border-radius:50%;animation:spin 1s linear infinite;margin-right:8px}"));
  server.sendContent_P(PSTR("@keyframes spin{0%{transform:rotate(0deg)}100%{transform:rotate(360deg)}}</style></head><body>"));
  yield();

  server.sendContent_P(PSTR("<h3>WiFi Configuration</h3>"));

  // Current status
  server.sendContent_P(PSTR("<div class='section'><div style='color:#0ff;margin-bottom:10px'>Current Status:</div>"));
  server.sendContent_P(PSTR("<div class='status'>"));

  WiFiState state = getWiFiState();
  if (state == WIFI_STATE_CONNECTED) {
    char buf[128];
    snprintf(buf, sizeof(buf), "Connected to: %s<br>IP Address: %s<br>Signal: %d dBm",
             WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    server.sendContent(buf);
  } else if (state == WIFI_STATE_AP_MODE) {
    server.sendContent_P(PSTR("AP Mode: TankSync<br>IP: 192.168.4.1"));
  } else {
    server.sendContent_P(PSTR("Disconnected"));
  }
  server.sendContent_P(PSTR("</div></div>"));
  yield();

  // WiFi Configuration Form
  server.sendContent_P(PSTR("<form id='f'><div class='section'>"));
  server.sendContent_P(PSTR("<label>Network SSID:</label>"));
  server.sendContent_P(PSTR("<select id='ssid'><option value=''>Select network...</option></select>"));
  server.sendContent_P(PSTR("<input type='text' id='ssid_manual' placeholder='Or enter SSID manually'>"));
  server.sendContent_P(PSTR("<label>Password:</label>"));
  server.sendContent_P(PSTR("<input type='password' id='pass' placeholder='WiFi Password'>"));
  server.sendContent_P(PSTR("<button type='button' class='scan' onclick='scan()'>Scan Networks</button>"));
  server.sendContent_P(PSTR("</div><div style='margin-top:20px'>"));
  server.sendContent_P(PSTR("<button type='button' class='back' onclick=\"location.href='/'\">Back</button>"));
  server.sendContent_P(PSTR("<button type='submit' class='save'>Connect</button>"));
  server.sendContent_P(PSTR("</div></form>"));

  // Reset button
  server.sendContent_P(PSTR("<button type='button' class='danger' onclick='reset()'>Reset WiFi &amp; Restart</button>"));
  yield();

  // JavaScript
  server.sendContent_P(PSTR("<script>"));

  // Scan function
  server.sendContent_P(PSTR("function scan(){"));
  server.sendContent_P(PSTR("let btn=document.querySelector('.scan');"));
  server.sendContent_P(PSTR("btn.innerHTML='<span class=\"spinner\"></span>Scanning...';btn.disabled=true;"));
  server.sendContent_P(PSTR("console.log('Starting WiFi scan...');"));
  server.sendContent_P(PSTR("fetch('/s').then(r=>{console.log('Scan response status:',r.status);if(!r.ok)throw new Error('HTTP '+r.status);return r.json()}).then(d=>{"));
  server.sendContent_P(PSTR("console.log('Scan data:',d);"));
  server.sendContent_P(PSTR("let sel=document.getElementById('ssid');sel.innerHTML='<option value=\"\">Select network...</option>';"));
  server.sendContent_P(PSTR("if(!d.n||d.n.length===0){sel.innerHTML='<option value=\"\">No networks found</option>';btn.innerHTML='⚠ No networks';btn.disabled=false;"));
  server.sendContent_P(PSTR("setTimeout(()=>{btn.innerHTML='Scan Networks';btn.disabled=false},3000);return}"));
  server.sendContent_P(PSTR("d.n.forEach(n=>{let opt=document.createElement('option');opt.value=n.s;opt.textContent=n.s+' ('+n.r+' dBm)';sel.appendChild(opt)});"));
  server.sendContent_P(PSTR("btn.innerHTML='✓ Found '+d.n.length+' networks';btn.disabled=false;"));
  server.sendContent_P(PSTR("setTimeout(()=>{btn.innerHTML='Scan Networks'},3000)"));
  server.sendContent_P(PSTR("}).catch(e=>{console.error('Scan error:',e);btn.innerHTML='✗ Scan failed';btn.disabled=false;"));
  server.sendContent_P(PSTR("setTimeout(()=>{btn.innerHTML='Scan Networks';btn.disabled=false},3000)})}"));

  // Form submit handler
  server.sendContent_P(PSTR("document.getElementById('f').onsubmit=function(e){e.preventDefault();"));
  server.sendContent_P(PSTR("let s=document.getElementById('ssid').value||document.getElementById('ssid_manual').value;"));
  server.sendContent_P(PSTR("let p=document.getElementById('pass').value;"));
  server.sendContent_P(PSTR("if(!s){alert('Please select or enter a network SSID');return}"));
  server.sendContent_P(PSTR("if(confirm('Connect to '+s+'? Device will restart.')){"));
  server.sendContent_P(PSTR("fetch('/c?s='+encodeURIComponent(s)+'&p='+encodeURIComponent(p)).then(r=>r.json()).then(d=>{alert(d.m)})}};"));

  // Reset function
  server.sendContent_P(PSTR("function reset(){if(confirm('Reset WiFi credentials and restart?')){"));
  server.sendContent_P(PSTR("fetch('/r').then(()=>{alert('Resetting...')})}}"));

  server.sendContent_P(PSTR("</script></body></html>"));
  yield();

  server.sendContent("");  // Terminate chunked response
}

void handleMqtt(WebServer& server) {
  // Use chunked response to prevent memory fragmentation
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  yield();

  server.sendContent_P(PSTR("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>MQTT - TankSync</title>"));
  yield();

  server.sendContent_P(PSTR("<style>body{font-family:monospace;background:#000;color:#0f0;padding:15px;font-size:14px;max-width:500px;margin:auto}"));
  server.sendContent_P(PSTR("h3{text-align:center}.section{background:#111;border:1px solid#333;padding:15px;margin:15px 0;border-radius:5px}"));
  server.sendContent_P(PSTR("label{display:block;margin:15px 0 5px}"));
  server.sendContent_P(PSTR("input{width:100%;padding:8px;background:#111;color:#0f0;border:1px solid#333;box-sizing:border-box}"));
  server.sendContent_P(PSTR("button{width:48%;padding:12px;border:none;margin:8px 1%;cursor:pointer;font-weight:bold}"));
  server.sendContent_P(PSTR(".save{background:#0f0;color:#000}.back{background:#333;color:#0f0}"));
  server.sendContent_P(PSTR(".toggle{display:flex;align-items:center;margin:15px 0}.toggle input[type=checkbox]{width:auto;margin-right:10px}"));
  server.sendContent_P(PSTR(".info{color:#0ff;font-size:12px;margin-top:3px}</style></head><body>"));
  yield();

  server.sendContent_P(PSTR("<h3>MQTT Configuration</h3>"));
  server.sendContent_P(PSTR("<form id='f'><div class='section'>"));

  // MQTT Enabled toggle
  server.sendContent_P(PSTR("<div class='toggle'><input type='checkbox' id='enabled'"));
  if (mqtt_enabled) server.sendContent_P(PSTR(" checked"));
  server.sendContent_P(PSTR("><label for='enabled' style='margin:0'>Enable MQTT</label></div>"));

  // MQTT Server
  server.sendContent_P(PSTR("<label>MQTT Server:</label>"));
  char buf[128];
  snprintf(buf, sizeof(buf), "<input type='text' id='server' value='%s' placeholder='192.168.1.100'>", mqtt_server.c_str());
  server.sendContent(buf);
  server.sendContent_P(PSTR("<div class='info'>IP address or hostname of MQTT broker</div>"));

  // MQTT Port
  server.sendContent_P(PSTR("<label>Port:</label>"));
  snprintf(buf, sizeof(buf), "<input type='number' id='port' value='%d' placeholder='1883'>", mqtt_port);
  server.sendContent(buf);
  server.sendContent_P(PSTR("<div class='info'>Default: 1883</div>"));
  yield();

  // MQTT Username
  server.sendContent_P(PSTR("<label>Username (optional):</label>"));
  snprintf(buf, sizeof(buf), "<input type='text' id='user' value='%s' placeholder='mqtt-user'>", mqtt_user.c_str());
  server.sendContent(buf);

  // MQTT Password
  server.sendContent_P(PSTR("<label>Password (optional):</label>"));
  snprintf(buf, sizeof(buf), "<input type='password' id='pass' value='%s' placeholder='password'>", mqtt_password.c_str());
  server.sendContent(buf);
  server.sendContent_P(PSTR("<div class='info'>Leave blank if no authentication required</div>"));
  yield();

  server.sendContent_P(PSTR("</div><div style='margin-top:20px'>"));
  server.sendContent_P(PSTR("<button type='button' class='back' onclick=\"location.href='/'\">Back</button>"));
  server.sendContent_P(PSTR("<button type='submit' class='save'>Save</button>"));
  server.sendContent_P(PSTR("</div></form>"));
  yield();

  // JavaScript
  server.sendContent_P(PSTR("<script>document.getElementById('f').onsubmit=function(e){e.preventDefault();"));
  server.sendContent_P(PSTR("let enabled=document.getElementById('enabled').checked?'1':'0';"));
  server.sendContent_P(PSTR("let server=document.getElementById('server').value;"));
  server.sendContent_P(PSTR("let port=document.getElementById('port').value;"));
  server.sendContent_P(PSTR("let user=document.getElementById('user').value;"));
  server.sendContent_P(PSTR("let pass=document.getElementById('pass').value;"));
  server.sendContent_P(PSTR("fetch('/mqtt_save?enabled='+enabled+'&server='+encodeURIComponent(server)+'&port='+port+'&user='+encodeURIComponent(user)+'&pass='+encodeURIComponent(pass))"));
  server.sendContent_P(PSTR(".then(r=>r.json()).then(d=>{alert(d.m);if(d.ok)location.href='/'})}</script></body></html>"));
  yield();

  server.sendContent("");  // Terminate chunked response
}

void handleMqttSave(WebServer& server) {
  yield();  // Feed watchdog

  mqtt_enabled = (server.arg("enabled") == "1");
  mqtt_server = server.arg("server");
  mqtt_port = server.arg("port").toInt();
  mqtt_user = server.arg("user");
  mqtt_password = server.arg("pass");

  // Validation
  if (mqtt_enabled && mqtt_server.length() == 0) {
    server.send(200, "application/json", "{\"ok\":false,\"m\":\"Server address required!\"}");
    return;
  }
  if (mqtt_port < 1 || mqtt_port > 65535) {
    mqtt_port = DEFAULT_MQTT_PORT;
  }

  yield();  // Feed watchdog before flash write
  saveMqttSettings();
  yield();  // Feed watchdog after flash write

  server.send(200, "application/json", "{\"ok\":true,\"m\":\"MQTT settings saved!\"}");
}

void handleLora(WebServer& server) {
  // Use chunked response to prevent memory fragmentation
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  yield();

  server.sendContent_P(PSTR("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>LoRa - TankSync</title>"));
  yield();

  server.sendContent_P(PSTR("<style>body{font-family:monospace;background:#000;color:#0f0;padding:15px;font-size:14px;max-width:500px;margin:auto}"));
  server.sendContent_P(PSTR("h3{text-align:center}.section{background:#111;border:1px solid#333;padding:15px;margin:15px 0;border-radius:5px}"));
  server.sendContent_P(PSTR("label{display:block;margin:15px 0 5px}"));
  server.sendContent_P(PSTR("input{width:100%;padding:8px;background:#111;color:#0f0;border:1px solid#333;box-sizing:border-box}"));
  server.sendContent_P(PSTR("button{width:48%;padding:12px;border:none;margin:8px 1%;cursor:pointer;font-weight:bold}"));
  server.sendContent_P(PSTR(".save{background:#0f0;color:#000}.back{background:#333;color:#0f0}"));
  server.sendContent_P(PSTR(".info{color:#0ff;font-size:12px;margin-top:3px}.warning{color:#ff0;font-size:12px;margin-top:10px}</style></head><body>"));
  yield();

  server.sendContent_P(PSTR("<h3>LoRa Configuration</h3>"));
  server.sendContent_P(PSTR("<form id='f'><div class='section'>"));

  // LoRa Frequency
  server.sendContent_P(PSTR("<label>Frequency (Hz):</label>"));
  char buf[128];
  snprintf(buf, sizeof(buf), "<input type='text' id='freq' value='%s' placeholder='865000000'>", LORA_FREQUENCY);
  server.sendContent(buf);
  server.sendContent_P(PSTR("<div class='info'>Common: 433000000 (433MHz), 865000000 (865MHz), 915000000 (915MHz)</div>"));
  server.sendContent_P(PSTR("<div class='warning'>WARNING: Must match transmitter! Frequency must comply with local regulations.</div>"));
  yield();

  // Network ID
  server.sendContent_P(PSTR("<label>Network ID (0-255):</label>"));
  snprintf(buf, sizeof(buf), "<input type='number' id='netid' value='%d' min='0' max='255' placeholder='6'>", LORA_NETWORK_ID);
  server.sendContent(buf);
  server.sendContent_P(PSTR("<div class='info'>Must match transmitter for communication</div>"));

  // Device Address
  server.sendContent_P(PSTR("<label>Device Address (0-255):</label>"));
  snprintf(buf, sizeof(buf), "<input type='number' id='addr' value='%d' min='0' max='255' placeholder='2'>", MY_ADDRESS);
  server.sendContent(buf);
  server.sendContent_P(PSTR("<div class='info'>This receiver's unique address (usually 2, transmitter is 1)</div>"));
  yield();

  server.sendContent_P(PSTR("</div><div style='margin-top:20px'>"));
  server.sendContent_P(PSTR("<button type='button' class='back' onclick=\"location.href='/'\">Back</button>"));
  server.sendContent_P(PSTR("<button type='submit' class='save'>Save &amp; Restart</button>"));
  server.sendContent_P(PSTR("</div></form>"));
  yield();

  // JavaScript
  server.sendContent_P(PSTR("<script>document.getElementById('f').onsubmit=function(e){e.preventDefault();"));
  server.sendContent_P(PSTR("let freq=document.getElementById('freq').value;"));
  server.sendContent_P(PSTR("let netid=document.getElementById('netid').value;"));
  server.sendContent_P(PSTR("let addr=document.getElementById('addr').value;"));
  server.sendContent_P(PSTR("if(confirm('Save LoRa settings? Device will restart.')){"));
  server.sendContent_P(PSTR("fetch('/lora_save?freq='+encodeURIComponent(freq)+'&netid='+netid+'&addr='+addr)"));
  server.sendContent_P(PSTR(".then(r=>r.json()).then(d=>{alert(d.m)})}}</script></body></html>"));
  yield();

  server.sendContent("");  // Terminate chunked response
}

void handleLoraSave(WebServer& server) {
  yield();  // Feed watchdog

  String freq = server.arg("freq");
  int netid = server.arg("netid").toInt();
  int addr = server.arg("addr").toInt();

  // Validation
  if (freq.length() == 0 || freq.toInt() < 100000000) {
    server.send(200, "application/json", "{\"ok\":false,\"m\":\"Invalid frequency!\"}");
    return;
  }
  if (netid < 0 || netid > 255) {
    server.send(200, "application/json", "{\"ok\":false,\"m\":\"Network ID must be 0-255!\"}");
    return;
  }
  if (addr < 0 || addr > 255) {
    server.send(200, "application/json", "{\"ok\":false,\"m\":\"Address must be 0-255!\"}");
    return;
  }

  yield();  // Feed watchdog before flash write
  saveLoRaSettings(freq, netid, addr);
  yield();  // Feed watchdog after flash write

  server.send(200, "application/json", "{\"ok\":true,\"m\":\"LoRa settings saved! Restarting...\"}");
  delay(1000);
  yield();
  ESP.restart();
}

void handleAlerts(WebServer& server) {
  // Use chunked response to prevent memory fragmentation
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  yield();

  server.sendContent_P(PSTR("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Alerts - TankSync</title>"));
  yield();

  server.sendContent_P(PSTR("<style>body{font-family:monospace;background:#000;color:#0f0;padding:15px;font-size:14px;max-width:500px;margin:auto}"));
  server.sendContent_P(PSTR("h3{text-align:center}.section{background:#111;border:1px solid#333;padding:15px;margin:15px 0;border-radius:5px}"));
  server.sendContent_P(PSTR("label{display:block;margin:15px 0 5px}"));
  server.sendContent_P(PSTR("input{width:100%;padding:8px;background:#111;color:#0f0;border:1px solid#333;box-sizing:border-box}"));
  server.sendContent_P(PSTR("button{width:48%;padding:12px;border:none;margin:8px 1%;cursor:pointer;font-weight:bold}"));
  server.sendContent_P(PSTR(".save{background:#0f0;color:#000}.back{background:#333;color:#0f0}"));
  server.sendContent_P(PSTR(".toggle{display:flex;align-items:center;margin:15px 0}.toggle input[type=checkbox]{width:auto;margin-right:10px}"));
  server.sendContent_P(PSTR(".info{color:#0ff;font-size:12px;margin-top:3px}</style></head><body>"));
  yield();

  server.sendContent_P(PSTR("<h3>Alert Configuration</h3>"));
  server.sendContent_P(PSTR("<form id='f'><div class='section'>"));

  // Alerts Enabled toggle
  server.sendContent_P(PSTR("<div class='toggle'><input type='checkbox' id='enabled'"));
  if (alerts_enabled) server.sendContent_P(PSTR(" checked"));
  server.sendContent_P(PSTR("><label for='enabled' style='margin:0'>Enable Alerts</label></div>"));

  // Low Water Threshold
  server.sendContent_P(PSTR("<label>Low Water Alert (%):</label>"));
  char buf[128];
  snprintf(buf, sizeof(buf), "<input type='number' id='water' value='%d' min='0' max='100' placeholder='20'>", alert_low_water);
  server.sendContent(buf);
  server.sendContent_P(PSTR("<div class='info'>Alert when water level drops below this percentage</div>"));
  yield();

  // Low Battery Threshold
  server.sendContent_P(PSTR("<label>Low Battery Alert (%):</label>"));
  snprintf(buf, sizeof(buf), "<input type='number' id='battery' value='%d' min='0' max='100' placeholder='20'>", alert_low_battery);
  server.sendContent(buf);
  server.sendContent_P(PSTR("<div class='info'>Alert when battery level drops below this percentage</div>"));

  // Alert Email (optional)
  server.sendContent_P(PSTR("<label>Alert Email (optional):</label>"));
  snprintf(buf, sizeof(buf), "<input type='email' id='email' value='%s' placeholder='user@example.com'>", alert_email.c_str());
  server.sendContent(buf);
  server.sendContent_P(PSTR("<div class='info'>Future feature: Email alerts (requires SMTP configuration)</div>"));
  yield();

  server.sendContent_P(PSTR("</div><div style='margin-top:20px'>"));
  server.sendContent_P(PSTR("<button type='button' class='back' onclick=\"location.href='/'\">Back</button>"));
  server.sendContent_P(PSTR("<button type='submit' class='save'>Save</button>"));
  server.sendContent_P(PSTR("</div></form>"));
  yield();

  // JavaScript
  server.sendContent_P(PSTR("<script>document.getElementById('f').onsubmit=function(e){e.preventDefault();"));
  server.sendContent_P(PSTR("let enabled=document.getElementById('enabled').checked?'1':'0';"));
  server.sendContent_P(PSTR("let water=document.getElementById('water').value;"));
  server.sendContent_P(PSTR("let battery=document.getElementById('battery').value;"));
  server.sendContent_P(PSTR("let email=document.getElementById('email').value;"));
  server.sendContent_P(PSTR("fetch('/alerts_save?enabled='+enabled+'&water='+water+'&battery='+battery+'&email='+encodeURIComponent(email))"));
  server.sendContent_P(PSTR(".then(r=>r.json()).then(d=>{alert(d.m);if(d.ok)location.href='/'})}</script></body></html>"));
  yield();

  server.sendContent("");  // Terminate chunked response
}

void handleAlertsSave(WebServer& server) {
  yield();  // Feed watchdog

  alerts_enabled = (server.arg("enabled") == "1");
  alert_low_water = server.arg("water").toInt();
  alert_low_battery = server.arg("battery").toInt();
  alert_email = server.arg("email");

  // Validation
  if (alert_low_water < 0 || alert_low_water > 100) {
    alert_low_water = DEFAULT_ALERT_LOW_WATER;
  }
  if (alert_low_battery < 0 || alert_low_battery > 100) {
    alert_low_battery = DEFAULT_ALERT_LOW_BATTERY;
  }

  yield();  // Feed watchdog before flash write
  saveAlertSettings();
  yield();  // Feed watchdog after flash write

  server.send(200, "application/json", "{\"ok\":true,\"m\":\"Alert settings saved!\"}");
}

void handleReset(WebServer& server) {
  clearWiFiCredentials();
  server.send(200, "text/plain", "OK");
  delay(1000);
  ESP.restart();
}
