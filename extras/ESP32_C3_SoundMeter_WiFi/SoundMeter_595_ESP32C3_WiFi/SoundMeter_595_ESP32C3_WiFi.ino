/*
  ESP32-C3 SuperMini + INMP441 + five-pin 74HC595 four-digit LED
  v2.7 Wi-Fi + gateway health watchdog + SHT31 + Prometheus / illustrative DIY sound level monitor (not calibrated / not IEC 61672).
  Arduino IDE: esp32 by Espressif Systems 3.x, board ESP32C3 Dev Module
  Libraries: ESP_I2S.h, WiFi.h, WebServer.h bundled with Arduino-ESP32 3.x. No TM1637 library.

  INMP441: VDD 3V3, GND GND, SCK GPIO4, WS GPIO5, SD GPIO6, L/R GND
  DISPLAY: VCC 3V3, GND GND, SCLK GPIO7, RCLK GPIO10, DIO GPIO3
  SHT31-D: VIN 3V3, GND GND, SCL GPIO1, SDA GPIO0; AL/AD unused

  IMPORTANT: This display driver assumes TWO chained 74HC595s, one byte
  for segments and one for digit selection. Run Display_595_Probe (v3)
  first: it shows "1234" then the four profile bits, and can also print
  the profile lines plus a raw segment walk over serial.
  Change the profile and SEGMENT_MAP based on what the probe shows.

  NEW: edit WIFI_SSID / WIFI_PASSWORD below; the ESP32 serves /metrics
  on TCP port 80. Keep this endpoint LAN-only; do not forward port 80.
  home_temperature_celsius / home_humidity_percent expose SHT31 values.
  sound_level_db = estimated unweighted dB SPL (matches smart-home bot v1.1);
  sound_level_dba = estimated A-weighted value displayed on the 595 module.
*/
#include <Arduino.h>
#include <ESP_I2S.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <esp_system.h>
#include <ping/ping_sock.h>
#include <lwip/inet.h>

constexpr int PIN_MIC_SCK=4, PIN_MIC_WS=5, PIN_MIC_SD=6;
constexpr uint8_t PIN_DISP_SCLK=7, PIN_DISP_RCLK=10, PIN_DISP_DIO=3;
constexpr uint8_t PIN_SHT_SDA=0, PIN_SHT_SCL=1;

// ---- Display-profile knobs: use values printed by Display_595_Probe ----
constexpr bool SEGMENT_ACTIVE_LOW=true;
constexpr bool DIGIT_ACTIVE_HIGH=true;
constexpr bool SEGMENT_FIRST=true;
constexpr bool REVERSE_DIGITS=false;
// Some boards use a non-standard segment pin order. DIGIT_FONT is written for
// logical bits a,b,c,d,e,f,g,dp. SEGMENT_MAP tells the driver which physical
// 595 output Q0..Q7 carries each logical segment.
// Identity = Q0=a, Q1=b, ... Q7=dp. If the probe's segment walk shows a
// different order, put the measured Q number here, e.g. {7,0,1,2,3,4,5,6}
// when Q7 lights segment a. Values must be 0..7 and all different.
constexpr uint8_t SEGMENT_MAP[8]={0,1,2,3,4,5,6,7};
constexpr uint8_t DIGIT_FONT[10]={
  0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F
};
constexpr uint8_t FONT_DASH=0x40, FONT_BLANK=0x00;

// ---- Wi-Fi settings (2.4 GHz network; edit before uploading) ----
const char WIFI_SSID[] = "DongDing701";
const char WIFI_PASSWORD[] = "Dongjiayi521";
constexpr char DEVICE_HOSTNAME[] = "soundmeter-c3";
constexpr uint16_t METRICS_PORT = 80;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 30000;
// Gateway ICMP is a LAN-only heartbeat; DO NOT use TrueNAS uptime to reset Wi-Fi.
constexpr uint32_t GATEWAY_PING_INTERVAL_MS = 30000;
constexpr uint32_t GATEWAY_PING_GRACE_MS = 45000;
constexpr uint8_t GATEWAY_PING_FAIL_LIMIT = 4;
// Long outages: restart radio first; reboot only as a last resort.
constexpr uint32_t WIFI_SOFT_RESET_AFTER_MS = 120000;
constexpr uint32_t WIFI_RESTART_AFTER_MS = 600000;
constexpr uint32_t SOFT_RESET_COOLDOWN_MS = 120000;
constexpr uint32_t WIFI_STATUS_INTERVAL_MS = 10000;
constexpr uint32_t READING_FRESH_MS = 5000;
constexpr uint32_t AUDIO_LOG_INTERVAL_MS = 5000;
// SHT31 is sampled independently: it must never block the HTTP loop.
constexpr uint32_t SHT_SAMPLE_INTERVAL_MS = 2000;
constexpr uint32_t SHT_FRESH_MS = 10000;
constexpr uint32_t SHT_LOG_INTERVAL_MS = 10000;

