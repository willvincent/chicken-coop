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
const int Debugging = 0;

// Digital & analog pins for various components
const int tempSense     =  6; // DS18S20 Temperature Sensor
const int lightSense    = A0; // Light Sensor
const int lampRelay     = 10; // Lamp Relay
const int heaterRelay   =  9; // Water Heater Relay
const int doorOpen      = 35; // MotorA left
const int doorClose     = 36; // MotorA right
const int doorEnable    = 37; // MotorA enable
const int doorTop       = 51; // Reed Switch
const int doorBottom    = 53; // Reed Switch
const int doorOpenLED   = 11; // Indicator light (blinks during open/close steady on when open)
const int doorClosedLED = 12; // Indicator Light (blinks during open/close steady on when closed)
const int fan           = 38; // MotorB left
const int fanEnable     = 39; // MotorB enable
const int rtcSDA        = 20; // Real Time Clock SDA (i2c arduino mega)
const int rtcSCL        = 21; // Real Time Clock SCL (i2c arduino mega)

// LCD Pins
// LCD pin 1 to ground
// LCD pin 2 to +5v
// LCD pin 3 to ground with 330ohm resistor (contrast)
const int lcd_4  = 24; // LCD RS
// LCD pin 5 to ground
const int lcd_6  = 26; // LCD Enable
// LCD pins 7-10 disconnected
const int lcd_11 = 27; // LCD Data 4
const int lcd_12 = 28; // LCD Data 5
const int lcd_13 = 29; // LCD Data 6
const int lcd_14 = 30; // LCD Data 7
const int lcd_15 =  8; // LCD Backlight+ (to pwm with 220ohm resistor)
// LCD Backlight- (pin 16) to ground


// ESP8266 Settings
#define debugger Serial // Pins 0 and 1
#define espPort Serial1 // Pins 18 and 19
const int chpdPin = 3;

// WIFI Settings
#define wHost "WIFI-SSID"
#define wPass "WIFI-PASSWORD"

// MQTT Host Settings
#define mHost "your-domain.com"
#define mPort 1883
#define mClientID "coop-duino"
#define mUsername "chicken_coop"
#define mPassword "secret"

// MQTT Subscription Channels
#define sTime    "time/beacon"
#define sRemote  "coop/remotetrigger"
#define sSunRise "sun/rise"
#define sSunSet  "sun/set"

// MQTT Publish Channels
#define pTemp "coop/temperature"
#define pLight "coop/brightness"
#define pStatus "coop/status"

// Misc Settings
const unsigned long millisPerDay    = 86400000; // Milliseconds per day
const unsigned long debounceWait    =      200; // Door debounce timer (200 ms)
const unsigned long ledBlinkRate    =      250; // Delay between LED blinks (1/4 sec)
const unsigned long lcdChange       =     5000; // How often to change LCD pages (5 sec)
const unsigned long lightReadRate   =     1000; // How often to read light level (1 sec)
const unsigned long remoteOverride  =   600000; // Length of time to lockout readings. (10 min)
const unsigned long publishInterval =    60000; // How often to publish light/temp sensor readings. (1 min)
const int           fanThreshold    =       78; // When temperature (Farenheit) exceeds this threshold, turn on fan
const int           heaterThreshold =       36; // When temperature (Farenheit) is below this threshold, turn on water heater
const int           lsLow           =        0; // Light Sensor lowest reading
const int           lsHigh          =      900; // Light Sensor highest reading

// Night time lockout to prevent reaction to light sensor readings if an exterior light source causes
// a reading otherwise bright enough to activate the interior light and/or door.
const boolean       nightLock      =      true; // Enable night time lockout


/*************************************************
       DO   NOT   EDIT   BELOW   THIS   LINE
 *************************************************/

