#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <PubSubClient.h>

// ================= WATCHDOG CONFIG =================
#define WDT_TIMEOUT 30 // 30 detik

// ================= GLOBALS & PREFERENCES =================
Preferences preferences;
String ssid = "realme 8i";
String password = "satusatusatu";
bool isAPMode = false;

// ================= MQTT CONFIG =================
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* mqtt_topic_data = "jemuran/data";
const char* mqtt_topic_status = "jemuran/status";
const char* mqtt_topic_command = "jemuran/command";

WiFiClient espClient;
PubSubClient mqttClient(espClient);
unsigned long lastMqttPublish = 0;
unsigned long lastMqttReconnect = 0;
#define MQTT_PUBLISH_INTERVAL 5000
#define MQTT_RECONNECT_INTERVAL 5000

// ================= WEATHER CONFIG =================
#define LATITUDE   -6.9
#define LONGITUDE  107.5
#define OPENWEATHER_API_KEY "b4ea8e4b114aaf44caf333dc5a99ec99"

// ================= PIN =================
#define SERVO_PIN 23
#define RAIN_SENSOR_PIN 39
#define LDR_SENSOR_PIN 36
#define STATUS_LED_PIN 2 // Built-in LED untuk status

// Threshold Variables
int thresholdRain = 3000;
int thresholdLdr = 1200;

// Sensor States & Debounce
int rainVal = 0;
int ldrVal = 0;
String rainStatus = "--";
String ldrStatus = "--";

bool rainStableState = false;
bool ldrStableState = false;

unsigned long lastRainChangeTime = 0;
unsigned long lastLdrChangeTime = 0;
unsigned long DEBOUNCE_DELAY = 3000; // 3 detik untuk stabilitas lebih baik

// Debounce Counter untuk validasi
int rainStableCounter = 0;
int ldrStableCounter = 0;
#define STABLE_COUNT_REQUIRED 3 // Harus stabil 3 kali berturut-turut

String systemStatus = "Memulai...";

// ================= NTP =================
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600;
bool timeInitialized = false;

// ================= OBJECT =================
WebServer server(80);
Servo myServo;

// ================= STATE =================
bool autoMode = true;
int servoAngle = 0;
int targetServoAngle = 0; // Untuk smooth movement
int openHour = 8;
int closeHour = 16;

// ================= WEATHER DATA =================
struct Forecast {
  String hour;
  String icon;
  bool valid;
};

Forecast forecast[4];
String weatherNow = "UNKNOWN";
String weatherIcon = "01d";
bool weatherIsGood = false;
unsigned long lastWeatherUpdate = 0;
unsigned long lastWeatherAttempt = 0;
int lastForecastDay = -1;
int weatherRetryCount = 0;
bool weatherDataValid = false;

// ================= TASK TIMING =================
unsigned long lastSensorUpdate = 0;
unsigned long lastServoUpdate = 0;
unsigned long lastLEDUpdate = 0;
#define SENSOR_UPDATE_INTERVAL 200
#define SERVO_UPDATE_INTERVAL 50  // Untuk smooth movement
#define LED_UPDATE_INTERVAL 500
#define WEATHER_UPDATE_INTERVAL 300000  // 5 menit
#define WEATHER_RETRY_INTERVAL 60000    // 1 menit jika gagal

// ================= FORWARD DECLARATIONS =================
void handleRoot();
void handleData();
void handleMode();
void handleServo();
void handleSetTime();
void handleSettings();
void setupAPMode();
void handleConfig();
void handleWifiSave();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void mqttReconnect();
void publishMqttData();

// ================= MQTT CALLBACK =================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.print("[MQTT] 📩 Received: ");
  Serial.println(message);
  
  // Parse command dari Node-RED
  if (message == "OPEN") {
    autoMode = false;
    targetServoAngle = 180;
    Serial.println("[MQTT] Command: OPEN");
  } else if (message == "CLOSE") {
    autoMode = false;
    targetServoAngle = 0;
    Serial.println("[MQTT] Command: CLOSE");
  } else if (message == "AUTO") {
    autoMode = true;
    Serial.println("[MQTT] Command: AUTO MODE");
  }
}

// ================= MQTT RECONNECT (Non-blocking) =================
void mqttReconnect() {
  if (mqttClient.connected()) return;
  if (millis() - lastMqttReconnect < MQTT_RECONNECT_INTERVAL) return;
  
  lastMqttReconnect = millis();
  
  Serial.print("[MQTT] 🔄 Connecting to broker...");
  String clientId = "jemuran-" + String(random(0xffff), HEX);
  
  if (mqttClient.connect(clientId.c_str())) {
    Serial.println(" ✅ Connected!");
    mqttClient.subscribe(mqtt_topic_command);
    Serial.printf("[MQTT] 📡 Subscribed to: %s\n", mqtt_topic_command);
  } else {
    Serial.printf(" ❌ Failed (rc=%d)\n", mqttClient.state());
  }
}

// ================= MQTT PUBLISH =================
void publishMqttData() {
  if (!mqttClient.connected()) return;
  if (millis() - lastMqttPublish < MQTT_PUBLISH_INTERVAL) return;
  
  lastMqttPublish = millis();
  
  // Format JSON untuk Node-RED
  StaticJsonDocument<512> doc;
  doc["rain_val"] = rainVal;
  doc["ldr_val"] = ldrVal;
  doc["rain_status"] = rainStatus;
  doc["ldr_status"] = ldrStatus;
  doc["servo_angle"] = servoAngle;
  doc["mode"] = autoMode ? "AUTO" : "MANUAL";
  doc["weather"] = weatherNow;
  doc["status"] = systemStatus;
  doc["weather_good"] = weatherIsGood;
  
  String json;
  serializeJson(doc, json);
  
  mqttClient.publish(mqtt_topic_data, json.c_str());
  Serial.println("[MQTT] 📤 Published data");
}