// ---- Sound level options ----
constexpr bool SHOW_A_WEIGHTED=true;
constexpr float CALIBRATION_OFFSET_DB=0.0f; // no calibration? leave zero
constexpr uint32_t SAMPLE_RATE_HZ=48000;
constexpr uint32_t DISPLAY_INTERVAL_MS=500;
constexpr size_t FRAMES_PER_READ=256;
constexpr double RMS_AT_94_DB_SPL=0.03543928915; // -26dBFS peak for 94dB SPL sine
constexpr double VERY_SMALL_RMS=1e-8;

WebServer httpServer(METRICS_PORT);
bool httpServerStarted = false;
bool wifiPreviouslyConnected = false;
uint32_t lastWifiRetryAt = 0;
uint32_t lastWifiStatusPrintAt = 0;
IPAddress listeningIP;
volatile uint32_t wifiDisconnectCount = 0;
uint32_t wifiReconnectAttemptCount = 0;
uint32_t httpRequestCount = 0;
uint32_t resetReason = 0;
volatile uint32_t audioLastReadUs = 0;
TaskHandle_t audioTaskHandle = nullptr;
TaskHandle_t shtTaskHandle = nullptr;
// Arduino Wi-Fi events arrive from another task: capture the reason here and
// print it from loop() rather than doing serial I/O from the event callback.
volatile int wifiLastDisconnectReason = -1;
volatile bool wifiHasDisconnectEvent = false;

// ESP-IDF ICMP echo session runs in its own lightweight task. Callbacks only
// update flags; WiFi/WebServer APIs are called in the Arduino loop task.
void serialPrintf(const char* fmt, ...);
void serialPrintln(const char* s);
esp_ping_handle_t gatewayPing = nullptr;
volatile bool gatewayPingGotReply = false;
volatile bool gatewayPingEnded = false;
bool gatewayPingRunning = false;
bool gatewayPingEverSucceeded = false;
uint8_t consecutiveGatewayFailures = 0;
uint32_t lastGatewayPingAt = 0;
uint32_t ipConnectedSince = 0;
uint32_t noIpSince = 0;
uint32_t continuousOfflineSince = 0;
uint32_t lastWifiResetAt = 0;
uint32_t gatewayPingFailuresTotal = 0;
uint32_t wifiSoftResetsTotal = 0;
uint32_t wifiStaleRecoveriesTotal = 0;
IPAddress gatewayPingIP;

static void onGatewayPingSuccess(esp_ping_handle_t, void*) {
  gatewayPingGotReply = true;
}
static void onGatewayPingEnd(esp_ping_handle_t, void*) {
  gatewayPingEnded = true;
}
// Keep the same session while the gateway address is unchanged. Avoid deleting
// an active ping session: after a disconnect it can safely time out.
void prepareGatewayPing(IPAddress gateway){
  if(gateway == IPAddress(0,0,0,0)) return;
  if(gatewayPing && gateway == gatewayPingIP) return;
  if(gatewayPing){
    if(gatewayPingRunning) return;
    esp_ping_delete_session(gatewayPing);
    gatewayPing = nullptr;
  }
  ip_addr_t ipAddr = {};
  String ipText = gateway.toString();
  if(!ipaddr_aton(ipText.c_str(), &ipAddr)) return;
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.count = 1;
  cfg.timeout_ms = 1200;
  cfg.interval_ms = 1000;
  cfg.target_addr = ipAddr;
  esp_ping_callbacks_t callbacks = {};
  callbacks.on_ping_success = onGatewayPingSuccess;
  callbacks.on_ping_end = onGatewayPingEnd;
  if(esp_ping_new_session(&cfg, &callbacks, &gatewayPing) == ESP_OK){
    gatewayPingIP = gateway;
    gatewayPingEverSucceeded = false;
    consecutiveGatewayFailures = 0;
    serialPrintf("[NET] gateway watchdog=%s started\n", ipText.c_str());
  } else {
    gatewayPing = nullptr;
    serialPrintln("[NET] gateway ping init failed; no forced recovery by ping");
  }
}
// Call from Arduino loop() only, not from asynchronous Wi-Fi event callbacks.
void resetWifiRadio(const char* reason){
  const uint32_t now = millis();
  if(lastWifiResetAt && (uint32_t)(now-lastWifiResetAt)<SOFT_RESET_COOLDOWN_MS) return;
  lastWifiResetAt = now;
  ++wifiSoftResetsTotal;
  serialPrintf("[RECOVERY] Wi-Fi radio reset #%lu: %s\n", (unsigned long)wifiSoftResetsTotal, reason);
  if(httpServerStarted){ httpServer.stop(); httpServerStarted=false; }
  wifiPreviouslyConnected=false;
  listeningIP=IPAddress(0,0,0,0);
  consecutiveGatewayFailures=0;
  gatewayPingEverSucceeded=false;
  // The ESP32-C3's display/audio tasks keep running during the radio reset.
  WiFi.disconnect(true, false);  // turn off radio, keep saved credentials
  delay(150);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiRetryAt=millis();
  noIpSince=lastWifiRetryAt;
}


