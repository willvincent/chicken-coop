/**
 * \file
 *       Arduino Chicken Coop Controller
 * \author
 *       Will Vincent <will@willvincent.com>
 */
 
#include <Wire.h>
#include <OneWire.h>
#include <LiquidCrystal.h>
#include <espduino.h>
#include <mqtt.h>
#include "RTClib.h"
 
// print debug messages or not to serial
const boolean Debugging = true;

// Digital & analog pins for various components
const int tempSense     =  2; // DS18S20 Temperature Sensor
const int lightSense    = A0; // Light Sensor
const int lampRelay     = 10; // Lamp Relay
const int heaterRelay   =  9; // Water Heater Relay
const int doorOpen      = 37; // MotorA left
const int doorClose     = 38; // MotorA right
const int doorSpeed     = 39; // MotorA speed
const int doorTop       = 40; // Reed Switch
const int doorBottom    = 41; // Reed Switch
const int doorOpenLED   = 11; // Indicator light (blinks during open/close steady on when open)
const int doorClosedLED = 12; // Indicator Light (blinks during open/close steady on when closed)
const int fan           = 35; // MotorB left
const int fanSpeed      = 36; // MotorB speed
const int rtcSDA        = 20; // Real Time Clock SDA (i2c arduino mega)
const int rtcSCL        = 21;  // Real Time Clock SCL (i2c arduino mega)

// LCD Pins
const int lcd_4  = 24;  // LCD RS
const int lcd_5  = 25;  // LCD R/W
const int lcd_6  = 26;  // LCD Enable
const int lcd_11 = 27; // LCD Data 4
const int lcd_12 = 28; // LCD Data 5
const int lcd_13 = 29; // LCD Data 6
const int lcd_14 = 30; // LCD Data 7
const int lcd_bl = 13; // LCD Backlight+

// ESP8266 Settings
#define debugger Serial // Pins 0 and 1
#define espPort Serial1 // Pins 18 and 19
const int chpdPin = 3;

// WIFI Settings
#define wHost "WIFI-SSID"
#define wPass "WIFI-PASSWORD"

// MQTT Host Settings
#define mHost "domain.com"
#define mPort 1883

// MQTT Subscription Channels
#define sTime "time/beacon"
#define sRemote "coop/remotetrigger"

// MQTT Publish Channels
#define pTemp "coop/temperature"
#define pLight "coop/brightness"
#define pStatus "coop/status"

// Misc Settings
const unsigned long millisPerDay    = 86400000; // Milliseconds per day
const unsigned long debounceWait    =      100; // Debounce timer (100ms)
const unsigned long remoteOverride  =   600000; // Length of time to lockout readings. (10min)
const unsigned long readingInterval =   300000; // How often to take sensor readings. (5min)
const int           fanThreshold    =       78; // When temperature exceeds this threshold, turn on fan
const int           heaterThreshold =       36; // When temperature is below this threshold, turn on water heater


// Runtime variables
unsigned long lastReading  = 0;
unsigned long lastDebounce = 0;
unsigned long lastRTCSync  = 0;
String        doorState    = "closed"; // Values will be one of: closed, closing, open, opening
boolean       heaterState  = false;
boolean       lampState    = false;
boolean       fanState     = false;
float tempC;
float tempF;
int brightness;

RTC_DS1307 rtc;
OneWire ds(tempSense);
LiquidCrystal lcd(lcd_4, lcd_5, lcd_6, lcd_11, lcd_12, lcd_13, lcd_14);

ESP esp(&espPort, chpdPin);
MQTT mqtt(&esp);
boolean wifiConnected = false;

void wifiCb(void* response) {
  uint32_t status;
  RESPONSE res(response);
  
  if (res.getArgc() == 1) {
    res.popArgs((uint8_t*)&status, 4);
    if (status == STATION_GOT_IP) {
      if (Debugging) {
        debugger.println("WIFI CONNECTED");
      }
      mqtt.connect(mHost, mPort);
      wifiConnected = true;
    }
    else {
      wifiConnected = false;
      mqtt.disconnect();
    }
  }
}

void mqttConnected(void* response) {
  if (Debugging) {
    debugger.println("MQTT Connected");
  }
  mqtt.subscribe(sTime);
  mqtt.subscribe(sRemote);
}

void mqttDisconnected(void* response) {
  if (Debugging) {
    debugger.println("MQTT Disconnected");
  }
}

void mqttPublished(void* response) {}

void mqttData(void* response) {
  RESPONSE res(response);
  String topic = res.popString();
  String data  = res.popString();
  if (Debugging) {
    debugger.print("MQTT Data Received: topic=");
    debugger.print(topic);
    debugger.print(" :: data=");
    debugger.println(data);
  }
  
  if (topic == "coop/remotetrigger") {
    // @TODO: Handle remote trigger for DOOR events
    
    if (data == "light") {
      toggleLamp();
      lastReading = millis() + remoteOverride;
    }
    
    if (data == "water heater") {
      toggleHeater();
      lastReading = millis() + remoteOverride;
    }
    
    if (data == "fan") {
      toggleFan();
      lastReading = millis() + remoteOverride;
    }
  }
  
  // Sync RTC to time beacon once/day
  if (topic == "time/beacon") {
    if ((millis() - millisPerDay) > lastRTCSync) {
      rtc.adjust(atoi(data.c_str()));
      lastRTCSync = millis();
    }
  }
}

/**
 * Toggle Lamp
 */
void toggleLamp() {
  if (lampState) {
    digitalWrite(lampRelay, LOW);
    mqtt.publish(pStatus, "light|off");
  }
  else {
    digitalWrite(lampRelay, HIGH);
    mqtt.publish(pStatus, "light|on");
  }
  lampState = !lampState;
}

