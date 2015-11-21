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
const int rtcSCL        = 21; // Real Time Clock SCL (i2c arduino mega)

// LCD Pins
const int lcd_4  = 24; // LCD RS
const int lcd_5  = 25; // LCD R/W
const int lcd_6  = 26; // LCD Enable
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
#define mClientID "coop-duino"
#define mUsername "chicken_coop"
#define mPassword "secret"

// MQTT Subscription Channels
#define sTime "time/beacon"
#define sRemote "coop/remotetrigger"

// MQTT Publish Channels
#define pTemp "coop/temperature"
#define pLight "coop/brightness"
#define pStatus "coop/status"

// Misc Settings
const unsigned long millisPerDay    = 86400000; // Milliseconds per day
const unsigned long debounceWait    =       25; // Debounce timer (25 ms)
const unsigned long ledBlinkRate    =      250; // Delay between LED blinks (1/4 sec)
const unsigned long remoteOverride  =   600000; // Length of time to lockout readings. (10 min)
const unsigned long readingInterval =    60000; // How often to take sensor readings. (1 min)
const int           fanThreshold    =       78; // When temperature (Farenheit) exceeds this threshold, turn on fan
const int           heaterThreshold =       36; // When temperature (Farenheit) is below this threshold, turn on water heater

// Night time lockout to prevent reaction to light sensor readings if an exterior light source causes
// a reading otherwise bright enough to activate the interior light and/or door.
const boolean       nightLock      =     true; // Enable night time lockout
const int           nightLockStart =       22; // Hour (in 24hr time) to initiate night time lockout (10pm)
const int           nightLockEnd   =        4; // Hour (in 24hr time) to end night time lockout (4am)

// Runtime variables
unsigned long lastReading        = 0;
unsigned long lastDebounce       = 0;
unsigned long lastRTCSync        = 0;
unsigned long lastLEDBlink       = 0;
unsigned long remoteLockStart    = 0;
String        doorState          = "open"; // Values will be one of: closed, closing, open, opening
boolean       heaterState        = false;
boolean       lampState          = false;
boolean       fanState           = false;
float         tempC              = 0;
float         tempF              = 0;
int           brightness         = 0;
int           doorTopVal         = 0;
int           doorTopVal2        = 0;
int           doorTopState       = 0;
int           doorTopPrev        = 0;
int           doorBottomVal      = 0;
int           doorBottomVal2     = 0;
int           doorBottomState    = 0;
int           doorBottomPrev     = 0;

RTC_DS1307 RTC;
OneWire ds(tempSense);
LiquidCrystal lcd(lcd_4, lcd_5, lcd_6, lcd_11, lcd_12, lcd_13, lcd_14);