void wifiEventHandler(WiFiEvent_t event, WiFiEventInfo_t info){
  if(event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED){
    wifiLastDisconnectReason = (int)info.wifi_sta_disconnected.reason;
    ++wifiDisconnectCount;
    wifiHasDisconnectEvent = true;
  }
}
// Audio sampling and HTTP serve run in separate FreeRTOS tasks on the C3's
// single core. A short critical section protects shared latest readings.
portMUX_TYPE readingMux = portMUX_INITIALIZER_UNLOCKED;
double latestDbA = NAN, latestDbZ = NAN;
uint32_t lastValidReadingAt = 0;
bool hasValidReading = false;
void publishReading(double a, double z, uint32_t at){
  portENTER_CRITICAL(&readingMux);
  latestDbA = a;
  latestDbZ = z;
  lastValidReadingAt = at;
  hasValidReading = true;
  portEXIT_CRITICAL(&readingMux);
}

// ---- GY-SHT31-D temperature/humidity (I2C) ----
// This is the same 0x2400, CRC-checked measurement proven by the user's
// standalone test. Read it in its own task so /metrics never waits for I2C.
portMUX_TYPE shtMux = portMUX_INITIALIZER_UNLOCKED;
float latestTemperatureC = NAN;
float latestHumidityPercent = NAN;
uint32_t shtLastGoodAt = 0;
bool shtHasReading = false;
uint32_t shtReadSuccesses = 0;
uint32_t shtReadErrors = 0;
uint8_t shtDetectedAddress = 0;
bool shtBusReady = false;

uint8_t shtCrc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31)
                         : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

uint8_t shtFindAddress() {
  const uint8_t candidates[] = {0x44, 0x45};
  for (uint8_t address : candidates) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) return address;
  }
  return 0;
}

bool shtReadMeasurement(uint8_t address, float &temperature, float &humidity) {
  if (!address) return false;
  Wire.beginTransmission(address);
  Wire.write((uint8_t)0x24);
  Wire.write((uint8_t)0x00); // single-shot, high repeatability, no clock stretching
  if (Wire.endTransmission() != 0) return false;

  vTaskDelay(pdMS_TO_TICKS(20)); // conversion completes in ~15 ms
  if (Wire.requestFrom(address, (uint8_t)6) != 6) return false;

  uint8_t data[6];
  for (uint8_t i = 0; i < 6; ++i) data[i] = (uint8_t)Wire.read();
  if (shtCrc8(data, 2) != data[2] || shtCrc8(data + 3, 2) != data[5])
    return false;

  const uint16_t rawT = ((uint16_t)data[0] << 8) | data[1];
  const uint16_t rawH = ((uint16_t)data[3] << 8) | data[4];
  temperature = -45.0f + 175.0f * rawT / 65535.0f;
  humidity = 100.0f * rawH / 65535.0f;
  return isfinite(temperature) && isfinite(humidity) &&
         temperature >= -40.0f && temperature <= 125.0f &&
         humidity >= 0.0f && humidity <= 100.0f;
}

void shtSamplingTask(void* unused) {
  (void)unused;
  uint8_t address = 0;
  uint8_t consecutiveFailures = 0;
  uint32_t lastLogAt = 0;
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    float temperature = NAN, humidity = NAN;
    if (shtBusReady && address == 0) {
      address = shtFindAddress();
      if (address) serialPrintf("[SHT31] detected at 0x%02X\n", address);
    }
    const bool ok = shtBusReady && address != 0 &&
                    shtReadMeasurement(address, temperature, humidity);
    const uint32_t now = millis();
    portENTER_CRITICAL(&shtMux);
    if (ok) {
      latestTemperatureC = temperature;
      latestHumidityPercent = humidity;
      shtLastGoodAt = now;
      shtHasReading = true;
      shtDetectedAddress = address;
      ++shtReadSuccesses;
    } else {
      ++shtReadErrors;
    }
    portEXIT_CRITICAL(&shtMux);

    if (ok) {
      consecutiveFailures = 0;
      if (lastLogAt == 0 || (uint32_t)(now - lastLogAt) >= SHT_LOG_INTERVAL_MS) {
        lastLogAt = now;
        serialPrintf("[SHT31] T=%.2f C, RH=%.2f %% (0x%02X)\n",
                     (double)temperature, (double)humidity, address);
      }
    } else {
      if (consecutiveFailures < 255) ++consecutiveFailures;
      if (consecutiveFailures == 1 || consecutiveFailures == 3) {
        serialPrintf("[SHT31] read failed (%u consecutive); audio/Wi-Fi continue\n",
                     (unsigned)consecutiveFailures);
      }
      if (consecutiveFailures >= 3) {
        // Allow a detached/replaced sensor to be re-discovered at 0x44 or 0x45.
        address = 0;
        consecutiveFailures = 0;
      }
    }
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SHT_SAMPLE_INTERVAL_MS));
  }
}