// Runtime variables
int           nightLockStart     =    22; // Hour (in 24hr time) to initiate night time lockout (10pm)
int           nightLockEnd       =     4; // Hour (in 24hr time) to end night time lockout (4am)
unsigned long lastPublish        =     0;
unsigned long lastDebounce       =     0;
unsigned long lastLightRead      =     0;
unsigned long lastRTCSync        =     0;
unsigned long lastLEDBlink       =     0;
unsigned long remoteLockStart    =     0;
unsigned long lastLCDChange      =     0;
String        doorState          =    ""; // Values will be one of: closed, closing, open, opening
String        doorStatePrev      =    "";
boolean       heaterState        = false;
boolean       lampState          = false;
boolean       fanState           = false;
float         tempC              =     0;
float         tempF              =     0;
int           brightness         =     0;
int           doorTopVal         =     0;
int           doorTopVal2        =     0;
int           doorTopState       =     0;
int           doorTopPrev        =     0;
int           doorBottomVal      =     0;
int           doorBottomVal2     =     0;
int           doorBottomState    =     0;
int           doorBottomPrev     =     0;
int           lcdPage            =     0; // Toggle between LCD page states
uint32_t      bootTime           =     0;

RTC_DS1307 RTC;
OneWire ds(tempSense);
LiquidCrystal lcd(lcd_4, lcd_6, lcd_11, lcd_12, lcd_13, lcd_14);

#if Debugging > 0
  ESP esp(&espPort, &debugger, chpdPin);
#else
  ESP esp(&espPort, chpdPin);
#endif

MQTT mqtt(&esp);
boolean wifiConnected = false;

/**
 * Manage WIFI connection
 */
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

/**
 * MQTT Connection event handler.
 *
 * Subscribes to desired channels
 */
void mqttConnected(void* response) {
  if (Debugging) {
    debugger.println("MQTT Connected");
  }

  // Subscribe to time beacon channel to keep RTC up to date.
  mqtt.subscribe(sTime, 0);

  // Subscribe to remote trigger channel to allow remote control of chicken coop
  mqtt.subscribe(sRemote, 2);

  // Subscribe to sunrise/set updates
  mqtt.subscribe(sSunRise, 2);
  mqtt.subscribe(sSunSet, 2);
  
  // Publish that we're online!
  mqtt.publish("client/online", "1", 2, 0);
}

/**
 * MQTT Disconnect event handler.
 */
void mqttDisconnected(void* response) {
  if (Debugging) {
    debugger.println("MQTT Disconnected");
  }
}

/**
 * MQTT Publish even handler.
 *
 * We don't really need this, but it allows logging of any sent
 * messages for debugging purposes.
 */
void mqttPublished(void* response) {
  RESPONSE res(response);
  String topic = res.popString();
  String data  = res.popString();
/*  if (Debugging) {
    if (topic != "time/beacon") {
      debugger.print("MQTT Data published to channel '");
      debugger.print(topic);
      debugger.print("': ");
      debugger.println(data);
    }
  } */
}

/**
 * Handle incoming MQTT messages.
 *
 * This allows us to remotely trigger events via WIFI!
 */
