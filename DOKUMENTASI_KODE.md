# 📚 Dokumentasi Kode ESP32 Jemuran Pintar v2.0

## Daftar Isi
1. [Overview Project](#overview-project)
2. [Library yang Digunakan](#library-yang-digunakan)
3. [Konfigurasi Pin & Variabel](#konfigurasi-pin--variabel)
4. [MQTT Integration](#mqtt-integration)
5. [Sistem Sensor & Debounce](#sistem-sensor--debounce)
6. [Logika Kontrol Servo](#logika-kontrol-servo)
7. [Weather API Integration](#weather-api-integration)
8. [Web Server & Interface](#web-server--interface)
9. [Flow Program Utama](#flow-program-utama)
10. [Node-RED Integration](#node-red-integration)

---

## Overview Project

### Apa itu Jemuran Pintar?
Jemuran Pintar adalah sistem otomatis untuk membuka/menutup jemuran berdasarkan:
- **Sensor Hujan** - Mendeteksi hujan
- **Sensor Cahaya (LDR)** - Mendeteksi kondisi terang/gelap
- **Data Cuaca Online** - Dari OpenWeatherMap API
- **Jam Operasi** - Hanya beroperasi di jam yang ditentukan

### Arsitektur Sistem
```
┌─────────────────────────────────────────────────────────────────┐
│                         ESP32                                   │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐       │
│  │  Rain    │  │   LDR    │  │  Servo   │  │  WiFi    │       │
│  │  Sensor  │  │  Sensor  │  │  Motor   │  │  Module  │       │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘       │
│       │             │             │             │              │
│       └─────────────┴─────────────┴─────────────┘              │
│                         │                                       │
│              ┌──────────┴──────────┐                           │
│              │    Main Program     │                           │
│              └──────────┬──────────┘                           │
└─────────────────────────┼───────────────────────────────────────┘
                          │
        ┌─────────────────┼─────────────────┐
        │                 │                 │
        ▼                 ▼                 ▼
   ┌─────────┐      ┌─────────┐      ┌─────────┐
   │  MQTT   │      │  Web    │      │ Weather │
   │ Broker  │      │ Server  │      │   API   │
   └─────────┘      └─────────┘      └─────────┘
        │                 │
        ▼                 ▼
   ┌─────────┐      ┌─────────┐
   │Node-RED │      │ Browser │
   │Dashboard│      │   UI    │
   └─────────┘      └─────────┘
```

---

## Library yang Digunakan

```cpp
#include <WiFi.h>           // Koneksi WiFi ESP32
#include <ESPmDNS.h>        // mDNS untuk akses via nama domain lokal
#include <WebServer.h>      // Web server untuk UI
#include <ESP32Servo.h>     // Kontrol servo motor
#include <HTTPClient.h>     // HTTP request ke Weather API
#include <ArduinoJson.h>    // Parse JSON dari API
#include <time.h>           // Sinkronisasi waktu NTP
#include <Preferences.h>    // Menyimpan config ke flash
#include <esp_task_wdt.h>   // Watchdog timer (anti-hang)
#include <PubSubClient.h>   // MQTT client untuk Node-RED
```

### Penjelasan Setiap Library:

| Library | Fungsi |
|---------|--------|
| `WiFi.h` | Menghubungkan ESP32 ke jaringan WiFi |
| `ESPmDNS.h` | Membuat `http://jemuran.local` bisa diakses |
| `WebServer.h` | Menyajikan halaman web untuk kontrol |
| `ESP32Servo.h` | Menggerakkan servo motor 0°-180° |
| `HTTPClient.h` | Mengambil data cuaca dari internet |
| `ArduinoJson.h` | Membaca response JSON dari API |
| `time.h` | Mendapatkan waktu dari server NTP |
| `Preferences.h` | Menyimpan SSID/password di memori flash |
| `esp_task_wdt.h` | Mencegah ESP32 hang (auto-restart) |
| `PubSubClient.h` | Komunikasi MQTT dengan Node-RED |

---

## Konfigurasi Pin & Variabel

### Pin Definition
```cpp
#define SERVO_PIN 23          // GPIO23 untuk servo
#define RAIN_SENSOR_PIN 39    // GPIO39 (VP) untuk sensor hujan
#define LDR_SENSOR_PIN 36     // GPIO36 (VN) untuk sensor cahaya
#define STATUS_LED_PIN 2      // GPIO2 LED built-in
```

### Penjelasan:
- **GPIO39 & GPIO36** adalah pin ADC (Analog to Digital Converter) khusus di ESP32 yang hanya bisa INPUT
- **GPIO23** dipilih karena kompatibel dengan PWM untuk servo
- **GPIO2** adalah LED bawaan ESP32 untuk indikator status

### Threshold Variables
```cpp
int thresholdRain = 3000;  // Nilai ADC untuk deteksi hujan
int thresholdLdr = 1200;   // Nilai ADC untuk deteksi gelap
```

### Bagaimana Threshold Bekerja:

```
RAIN SENSOR (Menggunakan logika terbalik):
┌───────────────────────────────────────────┐
│  ADC Value                                │
│  0 ◄────────────────────────────────► 4095│
│  │                    ▲                   │
│  │                    │ threshold = 3000  │
│  │         HUJAN      │      KERING       │
│  │      (< 3000)      │    (>= 3000)      │
└───────────────────────────────────────────┘

LDR SENSOR:
┌───────────────────────────────────────────┐
│  ADC Value                                │
│  0 ◄────────────────────────────────► 4095│
│  │         ▲                              │
│  │         │ threshold = 1200             │
│  │  CERAH  │           GELAP              │
│  │ (< 1200)│         (>= 1200)            │
└───────────────────────────────────────────┘
```

---

## MQTT Integration

### Konfigurasi MQTT
```cpp
const char* mqtt_server = "broker.hivemq.com";  // Broker publik gratis
const int mqtt_port = 1883;                     // Port standar MQTT
const char* mqtt_topic_data = "jemuran/data";   // Topic untuk kirim data
const char* mqtt_topic_command = "jemuran/command"; // Topic untuk terima perintah
```

### Bagaimana MQTT Bekerja:

```
┌─────────────────────────────────────────────────────────────┐
│                     MQTT BROKER                             │
│                  (broker.hivemq.com)                        │
│                                                             │
│   Topic: jemuran/data          Topic: jemuran/command       │
│   ┌─────────────────┐          ┌─────────────────┐         │
│   │  ESP32 PUBLISH  │          │  NODE-RED PUB   │         │
│   │  sensor data    │          │  OPEN/CLOSE/AUTO│         │
│   └────────┬────────┘          └────────┬────────┘         │
│            │                            │                   │
│            ▼                            ▼                   │
│   ┌─────────────────┐          ┌─────────────────┐         │
│   │ NODE-RED SUB    │          │   ESP32 SUB     │         │
│   │ tampil dashboard│          │ eksekusi command│         │
│   └─────────────────┘          └─────────────────┘         │
└─────────────────────────────────────────────────────────────┘
```

### Fungsi MQTT Callback
```cpp
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  // Parse command dari Node-RED
  if (message == "OPEN") {
    autoMode = false;        // Matikan mode auto
    targetServoAngle = 180;  // Buka jemuran
  } else if (message == "CLOSE") {
    autoMode = false;
    targetServoAngle = 0;    // Tutup jemuran
  } else if (message == "AUTO") {
    autoMode = true;         // Aktifkan mode auto
  }
}
```

### Fungsi Publish Data
```cpp
void publishMqttData() {
  // Buat JSON object
  StaticJsonDocument<512> doc;
  doc["rain_val"] = rainVal;           // Nilai mentah sensor hujan
  doc["ldr_val"] = ldrVal;             // Nilai mentah sensor cahaya
  doc["rain_status"] = rainStatus;     // "Hujan" atau "Tidak Hujan"
  doc["ldr_status"] = ldrStatus;       // "Cerah" atau "Gelap"
  doc["servo_angle"] = servoAngle;     // Posisi servo saat ini
  doc["mode"] = autoMode ? "AUTO" : "MANUAL";
  doc["weather"] = weatherNow;         // Deskripsi cuaca
  doc["status"] = systemStatus;        // Status sistem lengkap
  
  // Kirim ke broker
  String json;
  serializeJson(doc, json);
  mqttClient.publish(mqtt_topic_data, json.c_str());
}
```

---

## Sistem Sensor & Debounce

### Apa itu Debounce?
Sensor analog sering memberikan nilai yang **fluktuatif** (naik-turun cepat). Debounce memastikan bahwa perubahan state hanya terjadi jika nilai **stabil** selama waktu tertentu.

```
TANPA DEBOUNCE:
Time:  1   2   3   4   5   6   7   8   9   10
Value: 2900 3100 2800 3200 2950 3050 2900 3100 2850 3000
State: HUJAN KERING HUJAN KERING HUJAN KERING HUJAN KERING HUJAN KERING
       └─────────────────────────────────────────────────────────────┘
                        ⚠️ State berubah-ubah terus!

DENGAN DEBOUNCE (3 detik):
Time:  1   2   3   4   5   6   7   8   9   10
Value: 2900 3100 2800 3200 2950 3050 2900 3100 2850 3000
                              └──── 3 detik stabil ────┘
State: HUJAN HUJAN HUJAN HUJAN HUJAN HUJAN HUJAN HUJAN HUJAN KERING
       └─────────────────────────────────────────────────────────────┘
                        ✅ State stabil, hanya berubah sekali!
```

### Implementasi Debounce dengan Counter
```cpp
void updateSensors() {
  // Baca nilai mentah
  int rawRain = analogRead(RAIN_SENSOR_PIN);
  
  // Tentukan state berdasarkan threshold
  bool currentRainReading = (rawRain < thresholdRain); // true = hujan
  
  // Cek apakah nilai berubah
  if (currentRainReading != lastRainReading) {
    lastRainChangeTime = millis();  // Reset timer
    rainStableCounter = 0;          // Reset counter
  }
  lastRainReading = currentRainReading;
  
  // Jika sudah lewat DEBOUNCE_DELAY (3 detik)
  if ((millis() - lastRainChangeTime) > DEBOUNCE_DELAY) {
    rainStableCounter++;
    
    // Harus stabil 3 kali berturut-turut
    if (rainStableCounter >= STABLE_COUNT_REQUIRED) {
      rainStableState = currentRainReading;  // Update state resmi
    }
  }
  
  // Update status text
  rainStatus = rainStableState ? "Hujan" : "Tidak Hujan";
}
```

### Diagram Debounce Flow:
```
┌──────────────────────────────────────────────────────────┐
│                    DEBOUNCE FLOW                         │
├──────────────────────────────────────────────────────────┤
│                                                          │
│   analogRead()                                           │
│       │                                                  │
│       ▼                                                  │
│   currentReading = (value < threshold)                   │
│       │                                                  │
│       ▼                                                  │
│   ┌─────────────────────────────────┐                   │
│   │ currentReading != lastReading?  │                   │
│   └─────────────────┬───────────────┘                   │
│                     │                                    │
│         ┌───────────┴───────────┐                       │
│         │ YES                   │ NO                     │
│         ▼                       ▼                        │
│   Reset Timer              Check Timer                   │
│   Reset Counter                 │                        │
│         │                       ▼                        │
│         │              Timer > 3 detik?                  │
│         │                       │                        │
│         │         ┌─────────────┴─────────────┐         │
│         │         │ YES                       │ NO       │
│         │         ▼                           │          │
│         │    Counter++                        │          │
│         │         │                           │          │
│         │         ▼                           │          │
│         │    Counter >= 3?                    │          │
│         │         │                           │          │
│         │    ┌────┴────┐                      │          │
│         │    │YES      │NO                    │          │
│         │    ▼         │                      │          │
│         │ UPDATE       │                      │          │
│         │ STABLE       │                      │          │
│         │ STATE        │                      │          │
│         │              │                      │          │
│         └──────────────┴──────────────────────┘          │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

---

## Logika Kontrol Servo

### Decision Tree
```cpp
void updateServoLogic() {
  if (autoMode) {
    int newTarget = 0;  // Default: tutup
    
    // Cek jam operasi
    bool isOpTime = (hour >= openHour && hour < closeHour);
    
    // Decision tree dengan prioritas
    if (!timeInitialized) {
      systemStatus = "Waiting for NTP Sync...";
      newTarget = 0;
    } 
    else if (!isOpTime) {
      systemStatus = "Closed - Outside Operating Hours";
      newTarget = 0;
    } 
    else if (rainStableState) {  // Hujan = true
      systemStatus = "Closed - Rain Detected";
      newTarget = 0;
    } 
    else if (!ldrStableState) {  // Gelap = ldrStableState false
      systemStatus = "Closed - Low Light";
      newTarget = 0;
    } 
    else if (!weatherDataValid) {
      systemStatus = "Closed - Weather Data Invalid";
      newTarget = 0;
    } 
    else if (!weatherIsGood) {
      systemStatus = "Closed - Bad Weather Forecast";
      newTarget = 0;
    } 
    else {
      // SEMUA kondisi terpenuhi!
      systemStatus = "Open - Safe Conditions";
      newTarget = 180;  // BUKA
    }
    
    targetServoAngle = newTarget;
  }
}
```

### Diagram Decision Tree:
```
┌────────────────────────────────────────────────────────────────┐
│                    SERVO DECISION TREE                         │
├────────────────────────────────────────────────────────────────┤
│                                                                │
│                    ┌─────────────┐                             │
│                    │  AUTO MODE? │                             │
│                    └──────┬──────┘                             │
│                           │                                    │
│            ┌──────────────┴──────────────┐                    │
│            │ YES                         │ NO                  │
│            ▼                             ▼                     │
│     ┌────────────┐              ┌──────────────┐              │
│     │ NTP Sync?  │              │ Manual Mode  │              │
│     └─────┬──────┘              │ (User Control)│              │
│           │                     └──────────────┘              │
│     ┌─────┴─────┐                                             │
│     │NO         │YES                                          │
│     ▼           ▼                                             │
│   TUTUP   ┌──────────────┐                                    │
│           │ Jam Operasi? │                                    │
│           └──────┬───────┘                                    │
│                  │                                            │
│     ┌────────────┴────────────┐                              │
│     │NO                       │YES                            │
│     ▼                         ▼                               │
│   TUTUP              ┌─────────────┐                         │
│                      │ Hujan?      │                         │
│                      └──────┬──────┘                         │
│                             │                                 │
│            ┌────────────────┴────────────────┐               │
│            │YES                              │NO              │
│            ▼                                 ▼                │
│          TUTUP                    ┌─────────────┐            │
│                                   │ Gelap?      │            │
│                                   └──────┬──────┘            │
│                                          │                    │
│                    ┌─────────────────────┴─────────────────┐ │
│                    │YES                                    │NO│
│                    ▼                                       ▼  │
│                  TUTUP                         ┌────────────┐│
│                                                │Weather OK? ││
│                                                └─────┬──────┘│
│                                                      │       │
│                              ┌───────────────────────┴─────┐ │
│                              │NO                           │YES
│                              ▼                             ▼ │
│                            TUTUP                         BUKA│
│                                                              │
└────────────────────────────────────────────────────────────────┘
```

### Smooth Servo Movement
```cpp
void updateServoPosition() {
  if (servoAngle != targetServoAngle) {
    // Gerak 2 derajat per step (smooth, tidak mengagetkan)
    if (servoAngle < targetServoAngle) {
      servoAngle += 2;
      if (servoAngle > targetServoAngle) servoAngle = targetServoAngle;
    } else {
      servoAngle -= 2;
      if (servoAngle < targetServoAngle) servoAngle = targetServoAngle;
    }
    
    myServo.write(servoAngle);  // Eksekusi gerakan
  }
}
```

Kenapa **smooth movement**?
- Servo langsung ke 180° → bisa merusak mekanisme/jemuran
- Gerakan bertahap → lebih aman dan profesional

---

## Weather API Integration

### OpenWeatherMap API
```cpp
void updateWeatherNow() {
  // Buat URL request
  String url = "http://api.openweathermap.org/data/2.5/weather?";
  url += "lat=" + String(LATITUDE);      // Koordinat lokasi
  url += "&lon=" + String(LONGITUDE);
  url += "&units=metric";                // Celsius
  url += "&appid=" + String(API_KEY);
  
  HTTPClient http;
  http.begin(url);
  http.setTimeout(10000);  // 10 detik timeout
  
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    // Parse JSON response
    StaticJsonDocument<2048> doc;
    deserializeJson(doc, http.getString());
    
    // Ambil data cuaca
    weatherNow = doc["weather"][0]["description"].as<String>();
    weatherIcon = doc["weather"][0]["icon"].as<String>();
    
    // Cek apakah cuaca bagus
    int id = doc["weather"][0]["id"].as<int>();
    weatherIsGood = (id == 800 || (id >= 801 && id <= 804));
  }
  
  http.end();
}
```

### Weather ID Codes
```
┌─────────────────────────────────────────────────────────┐
│              OPENWEATHERMAP WEATHER CODES               │
├─────────────────────────────────────────────────────────┤
│  ID Range    │  Condition      │  Jemuran Action        │
├──────────────┼─────────────────┼────────────────────────┤
│  200-299     │  Thunderstorm   │  TUTUP ❌              │
│  300-399     │  Drizzle        │  TUTUP ❌              │
│  500-599     │  Rain           │  TUTUP ❌              │
│  600-699     │  Snow           │  TUTUP ❌              │
│  700-799     │  Atmosphere     │  TUTUP ❌              │
│  800         │  Clear Sky ☀️   │  BUKA ✅               │
│  801-804     │  Clouds ☁️      │  BUKA ✅               │
└─────────────────────────────────────────────────────────┘
```

---

## Web Server & Interface

### Route Handlers
```cpp
// Setup routes di setup()
server.on("/", handleRoot);           // Halaman utama
server.on("/data", handleData);       // JSON data untuk AJAX
server.on("/mode", handleMode);       // Ganti mode AUTO/MANUAL
server.on("/servo", handleServo);     // Kontrol servo manual
server.on("/settime", handleSetTime); // Set jam operasi
server.on("/settings", handleSettings); // Set threshold sensor
server.on("/resetwifi", handleResetWifi); // Reset WiFi config
```

### AJAX Update Flow
```
┌─────────────────────────────────────────────────────────────┐
│                    WEB INTERFACE FLOW                       │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│   Browser                         ESP32                     │
│   ┌──────┐                       ┌──────┐                  │
│   │      │──── GET / ───────────►│      │                  │
│   │      │◄─── HTML Page ────────│      │                  │
│   │      │                       │      │                  │
│   │      │   (setiap 2 detik)    │      │                  │
│   │      │──── GET /data ───────►│      │                  │
│   │      │◄─── JSON Response ────│      │                  │
│   │      │                       │      │                  │
│   │      │   (user klik button)  │      │                  │
│   │      │──── GET /mode?set=... │      │                  │
│   │      │◄─── "OK" ─────────────│      │                  │
│   └──────┘                       └──────┘                  │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### JSON Response dari /data
```json
{
  "time": "12:30",
  "mode": "AUTO",
  "servo": "180°",
  "weather": "clear sky",
  "weather_icon": "01d",
  "rain_status": "Tidak Hujan",
  "ldr_status": "Cerah",
  "rain_val": 3500,
  "ldr_val": 800,
  "rain_thresh": 3000,
  "ldr_thresh": 1200,
  "status_text": "Open - Safe Conditions",
  "open": 8,
  "close": 16,
  "forecast": [
    {"hour": "14:00", "icon": "01d", "valid": true},
    {"hour": "17:00", "icon": "02d", "valid": true}
  ]
}
```

---

## Flow Program Utama

### setup() Function
```
┌────────────────────────────────────────────────────────────────┐
│                       SETUP FLOW                               │
├────────────────────────────────────────────────────────────────┤
│                                                                │
│   1. Serial.begin(115200)                                      │
│      └──► Inisialisasi komunikasi serial untuk debug          │
│                                                                │
│   2. Watchdog Timer                                            │
│      └──► Mencegah ESP32 hang (auto restart setelah 30 detik) │
│                                                                │
│   3. pinMode() untuk LED, Rain, LDR                            │
│      └──► Set mode INPUT/OUTPUT untuk setiap pin              │
│                                                                │
│   4. WiFi Connection                                           │
│      ├──► Coba connect ke WiFi yang di-hardcode               │
│      ├──► Jika gagal → Masuk AP Mode (JEMURAN-CONFIG)         │
│      └──► Jika sukses → Lanjut ke langkah berikutnya          │
│                                                                │
│   5. Servo Initialization                                      │
│      └──► Attach servo ke pin, set posisi awal 0°             │
│                                                                │
│   6. NTP Time Sync                                             │
│      └──► Sinkronisasi waktu dari server internet             │
│                                                                │
│   7. Web Server Setup                                          │
│      └──► Daftarkan semua route handlers                      │
│                                                                │
│   8. MQTT Setup                                                │
│      └──► Set broker, callback, dan subscribe topic           │
│                                                                │
│   9. Initial Weather Fetch                                     │
│      └──► Ambil data cuaca pertama kali                       │
│                                                                │
└────────────────────────────────────────────────────────────────┘
```

### loop() Function
```cpp
void loop() {
  esp_task_wdt_reset();      // 1. Feed watchdog (anti-hang)
  
  server.handleClient();     // 2. Handle HTTP requests
  
  if (!isAPMode) {
    checkWiFiConnection();   // 3. Auto-reconnect WiFi
    handleMQTT();            // 4. MQTT loop + publish
    manageWeatherUpdates();  // 5. Update cuaca periodik
  }
  
  updateSensors();           // 6. Baca sensor + debounce
  updateServoLogic();        // 7. Decision logic
  updateServoPosition();     // 8. Smooth servo movement
  updateStatusLED();         // 9. Update LED indikator
  
  delay(10);                 // 10. Stabilitas
}
```

### Loop Timing Diagram
```
┌────────────────────────────────────────────────────────────────┐
│                    LOOP TIMING (Non-Blocking)                  │
├────────────────────────────────────────────────────────────────┤
│                                                                │
│  Time (ms)  0    100   200   300   400   500   ...   5000     │
│             │     │     │     │     │     │           │        │
│  Watchdog   ✓     ✓     ✓     ✓     ✓     ✓     ...   ✓       │
│  (always)   │     │     │     │     │     │           │        │
│             │     │     │     │     │     │           │        │
│  Web Server ✓     ✓     ✓     ✓     ✓     ✓     ...   ✓       │
│  (always)   │     │     │     │     │     │           │        │
│             │     │     │     │     │     │           │        │
│  Sensors    ✓     ─     ✓     ─     ✓     ─     ...   ✓       │
│  (200ms)    │           │           │                 │        │
│             │           │           │                 │        │
│  Servo      ✓     ✓     ✓     ✓     ✓     ✓     ...   ✓       │
│  (50ms)     │     │     │     │     │     │           │        │
│             │     │     │     │     │     │           │        │
│  LED        ─     ─     ─     ─     ─     ✓     ...   ✓       │
│  (500ms)                                  │           │        │
│             │                             │           │        │
│  MQTT       ─     ─     ─     ─     ─     ─     ...   ✓       │
│  Publish                                              │        │
│  (5000ms)                                             │        │
│                                                                │
└────────────────────────────────────────────────────────────────┘
```

---

## Node-RED Integration

### Data Flow
```
┌─────────────────────────────────────────────────────────────────┐
│                     NODE-RED FLOW                               │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────────┐                                           │
│  │  MQTT In        │                                           │
│  │  jemuran/data   │                                           │
│  └────────┬────────┘                                           │
│           │                                                     │
│           │  JSON: {"rain_val": 2500, "ldr_val": 1000, ...}   │
│           │                                                     │
│           ├──────────────┬──────────────┬──────────────┐       │
│           │              │              │              │       │
│           ▼              ▼              ▼              ▼       │
│    ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐  │
│    │ Change   │   │ Change   │   │ Change   │   │ Change   │  │
│    │ rain_val │   │ ldr_val  │   │ servo    │   │ status   │  │
│    └────┬─────┘   └────┬─────┘   └────┬─────┘   └────┬─────┘  │
│         │              │              │              │         │
│         ▼              ▼              ▼              ▼         │
│    ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐  │
│    │  Gauge   │   │  Gauge   │   │  Gauge   │   │   Text   │  │
│    │   Rain   │   │   LDR    │   │  Servo   │   │  Status  │  │
│    └──────────┘   └──────────┘   └──────────┘   └──────────┘  │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │                    CONTROL BUTTONS                        │  │
│  │                                                           │  │
│  │  ┌────────┐   ┌────────┐   ┌────────┐                    │  │
│  │  │  BUKA  │   │ TUTUP  │   │  AUTO  │                    │  │
│  │  │ "OPEN" │   │"CLOSE" │   │ "AUTO" │                    │  │
│  │  └───┬────┘   └───┬────┘   └───┬────┘                    │  │
│  │      │            │            │                          │  │
│  │      └────────────┴────────────┘                          │  │
│  │                    │                                      │  │
│  │                    ▼                                      │  │
│  │           ┌─────────────────┐                            │  │
│  │           │    MQTT Out     │                            │  │
│  │           │ jemuran/command │                            │  │
│  │           └─────────────────┘                            │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## Ringkasan

### Komponen Utama:
| Komponen | Fungsi |
|----------|--------|
| **ESP32** | Otak sistem, menjalankan semua logika |
| **Sensor Hujan** | Deteksi hujan dengan ADC |
| **Sensor LDR** | Deteksi cahaya dengan ADC |
| **Servo Motor** | Membuka/menutup jemuran |
| **WiFi** | Koneksi internet untuk API & kontrol |
| **Web Server** | Interface web untuk kontrol lokal |
| **MQTT** | Komunikasi dengan Node-RED |
| **OpenWeatherMap** | Data ramalan cuaca |

### Fitur Keamanan:
| Fitur | Deskripsi |
|-------|-----------|
| **Watchdog Timer** | Auto-restart jika hang |
| **WiFi Auto-Reconnect** | Reconnect otomatis jika putus |
| **Debounce** | Mencegah false trigger dari sensor |
| **Smooth Servo** | Gerakan halus, tidak merusak mekanik |
| **AP Mode Fallback** | Bisa config WiFi jika gagal connect |

---

## 📞 Troubleshooting

| Masalah | Solusi |
|---------|--------|
| Servo tidak bergerak | Cek power supply (gunakan 5V terpisah) |
| Sensor nilai aneh | Cek wiring, pastikan GND tersambung |
| WiFi gagal connect | Cek SSID/password, pastikan 2.4GHz |
| MQTT tidak connect | Cek koneksi internet, coba broker lain |
| Web tidak bisa diakses | Cek IP di Serial Monitor |

---

*Dokumentasi ini dibuat untuk memudahkan pemahaman kode ESP32 Jemuran Pintar v2.0*