/**
 * Toggle Heater
 */
void toggleHeater() {
  if (heaterState) {
    digitalWrite(heaterRelay, LOW);
    mqtt.publish(pStatus, "water heater|off");
  }
  else {
    digitalWrite(heaterRelay, HIGH);
    mqtt.publish(pStatus, "water heater|on");
  }
  heaterState = !heaterState;
}

/**
 * Toggle Fan
 */
void toggleFan() {
  if (fanState) {
    digitalWrite(fan, LOW);
    digitalWrite(fanSpeed, LOW);
    mqtt.publish(pStatus, "fan|off");
  }
  else {
    digitalWrite(fan, HIGH);
    digitalWrite(fanSpeed, HIGH);
    mqtt.publish(pStatus, "fan|on");
  }
  fanState = !fanState;
}

/**
 * get Temperature Reading (in celcius)
 */
float getTemp() {
  byte data[12];
  byte addr[8];
  
  if (!ds.search(addr)) {
    // No more sensors on chain, reset search
    ds.reset_search();
    return -1000;
  }
  
  if (OneWire::crc8(addr, 7) != addr[7]) {
    if (Debugging) {
      debugger.println("Error reading temperature sensor: CRC is not valid!");
    }
    return -1000;
  }
  
  if (addr[0] != 0x10 && addr[0] != 0x28) {
    if (Debugging) {
      debugger.println("Error reading temperature sensor: Device is not recognized");
    }
    return -1000;
  }
  
  ds.reset();
  ds.select(addr);
  ds.write(0x44,1); // Start conversation, with parasite power on at the end
  
  byte present = ds.reset();
  ds.select(addr);
  ds.write(0xBE); // Read Scratchpad
  
  for (int i = 0; i < 9; i++) {
    data[i] = ds.read();
  }
  
  ds.reset_search();
  
  byte MSB = data[1];
  byte LSB = data[0];
  
  float tempRead = ((MSB << 8) | LSB);
  float TemperatureSum = tempRead / 16;
  
  return TemperatureSum;
}

void updateLCD() {
  char tempStr[5];
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print(dtostrf(tempF, 5, 1, tempStr));
  lcd.print("F | L ");
  lcd.print(brightness);
  lcd.print("%");
  lcd.setCursor(0,1);
  lcd.print("Door: ");
  lcd.print(doorState);
}

/****************************/

void readSensors() {
  // Fetch temp and convert to farenheit
  tempC = getTemp();
  tempF = ((tempC * 9.0) / 5.0) + 32;
  char tempStr[5];
  mqtt.publish(pTemp, dtostrf(tempF, 5, 1, tempStr));
  
  // Read light sensor and convert to brightness percentage
  brightness = analogRead(tempSense);
  brightness = map(brightness, 746, 13, 0, 100);  // Remap value to a 0-100 scale
  brightness = constrain(brightness, 0, 100);     // constrain value to 0-100 scale
  char briStr[3];
  mqtt.publish(pLight, dtostrf(brightness, 3, 0, briStr));
  
  lastReading = millis();
}



void setup() {
  if (Debugging) {
    debugger.begin(115200);
    debugger.println("Initialising...");
  }
  espPort.begin(115200);
  esp.enable();
  delay(500);
  esp.reset();
  delay(500);
  while(!esp.ready());
  
  if (Debugging) {
    debugger.println("ARDUINO: Setup MQTT client");
  }
  if (!mqtt.begin("coop_duino", "", "", 120, 1)) { // client_id, username, password, keepalive time, cleansession boolean
    if (Debugging) {
      debugger.println("ARDUINO: Failed to setup MQTT");
    }
    while(1);
  }
  
  mqtt.connectedCb.attach(&mqttConnected);
  mqtt.disconnectedCb.attach(&mqttDisconnected);
  mqtt.publishedCb.attach(&mqttPublished);
  mqtt.dataCb.attach(&mqttData);
  
  if (Debugging) {
    debugger.println("ARDUINO: Setup WIFI");
  }
  esp.wifiCb.attach(&wifiCb);
  esp.wifiConnect(wHost, wPass);
  
  
  /**
   * Setup pin modes
   */
  pinMode(lampRelay, OUTPUT);
  pinMode(heaterRelay, OUTPUT);  
  pinMode(doorOpen, OUTPUT);
  pinMode(doorClose, OUTPUT);
  pinMode(doorSpeed, OUTPUT);
  pinMode(doorOpenLED, OUTPUT);
  pinMode(doorClosedLED, OUTPUT);
  pinMode(doorTop, INPUT);
  pinMode(doorBottom, INPUT);
  pinMode(fan, OUTPUT);
  pinMode(fanSpeed, OUTPUT);
  pinMode(lcd_bl, OUTPUT);
  
  // Pin defaults
  digitalWrite(lampRelay, LOW);
  digitalWrite(heaterRelay, LOW);
  digitalWrite(doorOpen, LOW);
  digitalWrite(doorClose, LOW);
  digitalWrite(doorSpeed, LOW);
  digitalWrite(doorOpenLED, LOW);
  digitalWrite(doorClosedLED, LOW);
  digitalWrite(doorTop, HIGH);    // Enable resistor
  digitalWrite(doorBottom, HIGH); // Enable resistor
  digitalWrite(fan, LOW);
  digitalWrite(fanSpeed, LOW);
  digitalWrite(lcd_bl, HIGH);
  
  lcd.begin(16,2);
}



void loop() {
  esp.process();
  
  if ((millis() - readingInterval) > lastReading) {
    // Do stuff!
    readSensors();
    updateLCD();
    // 
  }
}