void mqttData(void* response) {
  RESPONSE res(response);
  String topic = res.popString();
  String data  = res.popString();

  if (topic == sRemote) {
    // If door movement is triggered, toggle door state to
    // opening or closing based on current state.
    // If door is currently moving, the trigger is ignored.
    if (data == "door") {
      if (doorState == "open") {
        doorState = "closing";
        remoteLockStart = millis();
      }
      else if (doorState == "closed") {
        doorState = "opening";
        remoteLockStart = millis();
      }
    }

    // Toggle interior light
    if (data == "light") {
      toggleLamp();
      remoteLockStart = millis();
    }

    // Toggle water heater
    if (data == "water heater") {
      toggleHeater();
      remoteLockStart = millis();
    }

    // Toggle fan
    if (data == "fan") {
      toggleFan();
      remoteLockStart = millis();
    }
  }

  // Adjust sunrise/set times for nightlock
  if (topic == sSunRise) {
    nightLockEnd = atoi(data.c_str());
    if (Debugging) {
      debugger.print("Night lock end updated to: ");
      debugger.println(nightLockEnd);
    }
  }
  
  if (topic == sSunSet) {
    nightLockStart = atoi(data.c_str());
    if (Debugging) {
      debugger.print("Night lock start updated to: ");
      debugger.println(nightLockStart);
    }
  }

  // Sync RTC to time beacon once/day
  if (topic == sTime) {
    if (lastRTCSync == 0 || ((unsigned long)(millis() - lastRTCSync) > millisPerDay)) {
      RTC.adjust(strtoul(data.c_str(), NULL, 0));
      lastRTCSync = millis();
      if (Debugging) {
        DateTime now = RTC.now();
        char dateStr[10];
        char timeStr[8];
        sprintf(dateStr, "%02d/%02d/%04d", now.month(), now.day(), now.year());
        int hr = now.hour();
        boolean ampm = false;
        if (hr > 12) {
          hr = hr - 12;
          ampm = true;
        }
        else if (hr == 12) {
          ampm = true;
        }
        else if (hr == 0) {
          hr = 12;
        }
        sprintf(timeStr, "%02d:%02d:%02d", hr, now.minute(), now.second());
        debugger.println("RTC Updated:");
        debugger.println(dateStr);
        debugger.print(timeStr);
        if (ampm) {
          debugger.println("pm");
        }
        else {
          debugger.println("am");
        }
      }
    }
  }
}

/**
 * Toggle Lamp
 */
void toggleLamp() {
  if (lampState) {
    digitalWrite(lampRelay, LOW);
    if (Debugging) {
      debugger.println("Interior light off.");
    }
    if (wifiConnected) {
      mqtt.publish(pStatus, "light|off", 2, 0);
    }
  }
  else {
    digitalWrite(lampRelay, HIGH);
    if (Debugging) {
      debugger.println("Interior light on.");
    }
    if (wifiConnected) {
      mqtt.publish(pStatus, "light|on", 2, 0);
    }
  }
  lampState = !lampState;
}

/**
 * Toggle Heater
 */
void toggleHeater() {
  if (heaterState) {
    digitalWrite(heaterRelay, LOW);
    if (Debugging) {
      debugger.println("Water heater off.");
    }
    if (wifiConnected) {
      mqtt.publish(pStatus, "water heater|off", 2, 0);
    }
  }
  else {
    digitalWrite(heaterRelay, HIGH);
    if (Debugging) {
      debugger.println("Water heater on.");
    }
    if (wifiConnected) {
      mqtt.publish(pStatus, "water heater|on", 2, 0);
    }
  }
  heaterState = !heaterState;
}

/**
 * Toggle Fan
 */
