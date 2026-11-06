/*
  ESP32-C3 SuperMini + INMP441 + five-pin 74HC595 four-digit LED
  v2.2 / illustrative DIY sound level monitor (not calibrated / not IEC 61672).
  Arduino IDE: esp32 by Espressif Systems 3.x, board ESP32C3 Dev Module
  Library: ESP_I2S.h bundled with Arduino-ESP32 3.x. No TM1637 library.

  INMP441: VDD 3V3, GND GND, SCK GPIO4, WS GPIO5, SD GPIO6, L/R GND
  DISPLAY: VCC 3V3, GND GND, SCLK GPIO7, RCLK GPIO10, DIO GPIO3

  IMPORTANT: This display driver assumes TWO chained 74HC595s, one byte
  for segments and one for digit selection. Run Display_595_Probe (v3)
  first: it shows "1234" then the four profile bits, and can also print
  the profile lines plus a raw segment walk over serial.
  Change the profile and SEGMENT_MAP based on what the probe shows.
*/
#include <Arduino.h>
#include <ESP_I2S.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

constexpr int PIN_MIC_SCK=4, PIN_MIC_WS=5, PIN_MIC_SD=6;
constexpr uint8_t PIN_DISP_SCLK=7, PIN_DISP_RCLK=10, PIN_DISP_DIO=3;

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

// ---- Sound level options ----
constexpr bool SHOW_A_WEIGHTED=true;
constexpr float CALIBRATION_OFFSET_DB=0.0f; // no calibration? leave zero
constexpr uint32_t SAMPLE_RATE_HZ=48000;
constexpr uint32_t DISPLAY_INTERVAL_MS=500;
constexpr size_t FRAMES_PER_READ=256;
constexpr double RMS_AT_94_DB_SPL=0.03543928915; // -26dBFS peak for 94dB SPL sine
constexpr double VERY_SMALL_RMS=1e-8;

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

void setup(){
  Serial.begin(115200);
  delay(600);
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
}
void loop(){
  const size_t len=mic.readBytes((char*)rxStereo,sizeof(rxStereo));
  const size_t frames=len/(2*sizeof(int32_t));
  if(frames==0){
    if(++sampleFailureCount>100){
      serialPrintln("ERROR: no I2S samples; check audio wiring");
      setError();sampleFailureCount=0;
    }
    delay(2);return;
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
    if(!intervalSamples)return;
    double rmsZ=sqrt(energyZ/(double)intervalSamples);
    double rmsA=sqrt(energyA/(double)intervalSamples);
    energyZ=energyA=0; intervalSamples=0;
    if(rmsZ<VERY_SMALL_RMS){
      serialPrintln("ERROR: audio near-zero; check INMP441 left channel");
      setError();return;
    }
    const double dbZ=94.0+20.0*log10(rmsZ/RMS_AT_94_DB_SPL)+CALIBRATION_OFFSET_DB;
    const double dbA=94.0+20.0*log10(rmsA/RMS_AT_94_DB_SPL)+CALIBRATION_OFFSET_DB;
    displayDb(SHOW_A_WEIGHTED?dbA:dbZ);
    serialPrintf("A_est=%.1f dB(A)  Z_est=%.1f dB SPL  rmsA=%.7f rmsZ=%.7f\n",dbA,dbZ,rmsA,rmsZ);
  }
}