I2SClass mic;
int32_t rxStereo[FRAMES_PER_READ*2];
// 4 segment masks packed left to right, byte 0 = leftmost.
volatile uint32_t packedDisplay=0;
TaskHandle_t displayTaskHandle=nullptr;

inline uint8_t logicalPattern(uint32_t packed,uint8_t position){
  return (uint8_t)((packed >> (8*(3-position)))&0xFFu);
}
// Translate logical segment bits (a..dp) to the module's raw Q0..Q7 order.
inline uint8_t mapSegments(uint8_t logical){
  uint8_t raw=0;
  for(uint8_t i=0;i<8;i++){
    if(logical&(uint8_t)(1u<<i)) raw|=(uint8_t)(1u<<SEGMENT_MAP[i]);
  }
  return raw;
}
void emitRaw(uint8_t segments,uint8_t digitSelect){
  digitalWrite(PIN_DISP_RCLK,LOW);
  if(SEGMENT_FIRST){
    shiftOut(PIN_DISP_DIO,PIN_DISP_SCLK,MSBFIRST,segments);
    shiftOut(PIN_DISP_DIO,PIN_DISP_SCLK,MSBFIRST,digitSelect);
  } else {
    shiftOut(PIN_DISP_DIO,PIN_DISP_SCLK,MSBFIRST,digitSelect);
    shiftOut(PIN_DISP_DIO,PIN_DISP_SCLK,MSBFIRST,segments);
  }
  digitalWrite(PIN_DISP_RCLK,HIGH);
}
void displayRefreshTask(void* unused){
  (void)unused;
  uint8_t digitIndex=0;
  TickType_t lastWake=xTaskGetTickCount();
  for(;;){
    uint32_t snapshot=packedDisplay;
    uint8_t seg=mapSegments(logicalPattern(snapshot,digitIndex));
    if(SEGMENT_ACTIVE_LOW) seg=(uint8_t)~seg;
    uint8_t digitBit=(uint8_t)(1u<<(REVERSE_DIGITS?digitIndex:(3-digitIndex)));
    uint8_t digitMask=DIGIT_ACTIVE_HIGH?digitBit:(uint8_t)~digitBit;
    emitRaw(seg,digitMask);
    digitIndex=(digitIndex+1)&3;
    vTaskDelayUntil(&lastWake,pdMS_TO_TICKS(2)); // ~125 full refreshes/sec (1 kHz FreeRTOS tick)
  }
}
void setGlyphs(uint8_t a,uint8_t b,uint8_t c,uint8_t d){
  // aligned 32-bit write prevents a partially updated display pattern
  packedDisplay=((uint32_t)a<<24)|((uint32_t)b<<16)|((uint32_t)c<<8)|d;
}
void setError(){setGlyphs(FONT_DASH,FONT_DASH,FONT_DASH,FONT_DASH);}
void displayDb(double db){
  if(!isfinite(db)){ setError(); return; }
  int value=(int)lround(db);
  if(value<0)value=0;
  if(value>999)value=999;
  uint8_t c=DIGIT_FONT[value%10];
  uint8_t b=value>=10?DIGIT_FONT[(value/10)%10]:FONT_BLANK;
  uint8_t a=value>=100?DIGIT_FONT[(value/100)%10]:FONT_BLANK;
  setGlyphs(FONT_BLANK,a,b,c);
}

struct Biquad {
  double b0, b1, b2, a1, a2;
  double s1 = 0.0, s2 = 0.0;
  double process(double x) {
    const double y = b0*x + s1;
    s1 = b1*x - a1*y + s2;
    s2 = b2*x - a2*y;
    return y;
  }
};
Biquad aFilter[3] = {
  {0.2341830426035607, 0.4683660852071214, 0.2341830426035607,
    -0.2245584580597791, 0.0126066252715464},
  {1.0, -2.0, 1.0, -1.8938704947230710, 0.8951597690946617},
  {1.0, -2.0, 1.0, -1.9946144559930210, 0.9946217070140843}
};

// Simple DC removal for the unweighted display: not a certified Z-weight filter.
double dcEstimate = 0.0;
constexpr double DC_ALPHA = 0.0010;
uint64_t frameCounter = 0;
double energyA = 0.0, energyZ = 0.0;
uint32_t intervalSamples = 0;
uint32_t nextDisplayAt = 0;
uint32_t sampleFailureCount = 0;

// Non-blocking serial output. HWCDC::write() can block for seconds (or
// forever) if the USB host stops draining the port; the measurement and the
// display task must keep running even when nobody reads the serial monitor.
// If the whole message does not fit into the free TX space, drop it.
void serialPrintf(const char* fmt,...){
  char buf[192];
  va_list ap;
  va_start(ap,fmt);
  int n=vsnprintf(buf,sizeof(buf),fmt,ap);
  va_end(ap);
  if(n<=0) return;
  size_t len=((size_t)n<sizeof(buf))?(size_t)n:(sizeof(buf)-1);
  if(Serial.availableForWrite()>=(int)len){
    Serial.write((const uint8_t*)buf,len);
  }
}
void serialPrintln(const char* s){
  size_t len=strlen(s);
  if(Serial.availableForWrite()>=(int)(len+1)){
    Serial.write((const uint8_t*)s,len);
    Serial.write('\n');
  }
}