void toggleFan() {
  if (fanState) {
    digitalWrite(fan, LOW);
    digitalWrite(fanEnable, HIGH);
    if (Debugging) {
      debugger.println("Fan off.");
    }
    if (wifiConnected) {
      mqtt.publish(pStatus, "fan|off", 2, 0);
    }
  }
  else {
    digitalWrite(fan, HIGH);
    digitalWrite(fanEnable, HIGH);
    if (Debugging) {
      debugger.println("Fan on.");
    }
    if (wifiConnected) {
      mqtt.publish(pStatus, "fan|on", 2, 0);
    }
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
  ds.write(0x44, 1); // Start conversation, with parasite power on at the end

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

/**
 * Write current temp, brightness level, and door status to LCD screen.
 */
void updateLCD() {
  int sun = brightness;
  float tmpF = tempF;
  boolean lStat = lampState;
  boolean fStat = fanState;
  boolean hStat = heaterState;
  String dStat = doorState;
  DateTime now = RTC.now();

  if ((unsigned long)millis() - lastLCDChange > lcdChange) {
    lcdPage++;
    if (lcdPage > 3) {
      lcdPage = 0;
    }
    lastLCDChange = millis();
    lcd.clear();
  }

  if (lcdPage == 0) {
    char tempStr[5];
    lcd.setCursor(0, 0);
    lcd.print("TEMP  SUN  DOOR ");
    lcd.setCursor(0, 1);
    lcd.print(dtostrf(tmpF, 4, 1, tempStr));
    lcd.print("F");
    lcd.setCursor(6, 1);
    lcd.print(sun);
    lcd.print("%");
    lcd.setCursor(11, 1);
    if (dStat == "open") {
      lcd.print("Open");
    }
    else if (dStat == "opening" || dStat == "closing") {
      lcd.print("Move");
    }
    else {
      lcd.print("Shut");
    }
  }
  else if (lcdPage == 1) {
    lcd.setCursor(0, 0);
    lcd.print("LAMP  FAN  HEAT");
    lcd.setCursor(0, 1);
    if (lStat) {
      lcd.print("On   ");
    }
    else {
      lcd.print("Off  ");
    }
    if (fStat) {
      lcd.setCursor(6, 1);
      lcd.print("On   ");
    }
    else {
      lcd.setCursor(6, 1);
      lcd.print("Off ");
    }
    if (hStat) {
      lcd.setCursor(11, 1);
      lcd.print("On   ");
    }
    else {
      lcd.setCursor(11, 1);
      lcd.print("Off  ");
    }
  }
  else if (lcdPage == 2) {
    char dateStr[10];
    char timeStr[8];
    sprintf(dateStr, "%02d/%02d/%04d", now.month(), now.day(), now.year());
    int hr = now.hour();
    boolean ampm = false;
    if (hr > 12) {
      hr = hr - 12;
      ampm = true;
    }
    else if (hr == 12) {
      ampm = true;
    }
    else if (hr == 0) {
      hr = 12;
    }
    sprintf(timeStr, "%02d:%02d:%02d", hr, now.minute(), now.second());
    lcd.setCursor(3, 0);
    lcd.print(dateStr);
    lcd.setCursor(3, 1);
    lcd.print(timeStr);
    if (ampm) {
      lcd.print("pm");
    }
    else {
      lcd.print("am");
    }
  }
  else {
    int sec = now.unixtime() - bootTime;
    int day = floor(sec / 86400);
    sec = sec % 86400;
    int hour = floor(sec / 3600);
    sec = sec % 3600;
    int min = floor(sec / 60);
    sec = sec % 60;

    char uTime[16];
    sprintf(uTime, "%03dd %02dh %02dm %02ds", day, hour, min, sec);
    lcd.setCursor(5, 0);
    lcd.print("UPTIME");
    lcd.setCursor(0, 1);
    lcd.print(uTime);
  }
}

/**
 * Allow writing misc messages to the LCD screen.
 */
void lcdWrite(char *firstLine = "", char *secondLine = "") {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(firstLine);
  lcd.setCursor(0, 1);
  lcd.print(secondLine);
  if (Debugging) {
    debugger.println("Output to LCD:");
    debugger.println(firstLine);
    debugger.println(secondLine);
  }
}

/**
 * Handle door LEDs
 */
void doorLEDs() {
  // When door is closed, closed LED should be lit
  if (doorState == "closed") {
    digitalWrite(doorClosedLED, HIGH);
    digitalWrite(doorOpenLED, LOW);
  }
  // When door is open, open LED should be lit
  else if (doorState == "open") {
    digitalWrite(doorClosedLED, LOW);
    digitalWrite(doorOpenLED, HIGH);
  }
  // If door is opening/closing (or stuck?) blink LEDs back and forth
  else {
    if ((unsigned long)(millis() - lastLEDBlink) > ledBlinkRate) {
      digitalWrite(doorClosedLED, !digitalRead(doorClosedLED));
      digitalWrite(doorOpenLED, !digitalRead(doorOpenLED));
      lastLEDBlink = millis();
    }
  }
}

/**
 * Handle movement of the door
 */
void doorMove() {
  debounceDoor();
  doorStatePrev = doorState;
  if (doorState == "closed" || doorState == "closing") {
    if (doorBottomState != 0) {
      // Door isn't closed, run motor until it is.
      digitalWrite(doorClose, HIGH);
      digitalWrite(doorOpen, LOW);
      digitalWrite(doorEnable, HIGH);
    }
    else {
      // Door is closed, stop motor
      digitalWrite(doorClose, LOW);
      digitalWrite(doorOpen, LOW);
      digitalWrite(doorEnable, HIGH);
      doorState = "closed";
      if (doorStatePrev != doorState && wifiConnected) {
        mqtt.publish("coop/status", "door|closed", 2, 0);
      }
    }
  }
  else {
    if (doorTopState != 0) {
      // Door isn't open, run motor until it is.
      digitalWrite(doorClose, LOW);
      digitalWrite(doorOpen, HIGH);
      digitalWrite(doorEnable, HIGH);
    }
    else {
      // Door is open, stop motor.
      digitalWrite(doorClose, LOW);
      digitalWrite(doorOpen, LOW);
      digitalWrite(doorEnable, HIGH);
      doorState = "open";
      if (doorStatePrev != doorState && wifiConnected) {
        mqtt.publish("coop/status", "door|open", 2, 0);
      }
    }
  }
}

/**
 * Read current sensor data
 */
void readSensors() {
  // Fetch temp and convert to farenheit
  tempC = getTemp();
  tempF = ((tempC * 9.0) / 5.0) + 32;
  // Occasionally get false readings, lets try to prevent that....
  while (tempF > 140) {
    tempC = getTemp();
    tempF = ((tempC * 9.0) / 5.0) + 32;
  }

  if (lastLightRead == 0 || (unsigned long)millis() - lastLightRead > lightReadRate) {
    // Read light sensor and convert to brightness percentage
    brightness = analogRead(lightSense);
    brightness = map(brightness, 0, 1023, 0, 100);  // Remap value to a 0-100 scale
    brightness = constrain(brightness, 0, 100);     // constrain value to 0-100 scale
    lastLightRead = millis();
  }

  // Adjust lcd brightness based on light level
  if (brightness <= 10) {
    analogWrite(lcd_15, 25);
  }
  else if (brightness > 10 && brightness <= 20) {
    analogWrite(lcd_15, 50);
  }
  else if (brightness > 20 && brightness <= 30) {
    analogWrite(lcd_15, 75);
  }
  else if (brightness > 30 && brightness <= 40) {
    analogWrite(lcd_15, 100);
  }
  else if (brightness > 40 && brightness <= 50) {
    analogWrite(lcd_15, 125);
  }
  else if (brightness > 50 && brightness <= 60) {
    analogWrite(lcd_15, 150);
  }
  else if (brightness > 60 && brightness <= 70) {
    analogWrite(lcd_15, 175);
  }
  else if (brightness > 70 && brightness <= 80) {
    analogWrite(lcd_15, 200);
  }
  else if (brightness > 80 && brightness <= 90) {
    analogWrite(lcd_15, 225);
  }
  else {
    analogWrite(lcd_15, 250);
  }
}

/**
 * Respond to updated sensor data.
 */
void handleSensorReadings() {
  // Temperature based reactions
  // ---------------------------

  // Ensure fan is on if temperature is at or above fan threshold.
  if (tempF > fanThreshold) {
    if (!fanState) {
      if (Debugging) {
        debugger.println("Turning on fan.");
      }
      toggleFan();
    }
  }
  // Ensure fan is off if temperature is below fan threshold.
  else {
    if (fanState) {
      if (Debugging) {
        debugger.println("Turning off fan.");
      }
      toggleFan();
    }
  }

  // Ensure water heater is on if temperature is at or below heater threshold.
  if (tempF < heaterThreshold) {
    if (!heaterState) {
      if (Debugging) {
        debugger.println("Turning on water heater.");
      }
      toggleHeater();
    }
  }
  // Ensure water heater is off if temperature is above heater threshold.
  else {
    if (heaterState) {
      if (Debugging) {
        debugger.println("Turning off water heater.");
      }
      toggleHeater();
    }
  }

  // Light based reactions
  // ---------------------

  // Fetch current time from RTC
  DateTime now = RTC.now();
  // If nightlock is enabled, and we are within the designated time period, simply
  // ensure interior light is off and door is closed.
  if (nightLock && (now.hour() >= nightLockStart || now.hour() <= nightLockEnd)) {
    // Turn off interior light if it is on
    if (lampState) {
      if (Debugging) {
        debugger.println("NIGHTLOCK ENABLED: Turning off interior light.");
      }
      toggleLamp();
    }
    // Close door if it is open
    if (doorState == "open") {
      if (Debugging) {
        debugger.println("NIGHTLOCK ENABLED: Closing door.");
      }
      doorState = "closing";
      if (wifiConnected) {
        mqtt.publish(pStatus, "door|closing", 2, 0);
      }
    }
  }
  // Otherwise, handle brightness level based reactions

  // NOTE: We need a bit of a gap between these thresholds to prevent
  // bouncing if light readings fluctuate by a percentage or two.
  else {
    // If brightness level is 4-10%, ensure interior light is on
    if (brightness >= 4 && brightness <= 10) {
      if (!lampState) {
        if (Debugging) {
          debugger.println("Turning on interior light.");
        }
        toggleLamp();
      }
    }
    // otherwise if brightness is less than 3% or greater than 11% ensure interior light is off.
    else if (brightness < 3 || brightness > 11) {
      if (lampState) {
        if (Debugging) {
          debugger.println("Turning off interior light.");
        }
        toggleLamp();
      }
    }

    // Open door when brightness level is greater than 5%
    if (brightness >= 5) {
      if (doorState == "closed") {
        if (Debugging) {
          debugger.println("Opening door.");
        }
        doorState = "opening";
        if (wifiConnected) {
          mqtt.publish(pStatus, "door|opening", 2, 0);
        }
      }
    }
    // Otherwise, close door when light level falls below 2%.
    else if (brightness < 2) {
      if (doorState == "open") {
        if (Debugging) {
          debugger.println("Closing door.");
        }
        doorState = "closing";
        if (wifiConnected) {
          mqtt.publish(pStatus, "door|closing", 2, 0);
        }
      }
    }
  }
}

/**
 * Door switch debouncer
 */
void debounceDoor() {
  doorTopVal     = digitalRead(doorTop);
  doorBottomVal  = digitalRead(doorBottom);
  doorTopPrev    = doorTopState;
  doorBottomPrev = doorBottomState;

  if ((unsigned long)(millis() - lastDebounce) > debounceWait) {
    doorTopVal2    = digitalRead(doorTop);
    doorBottomVal2 = digitalRead(doorBottom);
    if (doorTopVal == doorTopVal2) {
      if (doorTopVal != doorTopState) {
        doorTopState = doorTopVal;
        if (doorTopState == 0) {
          doorState = "open";
          if (doorTopPrev != doorTopState) {
            if (Debugging) {
              debugger.println("Door open.");
            }
            if (wifiConnected) {
              mqtt.publish(pStatus, "door|open", 2, 0);
            }
          }
        }
      }
    }
    if (doorBottomVal == doorBottomVal2) {
      if (doorBottomVal != doorBottomState) {
        doorBottomState = doorBottomVal;
        if (doorBottomState == 0) {
          doorState = "closed";
          if (doorBottomPrev != doorBottomState) {
            if (Debugging) {
              debugger.println("Door closed.");
            }
            if (wifiConnected) {
              mqtt.publish(pStatus, "door|closed", 2, 0);
            }
          }
        }
      }
    }
    lastDebounce = millis();
  }
}

/**
 * Publish Temp and Light Readings.
 */
void publishReadings() {
  char briStr[3];
  mqtt.publish(pLight, dtostrf(brightness, 3, 0, briStr), 2, 0);

  char tempStr[5];
  mqtt.publish(pTemp, dtostrf(tempF, 5, 1, tempStr), 2, 0);

  // Since the webapp likes to get confused, lets remind them we're
  // online every time we update brightness and temp readings...
  mqtt.publish("client/online", "1", 2, 0);
}

/**
 * Initialization on startup.
 */
void setup() {
  debugger.begin(19200);
  if (Debugging) {
    debugger.println("Initialising...");
  }
  pinMode(lcd_15, OUTPUT);
  analogWrite(lcd_15, 150);

  RTC.begin();
  // Set the boot timestamp.
  DateTime now = RTC.now();
  bootTime = now.unixtime();

  lcd.begin(16, 2);
  lcdWrite("Initializing...");

  espPort.begin(19200);
  esp.enable();
  delay(500);
  esp.reset();
  delay(500);
  while (!esp.ready());

  if (Debugging) {
    debugger.println("ARDUINO: Setup MQTT client");
  }

  if (!mqtt.begin(mClientID, mUsername, mPassword, 20, 1)) { // client_id, username, password, keepalive time, cleansession boolean
    if (Debugging) {
      debugger.println("ARDUINO: Failed to setup MQTT");
    }
    lcdWrite("MQTT Connect", "Failure");
    while (1);
  }
  mqtt.lwt("client/online", "0");

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
  pinMode(doorEnable, OUTPUT);
  pinMode(doorOpenLED, OUTPUT);
  pinMode(doorClosedLED, OUTPUT);
  pinMode(doorTop, INPUT);
  pinMode(doorBottom, INPUT);
  pinMode(fan, OUTPUT);
  pinMode(fanEnable, OUTPUT);

  // Pin defaults
  digitalWrite(lampRelay, LOW);
  digitalWrite(heaterRelay, LOW);
  digitalWrite(doorOpen, LOW);
  digitalWrite(doorClose, LOW);
  digitalWrite(doorEnable, HIGH);
  digitalWrite(doorOpenLED, LOW);
  digitalWrite(doorClosedLED, HIGH);
  digitalWrite(doorTop, HIGH);    // Enable resistor
  digitalWrite(doorBottom, HIGH); // Enable resistor
  digitalWrite(fan, LOW);
  digitalWrite(fanEnable, HIGH);

  lcdWrite("System Ready");
}

/**
 * Main program loop
 */
void loop() {
  esp.process();
  // 5 second pause on initial startup to let devices settle, wifi connect, etc.
  if (millis() > 5000) {
    if (lastPublish == 0) {
      // Send default values to ensure states are in sync at other end of MQTT connection
      mqtt.publish("coop/status", "fan|off", 2, 0);
      mqtt.publish("coop/status", "light|off", 2, 0);
      mqtt.publish("coop/status", "water heater|off", 2, 0);
      if (digitalRead(doorBottom == 0)) {
        doorState = "closed";
        mqtt.publish("coop/status", "door|closed", 2, 0);
      }
      else if (digitalRead(doorTop == 0)) {
        doorState = "open";
        mqtt.publish("coop/status", "door|open", 2, 0);
      }
    }

    // Read new data from sensors
    readSensors();

    if (remoteLockStart == 0 ||
        (unsigned long)(millis() - remoteLockStart) > remoteOverride) {
      // Respond ot sensor data
      handleSensorReadings();
    }

    // Only publish new sensor data if it's been long enough since the last reading.
    if (lastPublish == 0 || (unsigned long)(millis() - lastPublish) > publishInterval) {
      if (wifiConnected) {
        publishReadings();
      }
      lastPublish = millis();
    }

    // Move the door as needed
    doorMove();

    // Update door LEDs as needed
    doorLEDs();

    // Update the LCD display
    updateLCD();
  }
}