// ================= TIME HELPER =================
String getTimeString(int &hourNow, bool &valid) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    hourNow = -1;
    valid = false;
    return "--:--";
  }
  hourNow = timeinfo.tm_hour;
  valid = true;
  timeInitialized = true;
  char buf[6];
  sprintf(buf, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  return String(buf);
}

// ================= LED STATUS =================
void updateStatusLED() {
  if (millis() - lastLEDUpdate < LED_UPDATE_INTERVAL) return;
  lastLEDUpdate = millis();
  
  static bool ledState = false;
  
  if (isAPMode) {
    // Blink cepat di AP mode
    ledState = !ledState;
    digitalWrite(STATUS_LED_PIN, ledState);
  } else if (WiFi.status() != WL_CONNECTED) {
    // Blink lambat jika disconnect
    static int blinkCount = 0;
    blinkCount++;
    if (blinkCount % 4 == 0) {
      ledState = !ledState;
      digitalWrite(STATUS_LED_PIN, ledState);
    }
  } else {
    // ON solid jika connected
    digitalWrite(STATUS_LED_PIN, HIGH);
  }
}

// ================= WEATHER NOW WITH RETRY =================
void updateWeatherNow() {
  if (WiFi.status() != WL_CONNECTED || isAPMode) {
    weatherDataValid = false;
    return;
  }

  HTTPClient http;
  String url = "http://api.openweathermap.org/data/2.5/weather?";
  url += "lat=" + String(LATITUDE);
  url += "&lon=" + String(LONGITUDE);
  url += "&units=metric";
  url += "&appid=" + String(OPENWEATHER_API_KEY);

  http.begin(url);
  http.setTimeout(10000); // 10 detik timeout
  
  Serial.println("[WEATHER] Fetching current weather...");
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    
    // Gunakan StaticJsonDocument untuk hindari fragmentasi
    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      if (!doc["weather"][0]["description"].isNull()) {
        weatherNow = doc["weather"][0]["description"].as<String>();
      }
      if (!doc["weather"][0]["icon"].isNull()) {
        weatherIcon = doc["weather"][0]["icon"].as<String>();
      }
      if (!doc["weather"][0]["id"].isNull()) {
        int id = doc["weather"][0]["id"].as<int>();
        // 800 = clear, 801-804 = clouds (few/scattered/broken/overcast)
        // Servo tetap buka untuk cuaca cerah dan berawan
        weatherIsGood = (id == 800 || (id >= 801 && id <= 804));
      }
      weatherDataValid = true;
      weatherRetryCount = 0;
      Serial.printf("[WEATHER] Success: %s (Good: %s)\n", 
                    weatherNow.c_str(), weatherIsGood ? "Yes" : "No");
    } else {
      Serial.printf("[WEATHER] JSON Parse Error: %s\n", error.c_str());
      weatherDataValid = false;
    }
  } else {
    Serial.printf("[WEATHER] HTTP Error: %d\n", httpCode);
    weatherDataValid = false;
    weatherRetryCount++;
  }
  http.end();
}

// ================= FORECAST WITH IMPROVED LOGIC =================
void updateForecast() {
  if (WiFi.status() != WL_CONNECTED || isAPMode) return;

  HTTPClient http;
  String url = "http://api.openweathermap.org/data/2.5/forecast?";
  url += "lat=" + String(LATITUDE);
  url += "&lon=" + String(LONGITUDE);
  url += "&units=metric";
  url += "&appid=" + String(OPENWEATHER_API_KEY);

  http.begin(url);
  http.setTimeout(10000);
  
  Serial.println("[FORECAST] Fetching...");
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    
    // Gunakan heap allocation hanya jika perlu
    DynamicJsonDocument* doc = new DynamicJsonDocument(8192);
    DeserializationError error = deserializeJson(*doc, payload);

    if (!error) {
      struct tm timeNow;
      if (!getLocalTime(&timeNow)) {
        delete doc;
        return;
      }
      
      int currentDay = timeNow.tm_mday;
      
      // Reset forecast jika ganti hari atau first run
      if (currentDay != lastForecastDay) {
        for (int i = 0; i < 4; i++) {
          forecast[i].hour = "--:--";
          forecast[i].icon = "01d";
          forecast[i].valid = false;
        }
        lastForecastDay = currentDay;
      }
      
      // Parse forecast data
      int forecastIndex = 0;
      time_t currentTime = time(NULL) + gmtOffset_sec;
      
      for (int idx = 0; idx < 40 && forecastIndex < 4; idx++) {
        if ((*doc)["list"][idx].isNull()) break;

        time_t t = (*doc)["list"][idx]["dt"];
        time_t local_t = t + gmtOffset_sec;
        
        // Hanya tampilkan forecast yang belum lewat
        if (local_t > currentTime) {
          struct tm timeinfo;
          gmtime_r(&local_t, &timeinfo);
          
          int hour = timeinfo.tm_hour;
          int minute = timeinfo.tm_min;
          
          char buf[6];
          sprintf(buf, "%02d:%02d", hour, minute);
          String timeStr = String(buf);
          
          forecast[forecastIndex].hour = timeStr;
          if (!(*doc)["list"][idx]["weather"][0]["icon"].isNull()) {
            forecast[forecastIndex].icon = (*doc)["list"][idx]["weather"][0]["icon"].as<String>();
          }
          forecast[forecastIndex].valid = true;
          forecastIndex++;
        }
      }
      Serial.printf("[FORECAST] Success: %d entries\n", forecastIndex);
    } else {
      Serial.printf("[FORECAST] JSON Error: %s\n", error.c_str());
    }
    delete doc; // Free memory
  } else {
    Serial.printf("[FORECAST] HTTP Error: %d\n", httpCode);
  }
  http.end();
}