// Text exposition format (Prometheus). No historical samples are stored
// here: Prometheus scrapes the most recent valid sound level once per second.
void handleMetrics(){
  ++httpRequestCount;
  const uint32_t now = millis();
  double dbA, dbZ;
  uint32_t readingAt;
  bool valid;
  portENTER_CRITICAL(&readingMux);
  dbA = latestDbA;
  dbZ = latestDbZ;
  readingAt = lastValidReadingAt;
  valid = hasValidReading;
  portEXIT_CRITICAL(&readingMux);
  const bool fresh = valid && ((uint32_t)(now-readingAt) <= READING_FRESH_MS);
  String body;
  body.reserve(2800);
  body += "# HELP sound_meter_audio_up 1 when I2S audio readings are recent; otherwise 0.\n";
  body += "# TYPE sound_meter_audio_up gauge\n";
  body += String("sound_meter_audio_up ") + (fresh ? "1\n" : "0\n");
  if(fresh){
    // The v1.1 Feishu bot queries sound_level_db and labels it unweighted dB SPL.
    body += "# HELP sound_level_db Estimated unweighted sound pressure level, not calibrated.\n";
    body += "# TYPE sound_level_db gauge\n";
    body += "sound_level_db " + String(dbZ, 2) + "\n";
    body += "# HELP sound_level_dba Estimated A-weighted level, not calibrated.\n";
    body += "# TYPE sound_level_dba gauge\n";
    body += "sound_level_dba " + String(dbA, 2) + "\n";
  }
  body += "# HELP sound_meter_sample_age_seconds Seconds since latest valid audio reading.\n";
  body += "# TYPE sound_meter_sample_age_seconds gauge\n";
  if(valid){
    body += "sound_meter_sample_age_seconds " + String((uint32_t)(now-readingAt)/1000.0, 3) + "\n";
  }
  body += "# HELP sound_meter_uptime_seconds Seconds since ESP32 restart.\n";
  body += "# TYPE sound_meter_uptime_seconds gauge\n";
  body += "sound_meter_uptime_seconds " + String(now/1000.0, 2) + "\n";
  if(WiFi.status() == WL_CONNECTED){
    body += "# HELP sound_meter_wifi_rssi_dbm Current Wi-Fi RSSI in dBm.\n";
    body += "# TYPE sound_meter_wifi_rssi_dbm gauge\n";
    body += "sound_meter_wifi_rssi_dbm " + String(WiFi.RSSI()) + "\n";
  }
  body += "# TYPE sound_meter_wifi_disconnects_total counter\n";
  body += "sound_meter_wifi_disconnects_total " + String((unsigned long)wifiDisconnectCount) + "\n";
  body += "# TYPE sound_meter_wifi_reconnect_attempts_total counter\n";
  body += "sound_meter_wifi_reconnect_attempts_total " + String((unsigned long)wifiReconnectAttemptCount) + "\n";
  body += "# TYPE sound_meter_http_requests_total counter\n";
  body += "sound_meter_http_requests_total " + String((unsigned long)httpRequestCount) + "\n";
  body += "# TYPE sound_meter_audio_read_last_seconds gauge\n";
  body += "sound_meter_audio_read_last_seconds " + String((double)audioLastReadUs/1000000.0, 4) + "\n";
  body += "# TYPE sound_meter_free_heap_bytes gauge\n";
  body += "sound_meter_free_heap_bytes " + String((unsigned long)ESP.getFreeHeap()) + "\n";
  body += "# TYPE sound_meter_gateway_ping_failures_total counter\n";
  body += "sound_meter_gateway_ping_failures_total " + String((unsigned long)gatewayPingFailuresTotal) + "\n";
  body += "# TYPE sound_meter_wifi_soft_resets_total counter\n";
  body += "sound_meter_wifi_soft_resets_total " + String((unsigned long)wifiSoftResetsTotal) + "\n";
  body += "# TYPE sound_meter_wifi_stale_recoveries_total counter\n";
  body += "sound_meter_wifi_stale_recoveries_total " + String((unsigned long)wifiStaleRecoveriesTotal) + "\n";
  body += "# TYPE sound_meter_reset_reason gauge\n";
  body += "sound_meter_reset_reason " + String((unsigned long)resetReason) + "\n";
  // These values are independent of the sound/audio pipeline.
  // Do not export stale temperature/humidity as if they were live readings.
  float temperatureC, humidityPct;
  uint32_t tempLastAt, tempGood, tempBad;
  uint8_t tempAddress;
  bool tempHasReading;
  portENTER_CRITICAL(&shtMux);
  temperatureC = latestTemperatureC;
  humidityPct = latestHumidityPercent;
  tempLastAt = shtLastGoodAt;
  tempHasReading = shtHasReading;
  tempGood = shtReadSuccesses;
  tempBad = shtReadErrors;
  tempAddress = shtDetectedAddress;
  portEXIT_CRITICAL(&shtMux);
  const bool shtFresh = tempHasReading &&
     (uint32_t)(now - tempLastAt) <= SHT_FRESH_MS;
  body += "# HELP home_sht31_up 1 if a valid SHT31 sample was received recently.\n";
  body += "# TYPE home_sht31_up gauge\n";
  body += String("home_sht31_up ") + (shtFresh ? "1\n" : "0\n");
  if (shtFresh) {
    body += "# HELP home_temperature_celsius Room temperature from SHT31 in Celsius.\n";
    body += "# TYPE home_temperature_celsius gauge\n";
    body += "home_temperature_celsius " + String(temperatureC, 2) + "\n";
    body += "# HELP home_humidity_percent Relative humidity from SHT31 in percent.\n";
    body += "# TYPE home_humidity_percent gauge\n";
    body += "home_humidity_percent " + String(humidityPct, 2) + "\n";
  }
  if (tempHasReading) {
    body += "# TYPE home_sht31_sample_age_seconds gauge\n";
    body += "home_sht31_sample_age_seconds " +
            String((uint32_t)(now - tempLastAt) / 1000.0f, 3) + "\n";
  }
  body += "# TYPE home_sht31_reads_total counter\n";
  body += "home_sht31_reads_total " + String((unsigned long)tempGood) + "\n";
  body += "# TYPE home_sht31_read_errors_total counter\n";
  body += "home_sht31_read_errors_total " + String((unsigned long)tempBad) + "\n";
  body += "# TYPE home_sht31_i2c_address gauge\n";
  body += "home_sht31_i2c_address " + String((unsigned)tempAddress) + "\n";
  httpServer.sendHeader("Cache-Control", "no-store");
  httpServer.send(200, "text/plain; version=0.0.4; charset=utf-8", body);
}
void startNetwork(){
  // Keep Wi-Fi setup non-blocking so I2S and the LED refresh task keep working.
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(wifiEventHandler);
  httpServer.on("/metrics", HTTP_GET, handleMetrics);
  httpServer.on("/", HTTP_GET, [](){
    httpServer.send(200, "text/plain; charset=utf-8",
                    "ESP32-C3 sound meter. Prometheus: GET /metrics\n");
  });
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiRetryAt=millis();
  lastWifiStatusPrintAt=0;
  serialPrintln("Wi-Fi connecting (non-blocking)...");
}
void serviceNetwork(){
  const uint32_t now=millis();
  if(wifiHasDisconnectEvent){
    const int reason=wifiLastDisconnectReason;
    wifiHasDisconnectEvent=false;
    serialPrintf("[WIFI] disconnected reason=%d total=%lu\n",
      reason, (unsigned long)wifiDisconnectCount);
  }
  const bool connected = (WiFi.status()==WL_CONNECTED && WiFi.localIP()!=IPAddress(0,0,0,0));
  if(connected){
    noIpSince=0;
    continuousOfflineSince=0;
    const IPAddress ip=WiFi.localIP();
    if(!httpServerStarted || ip!=listeningIP){
      if(httpServerStarted) httpServer.stop();
      httpServer.begin();
      httpServerStarted=true;
      listeningIP=ip;
    }
    if(!wifiPreviouslyConnected){
      wifiPreviouslyConnected=true;
      ipConnectedSince=now;
      consecutiveGatewayFailures=0;
      lastGatewayPingAt=now; // allow TCP stack and DHCP to settle first
      serialPrintf("[WIFI] connected IP=%s gateway=%s RSSI=%d\n",
        ip.toString().c_str(), WiFi.gatewayIP().toString().c_str(), WiFi.RSSI());
    }
    httpServer.handleClient();

    // Critical: WL_CONNECTED does not guarantee the LAN path is usable.
    // Only reconnect based on gateway probes AFTER we have seen at least
    // one successful gateway ping during this boot/connection.
    prepareGatewayPing(WiFi.gatewayIP());
    if(gatewayPingEnded){
      gatewayPingEnded=false;
      gatewayPingRunning=false;
      if(gatewayPingGotReply){
        gatewayPingEverSucceeded=true;
        consecutiveGatewayFailures=0;
      }else{
        ++gatewayPingFailuresTotal;
        if(gatewayPingEverSucceeded && consecutiveGatewayFailures<255) ++consecutiveGatewayFailures;
        serialPrintf("[NET] gateway ping missed, failures=%u, ever_ok=%d\n",
          (unsigned)consecutiveGatewayFailures,(int)gatewayPingEverSucceeded);
      }
    }
    if(gatewayPing && !gatewayPingRunning &&
       (uint32_t)(now-ipConnectedSince)>=GATEWAY_PING_GRACE_MS &&
       (uint32_t)(now-lastGatewayPingAt)>=GATEWAY_PING_INTERVAL_MS){
      gatewayPingGotReply=false;
      gatewayPingEnded=false;
      if(esp_ping_start(gatewayPing)==ESP_OK){
        gatewayPingRunning=true;
        lastGatewayPingAt=now;
      }
    }
    if(gatewayPingEverSucceeded &&
       consecutiveGatewayFailures>=GATEWAY_PING_FAIL_LIMIT){
      // Count only actual requested recovery attempts; radio reset is rate limited.
      if(!lastWifiResetAt || (uint32_t)(now-lastWifiResetAt)>=SOFT_RESET_COOLDOWN_MS)
        ++wifiStaleRecoveriesTotal;
      resetWifiRadio("connected but gateway does not reply to ICMP");
      return;
    }
  }else{
    if(!noIpSince) noIpSince=now;
    if(!continuousOfflineSince) continuousOfflineSince=now;
    if((uint32_t)(now-continuousOfflineSince)>=WIFI_RESTART_AFTER_MS){
      serialPrintln("[RECOVERY] Wi-Fi unusable for 10min: rebooting ESP32 as last resort");
      delay(50);
      ESP.restart();
    }
    if(wifiPreviouslyConnected || httpServerStarted){
      wifiPreviouslyConnected=false;
      if(httpServerStarted){
        httpServer.stop();
        httpServerStarted=false;
      }
      serialPrintln("[WIFI] offline; HTTP listener stopped");
    }
    // WiFi.reconnect() alone may fail to break a long-lived bad state.
    if((uint32_t)(now-noIpSince) >= WIFI_SOFT_RESET_AFTER_MS){
      resetWifiRadio("no Wi-Fi IP for 120 seconds");
    }else if((uint32_t)(now-lastWifiRetryAt)>=WIFI_RETRY_INTERVAL_MS){
      lastWifiRetryAt=now;
      ++wifiReconnectAttemptCount;
      bool accepted=WiFi.reconnect();
      serialPrintf("[WIFI] reconnect #%lu status=%d accepted=%d reason=%d\n",
        (unsigned long)wifiReconnectAttemptCount, (int)WiFi.status(),
        (int)accepted, (int)wifiLastDisconnectReason);
    }
  }
  if(lastWifiStatusPrintAt==0 || (uint32_t)(now-lastWifiStatusPrintAt)>=WIFI_STATUS_INTERVAL_MS){
    lastWifiStatusPrintAt=now;
    if(connected){
      serialPrintf("[NET] IP=%s RSSI=%d HTTP=%d gatewayFailures=%u\n",
        WiFi.localIP().toString().c_str(), WiFi.RSSI(), (int)httpServerStarted,
        (unsigned)consecutiveGatewayFailures);
    }else{
      serialPrintf("[NET] offline status=%d reason=%d seconds=%lu\n",
        (int)WiFi.status(),(int)wifiLastDisconnectReason,
        (unsigned long)((now-noIpSince)/1000));
    }
  }
}