ESP esp(&espPort, &debugger, chpdPin);
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
  if (Debugging) {
    if (topic != "time/beacon") {
      debugger.print("MQTT Data published to channel '");
      debugger.print(topic);
      debugger.print("': ");
      debugger.println(data);
    }
  }
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
  if (Debugging) {
    debugger.print("MQTT Data Received on channel '");
    debugger.print(topic);
    debugger.print("': ");
    debugger.println(data);
  }

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

  // Sync RTC to time beacon once/day
  if (topic == "time/beacon") {
    if (lastRTCSync == 0 || ((unsigned long)(millis() - lastRTCSync) > millisPerDay)) {
      RTC.adjust(strtoul(data.c_str(), NULL, 0));
      lastRTCSync = millis();
      if (Debugging) {
        DateTime now = RTC.now();
        debugger.println("RTC Updated:");
        debugger.print(now.month(), DEC);
        debugger.print("/");
        debugger.print(now.day(), DEC);
        debugger.print("/");
        debugger.println(now.year(), DEC);
        debugger.print(now.hour(), DEC);
        debugger.print(":");
        debugger.print(now.minute(), DEC);
        debugger.print(":");
        debugger.println(now.second(), DEC);
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
    digitalWrite(fanSpeed, LOW);
    if (Debugging) {
      debugger.println("Fan off.");
    }
    if (wifiConnected) {
      mqtt.publish(pStatus, "fan|off", 2, 0);
    }
  }
  else {
    digitalWrite(fan, HIGH);
    digitalWrite(fanSpeed, HIGH);
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

/**
 * Write current temp, brightness level, and door status to LCD screen.
 */
void updateLCD() {
  char tempStr[4];
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print(dtostrf(tempF, 4, 1, tempStr));
  lcd.print("F | L ");
  lcd.print(brightness);
  lcd.print("%");
  lcd.setCursor(0,1);
  lcd.print("Door: ");
  lcd.print(doorState);
}

/**
 * Allow writing misc messages to the LCD screen.
 */
void lcdWrite(char *firstLine = "", char *secondLine = "") {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print(firstLine);
  lcd.setCursor(0,1);
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
   if (doorState == "closed" || doorState == "closing") {
     if (doorBottomState != 0) {
       // Door isn't closed, run motor until it is.
       digitalWrite(doorClose, HIGH);
       digitalWrite(doorOpen, LOW);
       digitalWrite(doorSpeed, HIGH);
     }
     else {
       // Door is closed, stop motor
       digitalWrite(doorClose, LOW);
       digitalWrite(doorOpen, LOW);
       digitalWrite(doorSpeed, LOW);
     }
   }
   else {
     if (doorTopState != 0) {
       // Door isn't open, run motor until it is.
       digitalWrite(doorClose, LOW);
       digitalWrite(doorOpen, HIGH);
       digitalWrite(doorSpeed, HIGH);
     }
     else {
       // Door is open, stop motor.
       digitalWrite(doorClose, LOW);
       digitalWrite(doorOpen, LOW);
       digitalWrite(doorSpeed, LOW);
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
  char tempStr[5];
  if (wifiConnected) {
    mqtt.publish(pTemp, dtostrf(tempF, 5, 1, tempStr), 2, 0);
  }

  // Read light sensor and convert to brightness percentage
  brightness = analogRead(lightSense);
  brightness = map(brightness, 300, 150, 0, 100);  // Remap value to a 0-100 scale
  brightness = constrain(brightness, 0, 100);     // constrain value to 0-100 scale
  char briStr[3];
  if (wifiConnected) {
    mqtt.publish(pLight, dtostrf(brightness, 3, 0, briStr), 2, 0);
  }
  lastReading = millis();
}

/**
 * Respond to updated sensor data.
 */
void handleSensorReadings() {
  // Temperature based reactions
  // ---------------------------

  // Ensure fan is on if temperature is at or above fan threshold.
  if (tempF >= fanThreshold) {
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
  if (tempF <= heaterThreshold) {
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
  if (nightLock && (now.hour() > nightLockStart || now.hour() < nightLockEnd)) {
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
  else {
    // If brightness level is between 10 and 20%, ensure interior light is on
    if (brightness > 10 && brightness < 20) {
      if (!lampState) {
        if (Debugging) {
          debugger.println("Turning on interior light.");
        }
        toggleLamp();
      }
    }
    // otherwise ensure interior light is off.
    else {
      if (lampState) {
        if (Debugging) {
          debugger.println("Turning off interior light.");
        }
        toggleLamp();
      }
    }
    // Open door when brightness level is over 15%
    if (brightness > 15) {
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
    // Otherwise, close door when light level falls to 15% or below.
    else {
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
 * Initialization on startup.
 */
void setup() {
  debugger.begin(19200);
  if (Debugging) {
    debugger.println("Initialising...");
  }
  // Init LCD
  pinMode(lcd_bl, OUTPUT);
  digitalWrite(lcd_bl, HIGH);
  RTC.begin();
  lcd.begin(16,2);
  lcdWrite("Initializing...");

  espPort.begin(19200);
  lcdWrite("Initializing...", "WIFI Config");
  esp.enable();
  delay(500);
  esp.reset();
  delay(500);
  while(!esp.ready());
  
  if (Debugging) {
    debugger.println("ARDUINO: Setup MQTT client");
  }
  lcdWrite("Initializing...", "MQTT Config");
  if (!mqtt.begin(mClientID, mUsername, mPassword, 20, 1)) { // client_id, username, password, keepalive time, cleansession boolean
    if (Debugging) {
      debugger.println("ARDUINO: Failed to setup MQTT");
    }
    lcdWrite("MQTT Connect", "Failure");
    while(1);
  }
  mqtt.lwt("/lwt", "Offline");

  mqtt.connectedCb.attach(&mqttConnected);
  mqtt.disconnectedCb.attach(&mqttDisconnected);
  mqtt.publishedCb.attach(&mqttPublished);
  mqtt.dataCb.attach(&mqttData);

  if (Debugging) {
    debugger.println("ARDUINO: Setup WIFI");
  }
  esp.wifiCb.attach(&wifiCb);
  esp.wifiConnect(wHost, wPass);

  lcdWrite("Initializing...", "WIFI Connecting");

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

  lcdWrite("System Ready");
}

/**
 * Main program loop
 */
void loop() {
  esp.process();

    // Only fetch new sensor data if it's been long enough since the last reading.
    if (lastReading == 0 ||
        ((unsigned long)(millis() - remoteLockStart) > remoteOverride &&
        (unsigned long)(millis() - lastReading) > readingInterval)) {
      // Read new data from sensors
      readSensors();
  
      // Respond ot sensor data
      handleSensorReadings();
    }
  
    // Move the door as needed
    doorMove();
  
    // Update door LEDs as needed
    doorLEDs();
  
    // Update the LCD display
    updateLCD();

}