// ================= SENSOR UPDATE WITH IMPROVED DEBOUNCE =================
void updateSensors() {
  if (millis() - lastSensorUpdate < SENSOR_UPDATE_INTERVAL) return;
  lastSensorUpdate = millis();
  
  // Baca sensor
  int rawRain = analogRead(RAIN_SENSOR_PIN);
  int rawLdr = analogRead(LDR_SENSOR_PIN);
  rainVal = rawRain;
  ldrVal = rawLdr;
  
  // ========== RAIN DEBOUNCE WITH COUNTER ==========
  bool currentRainReading = (rawRain < thresholdRain);
  static bool lastRainReading = false;
  
  if (currentRainReading != lastRainReading) {
    lastRainChangeTime = millis();
    rainStableCounter = 0; // Reset counter
  }
  lastRainReading = currentRainReading;
  
  if ((millis() - lastRainChangeTime) > DEBOUNCE_DELAY) {
    if (currentRainReading == rainStableState) {
      rainStableCounter++;
      if (rainStableCounter >= STABLE_COUNT_REQUIRED) {
        rainStableCounter = STABLE_COUNT_REQUIRED; // Cap it
      }
    } else {
      rainStableCounter++;
      if (rainStableCounter >= STABLE_COUNT_REQUIRED) {
        rainStableState = currentRainReading;
        Serial.printf("[SENSOR] Rain state changed: %s\n", 
                      rainStableState ? "HUJAN" : "KERING");
      }
    }
  }
  rainStatus = rainStableState ? "Hujan" : "Tidak Hujan";
  
  // ========== LDR DEBOUNCE WITH COUNTER ==========
  bool currentLdrReading = (rawLdr < thresholdLdr);
  static bool lastLdrReading = false;
  
  if (currentLdrReading != lastLdrReading) {
    lastLdrChangeTime = millis();
    ldrStableCounter = 0;
  }
  lastLdrReading = currentLdrReading;
  
  if ((millis() - lastLdrChangeTime) > DEBOUNCE_DELAY) {
    if (currentLdrReading == ldrStableState) {
      ldrStableCounter++;
      if (ldrStableCounter >= STABLE_COUNT_REQUIRED) {
        ldrStableCounter = STABLE_COUNT_REQUIRED;
      }
    } else {
      ldrStableCounter++;
      if (ldrStableCounter >= STABLE_COUNT_REQUIRED) {
        ldrStableState = currentLdrReading;
        Serial.printf("[SENSOR] LDR state changed: %s\n", 
                      ldrStableState ? "CERAH" : "GELAP");
      }
    }
  }
  ldrStatus = ldrStableState ? "Cerah" : "Gelap";
}

// ================= SERVO LOGIC WITH SMOOTH MOVEMENT =================
void updateServoLogic() {
  if (autoMode) {
    int newTarget = 0;
    bool timeValid = false;
    int h = -1;
    
    // Cek waktu
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      h = timeinfo.tm_hour;
      timeValid = true;
    }
    
    bool isOpTime = (timeValid && h >= openHour && h < closeHour);
    
    // Decision tree
    if (!timeInitialized) {
      systemStatus = "Waiting for NTP Sync...";
      newTarget = 0;
    } else if (!isOpTime) {
      systemStatus = "Closed - Outside Operating Hours";
      newTarget = 0;
    } else if (rainStableState) {
      systemStatus = "Closed - Rain Detected";
      newTarget = 0;
    } else if (!ldrStableState) {
      systemStatus = "Closed - Low Light";
      newTarget = 0;
    } else if (!weatherDataValid) {
      systemStatus = "Closed - Weather Data Invalid";
      newTarget = 0;
    } else if (!weatherIsGood) {
      systemStatus = "Closed - Bad Weather Forecast";
      newTarget = 0;
    } else {
      systemStatus = "Open - Safe Conditions";
      newTarget = 180;
    }
    
    targetServoAngle = newTarget;
  } else {
    // Mode manual - targetServoAngle diatur oleh handleServo(), jangan override di sini
    systemStatus = "Mode Manual (" + String(targetServoAngle) + ")";
  }
}

// ================= SMOOTH SERVO MOVEMENT =================
void updateServoPosition() {
  if (millis() - lastServoUpdate < SERVO_UPDATE_INTERVAL) return;
  lastServoUpdate = millis();
  
  if (servoAngle != targetServoAngle) {
    // Smooth movement: 2 derajat per step
    if (servoAngle < targetServoAngle) {
      servoAngle += 2;
      if (servoAngle > targetServoAngle) servoAngle = targetServoAngle;
    } else {
      servoAngle -= 2;
      if (servoAngle < targetServoAngle) servoAngle = targetServoAngle;
    }
    
    myServo.write(servoAngle);
    
    if (servoAngle == targetServoAngle) {
      Serial.printf("[SERVO] Reached target: %d°\n", servoAngle);
    }
  }
}