void audioSamplingTask(void* unused);

void setup(){
  Serial.begin(115200);
  delay(600);
  resetReason=(uint32_t)esp_reset_reason();
  pinMode(PIN_DISP_SCLK,OUTPUT);
  pinMode(PIN_DISP_RCLK,OUTPUT);
  pinMode(PIN_DISP_DIO,OUTPUT);
  digitalWrite(PIN_DISP_SCLK,LOW);
  digitalWrite(PIN_DISP_RCLK,LOW);
  digitalWrite(PIN_DISP_DIO,LOW);
  setGlyphs(DIGIT_FONT[8],DIGIT_FONT[8],DIGIT_FONT[8],DIGIT_FONT[8]);
  // Single-core ESP32-C3: independent FreeRTOS refresh task is important;
  // I2S.readBytes can block for ~5ms, causing obvious flicker otherwise.
  BaseType_t ok=xTaskCreate(displayRefreshTask,"display595",2048,nullptr,2,&displayTaskHandle);
  if(ok!=pdPASS){ serialPrintln("FATAL: display task creation failed"); while(true)delay(1000); }
  delay(500);
  setGlyphs(FONT_BLANK,FONT_BLANK,FONT_BLANK,FONT_BLANK);
  mic.setPins(PIN_MIC_SCK,PIN_MIC_WS,-1,PIN_MIC_SD);
  if(!mic.begin(I2S_MODE_STD,SAMPLE_RATE_HZ,I2S_DATA_BIT_WIDTH_32BIT,
                I2S_SLOT_MODE_STEREO,I2S_STD_SLOT_BOTH)){
    serialPrintln("FATAL: I2S setup failed (ESP32 core 3.x?)");
    setError();
    while(true) delay(1000);
  }
  serialPrintln("ESP32-C3 / INMP441 / 74HC595 sound meter (estimated, not certified)");
  serialPrintf("Display profile: segmentLow=%d digitHigh=%d segmentFirst=%d reverseDigits=%d\n",
     SEGMENT_ACTIVE_LOW,DIGIT_ACTIVE_HIGH,SEGMENT_FIRST,REVERSE_DIGITS);
  char mapLine[64];
  int mapPos=snprintf(mapLine,sizeof(mapLine),"Segment map a..dp -> Q");
  uint8_t mapBits=0;
  for(uint8_t i=0;i<8;i++){
    if(mapPos>0 && mapPos<(int)sizeof(mapLine)){
      mapPos+=snprintf(mapLine+mapPos,sizeof(mapLine)-(size_t)mapPos,"%s%u",i?",":"",(unsigned)SEGMENT_MAP[i]);
    }
    if(SEGMENT_MAP[i]<8) mapBits|=(uint8_t)(1u<<SEGMENT_MAP[i]);
  }
  serialPrintln(mapLine);
  if(mapBits!=0xFF) serialPrintln("WARNING: SEGMENT_MAP should be a permutation of 0..7; glyphs may be wrong.");
  serialPrintf("Sound display: %s, rate=%luHz, calibration offset=%.1f dB\n",
     SHOW_A_WEIGHTED?"estimated dB(A)":"unweighted dB SPL",
     (unsigned long)SAMPLE_RATE_HZ,(double)CALIBRATION_OFFSET_DB);
  nextDisplayAt=millis()+DISPLAY_INTERVAL_MS;
  BaseType_t audioOk=xTaskCreate(audioSamplingTask,"audioI2S",8192,nullptr,1,&audioTaskHandle);
  if(audioOk!=pdPASS){ serialPrintln("FATAL: audio task creation failed"); setError(); while(true)delay(1000); }
  // GPIO0=SDA GPIO1=SCL; this module was detected at 0x44 in standalone test.
  shtBusReady = Wire.begin(PIN_SHT_SDA, PIN_SHT_SCL);
  if (shtBusReady) {
    Wire.setClock(100000);  // 100 kHz reduces wiring sensitivity
    Wire.setTimeOut(50);   // avoid hanging too long on a faulty I2C bus
    BaseType_t shtOk = xTaskCreate(shtSamplingTask, "sht31", 4096,
                                  nullptr, 1, &shtTaskHandle);
    if (shtOk != pdPASS) serialPrintln("[SHT31] task creation failed; sound meter still runs");
  } else {
    serialPrintln("[SHT31] I2C init failed; sound meter still runs");
  }
  startNetwork();
}
void audioSamplingTask(void* unused){
  (void)unused;
  uint32_t lastAudioLogAt=0;
  for(;;){
  const uint32_t readStartUs=micros();
  const size_t len=mic.readBytes((char*)rxStereo,sizeof(rxStereo));
  audioLastReadUs=micros()-readStartUs;
  const size_t frames=len/(2*sizeof(int32_t));
  if(frames==0){
    if(++sampleFailureCount>100){
      serialPrintln("ERROR: no I2S samples; check audio wiring");
      setError();sampleFailureCount=0;
    }
    vTaskDelay(pdMS_TO_TICKS(2));continue;
  }
  sampleFailureCount=0;
  for(size_t i=0;i<frames;i++){
    // INMP441 L/R is grounded: only use LEFT slot.
    const double x=(double)rxStereo[2*i]/2147483648.0;
    dcEstimate+=DC_ALPHA*(x-dcEstimate);
    const double z=x-dcEstimate;
    double a=x;
    for(int k=0;k<3;k++) a=aFilter[k].process(a);
    if(frameCounter>=SAMPLE_RATE_HZ){
      energyZ+=z*z;
      energyA+=a*a;
      ++intervalSamples;
    }
    ++frameCounter;
  }
  const uint32_t now=millis();
  if((int32_t)(now-nextDisplayAt)>=0){
    nextDisplayAt=now+DISPLAY_INTERVAL_MS;
    if(!intervalSamples)continue;
    double rmsZ=sqrt(energyZ/(double)intervalSamples);
    double rmsA=sqrt(energyA/(double)intervalSamples);
    energyZ=energyA=0; intervalSamples=0;
    if(rmsZ<VERY_SMALL_RMS){
      serialPrintln("ERROR: audio near-zero; check INMP441 left channel");
      setError();continue;
    }
    const double dbZ=94.0+20.0*log10(rmsZ/RMS_AT_94_DB_SPL)+CALIBRATION_OFFSET_DB;
    const double dbA=94.0+20.0*log10(rmsA/RMS_AT_94_DB_SPL)+CALIBRATION_OFFSET_DB;
    if(isfinite(dbA) && isfinite(dbZ)) publishReading(dbA,dbZ,now);
    displayDb(SHOW_A_WEIGHTED?dbA:dbZ);
    if((uint32_t)(now-lastAudioLogAt)>=AUDIO_LOG_INTERVAL_MS){
      lastAudioLogAt=now;
      serialPrintf("A_est=%.1f Z_est=%.1f dB (estimated); read=%lu us\n",
        dbA,dbZ,(unsigned long)audioLastReadUs);
    }
  }
  // Give Arduino loop() chances to serve HTTP on the same C3 core.
  taskYIELD();
  }
}

void loop(){
  serviceNetwork();
  delay(1);   // cooperative network task; no blocking audio reads here
}