// ================= WEATHER UPDATE MANAGER =================
void manageWeatherUpdates() {
  unsigned long now = millis();
  
  // Jika data valid, update setiap WEATHER_UPDATE_INTERVAL
  if (weatherDataValid) {
    if (now - lastWeatherUpdate > WEATHER_UPDATE_INTERVAL) {
      lastWeatherUpdate = now;
      lastWeatherAttempt = now;
      updateWeatherNow();
      updateForecast();
    }
  } 
  // Jika data invalid, retry lebih cepat
  else {
    if (now - lastWeatherAttempt > WEATHER_RETRY_INTERVAL) {
      lastWeatherAttempt = now;
      Serial.printf("[WEATHER] Retry attempt %d\n", weatherRetryCount + 1);
      updateWeatherNow();
      if (weatherDataValid) {
        updateForecast();
        lastWeatherUpdate = now;
      }
    }
  }
}

// ================= WiFi RECONNECT (Non-blocking) =================
void checkWiFiConnection() {
  static unsigned long lastReconnectAttempt = 0;
  static int reconnectAttempts = 0;
  
  if (isAPMode) return;
  
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastReconnectAttempt > 10000) { // Coba tiap 10 detik
      lastReconnectAttempt = millis();
      reconnectAttempts++;
      
      Serial.printf("[WiFi] Reconnecting... (attempt %d)\n", reconnectAttempts);
      WiFi.disconnect();
      WiFi.reconnect();
      
      // Reset jika terlalu banyak gagal
      if (reconnectAttempts > 5) {
        Serial.println("[WiFi] Too many failures. Restarting...");
        delay(1000);
        ESP.restart();
      }
    }
  } else {
    reconnectAttempts = 0; // Reset counter jika connect
  }
}

// ================= MQTT LOOP =================
void handleMQTT() {
  if (isAPMode || WiFi.status() != WL_CONNECTED) return;
  
  if (!mqttClient.connected()) {
    mqttReconnect();
  }
  
  mqttClient.loop();
  publishMqttData();
}

// ================= HANDLERS =================
void handleRoot() {
  server.send(200, "text/html", R"rawliteral(
<!DOCTYPE html><html><head>
<meta name=viewport content="width=device-width,initial-scale=1">
<meta charset="UTF-8">
<title>Smart Clothesline Controller</title>
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
<style>
:root{
  --sky-light:#87CEEB;
  --sky-dark:#4A90D9;
  --sun-yellow:#FFD93D;
  --sun-orange:#FF9500;
  --cloud-gray:#B0C4DE;
  --rain-blue:#5D9CEC;
  --dark:#1a1a2e;
  --card-bg:rgba(255,255,255,0.95);
  --text-primary:#2c3e50;
  --text-secondary:#64748b;
  --success:#10b981;
  --danger:#ef4444;
  --warning:#f59e0b;
}
*{margin:0;padding:0;box-sizing:border-box}
body{
  font-family:'Inter',system-ui,-apple-system,sans-serif;
  background:linear-gradient(180deg,var(--sky-light) 0%,var(--sky-dark) 50%,#2c5282 100%);
  min-height:100vh;
  padding:20px;
  background-attachment:fixed;
}
.container{max-width:480px;margin:0 auto}
.header{
  text-align:center;
  padding:30px 20px;
  margin-bottom:20px;
}
.header h1{
  color:#fff;
  font-size:24px;
  font-weight:700;
  letter-spacing:-0.5px;
  text-shadow:0 2px 10px rgba(0,0,0,0.2);
  margin-bottom:8px;
}
.header p{
  color:rgba(255,255,255,0.85);
  font-size:13px;
  font-weight:500;
}
.card{
  background:var(--card-bg);
  padding:24px;
  margin-bottom:16px;
  border-radius:20px;
  box-shadow:0 10px 40px rgba(0,0,0,0.12);
  backdrop-filter:blur(10px);
  transition:transform 0.2s;
}
.card:hover{transform:translateY(-2px)}
.card-title{
  font-size:13px;
  font-weight:600;
  color:var(--text-secondary);
  text-transform:uppercase;
  letter-spacing:0.5px;
  margin-bottom:16px;
  padding-bottom:12px;
  border-bottom:1px solid #e2e8f0;
}
.weather-display{
  display:flex;
  align-items:center;
  justify-content:space-between;
  padding:10px 0;
}
.weather-info{flex:1}
.weather-info .time{
  font-size:42px;
  font-weight:700;
  color:var(--text-primary);
  line-height:1;
  margin-bottom:8px;
}
.weather-info .condition{
  font-size:15px;
  color:var(--text-secondary);
  font-weight:500;
  text-transform:capitalize;
}
.weather-icon-wrapper{
  width:90px;
  height:90px;
  display:flex;
  align-items:center;
  justify-content:center;
}
.weather-icon-wrapper img{
  width:100%;
  height:100%;
  object-fit:contain;
  filter:drop-shadow(0 4px 8px rgba(0,0,0,0.1));
}
.status-banner{
  padding:16px 20px;
  border-radius:14px;
  margin-top:16px;
  font-size:14px;
  font-weight:600;
  display:flex;
  align-items:center;
  gap:10px;
}
.status-open{background:linear-gradient(135deg,#d1fae5,#a7f3d0);color:#065f46}
.status-closed{background:linear-gradient(135deg,#fee2e2,#fecaca);color:#991b1b}
.status-waiting{background:linear-gradient(135deg,#e0e7ff,#c7d2fe);color:#3730a3}
.sensor-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.sensor-item{
  padding:20px;
  background:linear-gradient(135deg,#f8fafc,#f1f5f9);
  border-radius:14px;
  text-align:center;
}
.sensor-label{
  font-size:11px;
  font-weight:600;
  color:var(--text-secondary);
  text-transform:uppercase;
  letter-spacing:0.5px;
  margin-bottom:8px;
}
.sensor-status{
  font-size:18px;
  font-weight:700;
  margin-bottom:4px;
}
.sensor-value{
  font-size:11px;
  color:var(--text-secondary);
  font-weight:500;
}
.control-buttons{
  display:flex;
  gap:10px;
  justify-content:center;
}
.btn{
  flex:1;
  padding:14px 24px;
  border:none;
  border-radius:12px;
  font-family:inherit;
  font-size:14px;
  font-weight:600;
  cursor:pointer;
  transition:all 0.2s;
}
.btn:active{transform:scale(0.98)}
.btn-auto{background:#e2e8f0;color:#64748b}
.btn-auto.active{background:linear-gradient(135deg,var(--sky-dark),#3182ce);color:#fff;box-shadow:0 4px 14px rgba(74,144,217,0.4)}
.btn-manual{background:#e2e8f0;color:#64748b}
.btn-manual.active{background:linear-gradient(135deg,var(--sun-orange),#ea580c);color:#fff;box-shadow:0 4px 14px rgba(255,149,0,0.4)}
.manual-controls{
  display:none;
  flex-direction:row;
  gap:10px;
  margin-top:16px;
}
.manual-controls.show{display:flex!important}
.btn-open{background:linear-gradient(135deg,#10b981,#059669);color:#fff}
.btn-close{background:linear-gradient(135deg,#ef4444,#dc2626);color:#fff}
.forecast-grid{
  display:grid;
  grid-template-columns:repeat(4,1fr);
  gap:8px;
}
.forecast-item{
  background:linear-gradient(135deg,#f8fafc,#f1f5f9);
  padding:14px 8px;
  border-radius:12px;
  text-align:center;
}
.forecast-time{
  font-size:12px;
  font-weight:600;
  color:var(--text-primary);
  margin-bottom:8px;
}
.forecast-item img{width:40px;height:40px}
.forecast-empty{
  grid-column:1/-1;
  text-align:center;
  padding:30px;
  color:var(--text-secondary);
  font-size:13px;
}
.op-hours{
  text-align:center;
  font-size:12px;
  color:var(--text-secondary);
  margin-bottom:14px;
}
.op-hours span{font-weight:600;color:var(--text-primary)}
.settings-grid{display:grid;gap:14px}
.setting-row{
  display:flex;
  align-items:center;
  gap:12px;
}
.setting-row label{
  flex:1;
  font-size:13px;
  font-weight:500;
  color:var(--text-primary);
}
.setting-row input{
  width:100px;
  padding:10px 14px;
  border:2px solid #e2e8f0;
  border-radius:10px;
  font-family:inherit;
  font-size:14px;
  font-weight:500;
  text-align:center;
  transition:border-color 0.2s;
}
.setting-row input:focus{outline:none;border-color:var(--sky-dark)}
.btn-save{
  width:100%;
  margin-top:16px;
  background:linear-gradient(135deg,var(--sky-dark),#3182ce);
  color:#fff;
  padding:14px;
  border:none;
  border-radius:12px;
  font-family:inherit;
  font-size:14px;
  font-weight:600;
  cursor:pointer;
  transition:all 0.2s;
}
.btn-save:hover{box-shadow:0 4px 14px rgba(74,144,217,0.4)}
.footer{
  text-align:center;
  padding:20px;
  color:rgba(255,255,255,0.7);
  font-size:11px;
}
.mqtt-status{
  text-align:center;
  padding:8px;
  background:rgba(255,255,255,0.1);
  border-radius:8px;
  margin-top:10px;
  font-size:11px;
  color:rgba(255,255,255,0.8);
}
</style>
</head>
<body>
<div class=container>

<div class=header>
<h1>Smart Clothesline</h1>
<p>Automatic Weather-Based Controller</p>
<div class=mqtt-status>MQTT: broker.hivemq.com | Node-RED Ready</div>
</div>

<div class=card>
<div class=weather-display>
<div class=weather-info>
<div class=time id=time>--:--</div>
<div class=condition id=weather>Loading...</div>
</div>
<div class=weather-icon-wrapper><img id=iconImg src="" alt="weather"></div>
</div>
<div class="status-banner status-waiting" id=statusBanner>
<span id=sys_status>Initializing system...</span>
</div>
</div>

<div class=card>
<div class=card-title>Sensor Status</div>
<div class=sensor-grid>
<div class=sensor-item>
<div class=sensor-label>Rain Sensor</div>
<div class=sensor-status id=s_rain>--</div>
<div class=sensor-value id=v_rain>ADC: --</div>
</div>
<div class=sensor-item>
<div class=sensor-label>Light Sensor</div>
<div class=sensor-status id=s_ldr>--</div>
<div class=sensor-value id=v_ldr>ADC: --</div>
</div>
</div>
</div>

<div class=card>
<div class=card-title>Control Mode</div>
<div class=control-buttons>
<button class="btn btn-auto" id=btnA onclick="setMode('auto')">AUTO</button>
<button class="btn btn-manual" id=btnM onclick="setMode('manual')">MANUAL</button>
</div>
<div class=manual-controls id=manual>
<button class="btn btn-open" onclick="setServo(180)">OPEN</button>
<button class="btn btn-close" onclick="setServo(0)">CLOSE</button>
</div>
</div>

<div class=card>
<div class=card-title>Weather Forecast</div>
<div class=op-hours>Operating Hours: <span id=opHours>--:-- - --:--</span></div>
<div class=forecast-grid id=fc></div>
</div>

<div class=card>
<div class=card-title>Time Settings</div>
<div class=settings-grid>
<div class=setting-row>
<label>Open Hour</label>
<input type=number id=o min=0 max=23>
</div>
<div class=setting-row>
<label>Close Hour</label>
<input type=number id=c min=0 max=23>
</div>
</div>
<button class=btn-save onclick=setTime()>Save Time</button>
</div>

<div class=card>
<div class=card-title>Sensor Threshold</div>
<div class=settings-grid>
<div class=setting-row>
<label>Rain Detection (ADC)</label>
<input type=number id=i_rain>
</div>
<div class=setting-row>
<label>Dark Light (ADC)</label>
<input type=number id=i_ldr>
</div>
</div>
<button class=btn-save onclick=saveSettings()>Save Threshold</button>
</div>

<div class=footer id=lastUpdate>Last update: --:--</div>

</div>

<script>
const icon=u=>`https://openweathermap.org/img/wn/${u}@2x.png`;
function setMode(m){fetch('/mode?set='+m).then(()=>setTimeout(()=>update(false),300));}
function setServo(p){fetch('/servo?pos='+p).then(()=>setTimeout(()=>update(false),300));}
function setTime(){
  const oVal=parseInt(o.value),cVal=parseInt(c.value);
  if(isNaN(oVal)||isNaN(cVal)||oVal<0||oVal>23||cVal<0||cVal>23){alert('Invalid time!');return;}
  fetch(`/settime?open=${oVal}&close=${cVal}`).then(()=>setTimeout(()=>update(false),1000));
}
function saveSettings(){
  const r=parseInt(i_rain.value),l=parseInt(i_ldr.value);
  if(isNaN(r)||isNaN(l)){alert('Invalid value!');return;}
  fetch(`/settings?rain=${r}&ldr=${l}`).then(()=>setTimeout(()=>update(false),500));
}
function update(skipInputs){
  fetch('/data').then(r=>r.json()).then(d=>{
    time.innerText=d.time;
    weather.innerText=d.weather;
    sys_status.innerText=d.status_text;
    iconImg.src=icon(d.weather_icon);
    s_rain.innerText=d.rain_status;
    s_ldr.innerText=d.ldr_status;
    v_rain.innerText='ADC: '+d.rain_val;
    v_ldr.innerText='ADC: '+d.ldr_val;
    s_rain.style.color=d.rain_status.includes('Tidak')?'#10b981':'#ef4444';
    s_ldr.style.color=d.ldr_status=='Cerah'?'#f59e0b':'#64748b';
    const banner=document.getElementById('statusBanner');
    banner.className='status-banner '+(d.status_text.includes('Open')?'status-open':d.status_text.includes('Closed')?'status-closed':'status-waiting');
    if(!skipInputs){
      if(document.activeElement!==o&&document.activeElement!==c){o.value=d.open;c.value=d.close;}
      if(document.activeElement!==i_rain&&document.activeElement!==i_ldr){i_rain.value=d.rain_thresh;i_ldr.value=d.ldr_thresh;}
    }
    opHours.innerText=String(d.open).padStart(2,'0')+':00 - '+String(d.close).padStart(2,'0')+':00';
    btnA.className='btn btn-auto'+(d.mode=='AUTO'?' active':'');
    btnM.className='btn btn-manual'+(d.mode=='MANUAL'?' active':'');
    manual.className='manual-controls'+(d.mode=='MANUAL'?' show':'');
    fc.innerHTML='';
    let count=0;
    d.forecast.forEach(f=>{
      if(f.hour!=='--:--'&&f.valid){
        count++;
        fc.innerHTML+='<div class=forecast-item><div class=forecast-time>'+f.hour+'</div><img src="'+icon(f.icon)+'" alt=""></div>';
      }
    });
    if(count==0)fc.innerHTML='<div class=forecast-empty>No forecast data available</div>';
    lastUpdate.innerText='Last update: '+d.time;
  }).catch(()=>lastUpdate.innerText='Connection error');
}
setInterval(()=>update(true),2000);
update(false);
</script>
</body></html>
)rawliteral");
}

void handleData() {
  bool timeValid = false;
  int hourNow = -1;
  String timeStr = getTimeString(hourNow, timeValid);
  
  StaticJsonDocument<1024> doc;
  doc["time"] = timeStr;
  doc["mode"] = autoMode ? "AUTO" : "MANUAL";
  doc["servo"] = String(servoAngle) + "°";
  doc["weather"] = weatherNow;
  doc["weather_icon"] = weatherIcon;
  doc["rain_status"] = rainStatus;
  doc["ldr_status"] = ldrStatus;
  doc["rain_val"] = rainVal;
  doc["ldr_val"] = ldrVal;
  doc["rain_thresh"] = thresholdRain;
  doc["ldr_thresh"] = thresholdLdr;
  doc["status_text"] = systemStatus;
  doc["open"] = openHour;
  doc["close"] = closeHour;
  
  JsonArray fcArray = doc.createNestedArray("forecast");
  for (int i = 0; i < 4; i++) {
    JsonObject fcObj = fcArray.createNestedObject();
    fcObj["hour"] = forecast[i].hour;
    fcObj["icon"] = forecast[i].icon;
    fcObj["valid"] = forecast[i].valid;
  }
  
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

void handleMode() {
  if (server.hasArg("set")) {
    bool newMode = (server.arg("set") == "auto");
    if (newMode != autoMode) {
      autoMode = newMode;
      Serial.printf("[MODE] Changed to: %s\n", autoMode ? "AUTO" : "MANUAL");
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleServo() {
  if (!autoMode && server.hasArg("pos")) {
    int newPos = constrain(server.arg("pos").toInt(), 0, 180);
    targetServoAngle = newPos;
    Serial.printf("[MANUAL] Servo target: %d°\n", newPos);
  }
  server.send(200, "text/plain", "OK");
}

void handleSetTime() {
  bool changed = false;
  if (server.hasArg("open")) {
    int newOpen = constrain(server.arg("open").toInt(), 0, 23);
    if (newOpen != openHour) {
      openHour = newOpen;
      changed = true;
    }
  }
  if (server.hasArg("close")) {
    int newClose = constrain(server.arg("close").toInt(), 0, 23);
    if (newClose != closeHour) {
      closeHour = newClose;
      changed = true;
    }
  }
  if (changed && !isAPMode) {
    Serial.printf("[TIME] Updated: %d:00 - %d:00\n", openHour, closeHour);
    // Force forecast update
    lastWeatherUpdate = 0;
  }
  server.send(200, "text/plain", "OK");
}

void handleSettings() {
  if (server.hasArg("rain")) {
    thresholdRain = server.arg("rain").toInt();
    Serial.printf("[SETTINGS] Rain threshold: %d\n", thresholdRain);
  }
  if (server.hasArg("ldr")) {
    thresholdLdr = server.arg("ldr").toInt();
    Serial.printf("[SETTINGS] LDR threshold: %d\n", thresholdLdr);
  }
  server.send(200, "text/plain", "OK");
}

// ================= AP MODE HANDLERS =================
void setupAPMode() {
  isAPMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("JEMURAN-CONFIG");

  Serial.println("\n╔════════════════════════════════╗");
  Serial.println("║      AP MODE ACTIVATED         ║");
  Serial.println("╚════════════════════════════════╝");
  Serial.println("📡 SSID: JEMURAN-CONFIG");
  Serial.print("🌐 IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.println("👉 Connect dan buka browser");

  server.on("/", handleConfig);
  server.on("/save", handleWifiSave);
  server.begin();
}

void handleConfig() {
  String html = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name=viewport content="width=device-width,initial-scale=1">
<meta charset="UTF-8">
<title>WiFi Setup</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:Arial;background:linear-gradient(135deg,#667eea,#764ba2);min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}
.card{background:#fff;padding:40px;border-radius:20px;box-shadow:0 20px 60px rgba(0,0,0,0.3);max-width:400px;width:100%}
h1{color:#333;text-align:center;margin-bottom:10px;font-size:28px}
.subtitle{text-align:center;color:#666;margin-bottom:30px;font-size:14px}
label{display:block;color:#555;font-weight:600;margin:15px 0 5px}
input{width:100%;padding:12px;border:2px solid #ddd;border-radius:10px;font-size:15px;transition:border 0.3s}
input:focus{outline:none;border-color:#667eea}
button{width:100%;background:linear-gradient(135deg,#667eea,#764ba2);color:#fff;padding:15px;border:none;border-radius:10px;font-size:16px;font-weight:600;cursor:pointer;margin-top:20px;transition:transform 0.2s}
button:hover{transform:translateY(-2px)}
button:active{transform:translateY(0)}
.icon{font-size:48px;text-align:center;margin-bottom:20px}
</style></head>
<body>
<div class=card>
<div class=icon>📡</div>
<h1>Setup WiFi</h1>
<div class=subtitle>Jemuran Pintar Configuration</div>
<form action="/save" method="POST">
  <label>📶 SSID (Nama WiFi)</label>
  <input type="text" name="ssid" placeholder="Nama WiFi Anda" required autofocus>
  <label>🔒 Password</label>
  <input type="password" name="pass" placeholder="Password WiFi">
  <button type="submit">💾 Simpan & Restart</button>
</form>
</div>
</body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleWifiSave() {
  String newSsid = server.arg("ssid");
  String newPass = server.arg("pass");
  
  if (newSsid.length() > 0) {
    preferences.begin("wifi-config", false);
    preferences.putString("ssid", newSsid);
    preferences.putString("pass", newPass);
    preferences.end();
    
    Serial.println("\n✅ WiFi config saved!");
    Serial.printf("   SSID: %s\n", newSsid.c_str());
    
    String html = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name=viewport content="width=device-width,initial-scale=1">
<meta charset="UTF-8">
<title>Saved!</title>
<style>
*{margin:0;padding:0}
body{font-family:Arial;background:linear-gradient(135deg,#2ecc71,#27ae60);min-height:100vh;display:flex;align-items:center;justify-content:center;color:#fff;text-align:center;padding:20px}
.card{background:rgba(255,255,255,0.95);color:#333;padding:40px;border-radius:20px;box-shadow:0 20px 60px rgba(0,0,0,0.3);max-width:500px}
h1{font-size:32px;margin:20px 0}
.icon{font-size:80px;animation:bounce 1s ease}
@keyframes bounce{0%,100%{transform:translateY(0)}50%{transform:translateY(-20px)}}
p{font-size:16px;line-height:1.6;margin:15px 0}
.url{background:#f8f9fa;padding:15px;border-radius:10px;margin:20px 0;font-family:monospace;font-size:18px;font-weight:600;color:#667eea}
</style></head>
<body>
<div class=card>
<div class=icon>✅</div>
<h1>Berhasil Disimpan!</h1>
<p>ESP32 akan restart dan connect ke WiFi:<br><b>)rawliteral" + newSsid + R"rawliteral(</b></p>
<p>Tunggu 20 detik, lalu buka:</p>
<div class=url>http://jemuran.local</div>
<p style="font-size:14px;color:#999">Atau cek IP di Serial Monitor</p>
</div>
</body></html>
)rawliteral";
    
    server.send(200, "text/html", html);
    delay(2000);
    ESP.restart();
  } else {
    server.send(400, "text/html", "<h1>Error!</h1><p>SSID tidak boleh kosong.</p><a href='/'>Kembali</a>");
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1500);
  
  Serial.println("\n\n");
  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║   ESP32 JEMURAN PINTAR v2.0           ║");
  Serial.println("║   Smart Clothesline Controller        ║");
  Serial.println("║   + MQTT Integration for Node-RED     ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.println();
  
  // Enable Watchdog (ESP-IDF v5.x API)
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT * 1000,  // Convert seconds to ms
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,  // Watch all cores
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
  Serial.println("[BOOT] ✓ Watchdog enabled (30s)");
  
  // Init LED
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
  Serial.println("[BOOT] ✓ Status LED initialized");
  
  // Init Sensors
  pinMode(RAIN_SENSOR_PIN, INPUT);
  pinMode(LDR_SENSOR_PIN, INPUT);
  Serial.println("[BOOT] ✓ Sensors initialized");
  
  // Clear saved WiFi config and use hardcoded credentials
  // HAPUS BLOK INI setelah pertama kali upload jika ingin pakai AP mode config
  preferences.begin("wifi-config", false);
  preferences.clear();
  preferences.end();
  Serial.println("[BOOT] ✓ Preferences cleared, using hardcoded WiFi");
  
  bool connected = false;
  
  if (ssid == "") {
    Serial.println("\n[SETUP] ⚠️  No WiFi config found");
    setupAPMode();
  } else {
    Serial.printf("[SETUP] 📡 Connecting to: %s\n", ssid.c_str());
    
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    int attempts = 0;
    Serial.print("[WIFI] ");
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
      delay(500);
      Serial.print(".");
      attempts++;
      esp_task_wdt_reset(); // Feed watchdog
    }
    Serial.println();
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n✅ WiFi Connected!");
      Serial.printf("   SSID: %s\n", WiFi.SSID().c_str());
      Serial.printf("   IP: %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("   RSSI: %d dBm\n", WiFi.RSSI());
      connected = true;
    } else {
      Serial.println("\n❌ WiFi Failed!");
      Serial.println("[SETUP] Switching to AP Mode...");
      setupAPMode();
    }
  }
  
  // Init Servo
  Serial.println("\n[SERVO] Initializing...");
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN, 500, 2400);
  
  // Smooth start position
  for (int i = 0; i <= servoAngle; i += 5) {
    myServo.write(i);
    delay(50);
    esp_task_wdt_reset();
  }
  Serial.println("[SERVO] ✓ Ready at 0°");
  
  if (!isAPMode) {
    // NTP Sync
    Serial.println("\n[NTP] Syncing time...");
    configTime(gmtOffset_sec, 0, ntpServer);
    
    struct tm timeinfo;
    int retries = 0;
    while (!getLocalTime(&timeinfo) && retries < 10) {
      Serial.print(".");
      delay(1000);
      retries++;
      esp_task_wdt_reset();
    }
    
    if (getLocalTime(&timeinfo)) {
      Serial.println("\n[NTP] ✓ Time synchronized");
      char buf[30];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
      Serial.printf("   Current time: %s\n", buf);
      timeInitialized = true;
    } else {
      Serial.println("\n[NTP] ⚠️  Time sync failed (will retry)");
    }
    
    // Setup Web Server
    server.on("/", handleRoot);
    server.on("/data", handleData);
    server.on("/mode", handleMode);
    server.on("/servo", handleServo);
    server.on("/settime", handleSetTime);
    server.on("/settings", handleSettings);
    server.on("/resetwifi", []() {
      preferences.begin("wifi-config", false);
      preferences.clear();
      preferences.end();
      server.send(200, "text/plain", "WiFi reset! Restarting...");
      delay(1000);
      ESP.restart();
    });
    
    server.begin();
    Serial.println("\n[SERVER] ✓ Web server started");
    
    // mDNS
    if (MDNS.begin("jemuran")) {
      Serial.println("[MDNS] ✓ http://jemuran.local");
    }
    
    // Setup MQTT
    Serial.println("\n[MQTT] Configuring...");
    mqttClient.setServer(mqtt_server, mqtt_port);
    mqttClient.setCallback(mqttCallback);
    Serial.printf("[MQTT] ✓ Broker: %s:%d\n", mqtt_server, mqtt_port);
    Serial.printf("[MQTT] ✓ Publish topic: %s\n", mqtt_topic_data);
    Serial.printf("[MQTT] ✓ Subscribe topic: %s\n", mqtt_topic_command);
    
    // Initial Weather Fetch
    Serial.println("\n[WEATHER] Fetching initial data...");
    updateWeatherNow();
    delay(1000);
    esp_task_wdt_reset();
    updateForecast();
    lastWeatherUpdate = millis();
    lastWeatherAttempt = millis();
  }
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║         SYSTEM READY! 🚀               ║");
  Serial.println("╚════════════════════════════════════════╝\n");
}

// ================= MAIN LOOP =================
void loop() {
  // Reset watchdog
  esp_task_wdt_reset();
  
  // Handle web requests
  server.handleClient();
  
  if (!isAPMode) {
    // Check WiFi connection (non-blocking)
    checkWiFiConnection();
    
    // Handle MQTT
    handleMQTT();
    
    // Manage weather updates
    manageWeatherUpdates();
  }
  
  // Update sensors (with debounce)
  updateSensors();
  
  // Update servo logic
  updateServoLogic();
  
  // Update servo position (smooth movement)
  updateServoPosition();
  
  // Update status LED
  updateStatusLED();
  
  // Small delay for stability
  delay(10);
}