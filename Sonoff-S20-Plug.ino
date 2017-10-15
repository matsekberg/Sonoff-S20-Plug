/*
   Sonoff-S20-Plug
   Firmware to use a Sonoff S20 wallplug device as a remote relay and temp/humidity sensor with MQTT and WiFi capabilities.

   Supports OTA update
   Mats Ekberg (C) 2017 GNU GPL v3

   Runs on this harware:
   http://sonoff.itead.cc/en/products/residential/s20-socket

   Uses these libraries:


   Flashed via USB/OTA in Arduino IDE with these parameters:
   Board:       Generic ESP8266 Module
   Flash size:  1M (64K SPIFFS)

*/
// DO EDIT
#define CONFIG_VERSION "S20P001"
#define S20
// END - DO EDIT


// DO NOT CHANGE
#include "sensorlibs.h"
#include "support/wifi-manager.h"
#include "support/mqtt-support.h"

#include "topics.h"
#include "support/wifi-manager.cpp"
#include "support/mqtt-support.cpp"
// END - DO NOT CHANGE



//
// MQTT message arrived, decode
//
void mqttCallbackHandle(char* topic, byte* payload, unsigned int length) {
  Serial.print(F("MQTT sub: "));
  Serial.println(topic);
}


//
// Handle short touch
//
void shortPress() {
  desiredRelayState = !desiredRelayState; //Toggle relay state.
  sendGroupEventTopic = false;
  sendEvent = true;
  noOfConfigTouches = 0;
}

//
// Handle long touch
//
void longPress() {
  desiredRelayState = !desiredRelayState; //Toggle relay state.
  sendGroupEventTopic = true;
  sendEvent = true;
  noOfConfigTouches = 0;
}

//
// Handle looong config touch
//
void configWifiPress() {
  noOfConfigTouches++;
  if (noOfConfigTouches >= CONFIG_TOUCHES_COUNT)
    configWifi = true;
}


//
// This is executed on touch
//
void buttonChangeCallback() {
  if (digitalRead(0) == 1) {

    // Button has been released, trigger one of the two possible options.
    if (millis() - millisSinceChange > CONFIG_WIFI_PRESS_MS) {
      configWifiPress();
    }
    else if (millis() - millisSinceChange > LONG_PRESS_MS) {
      longPress();
    }
    else if (millis() - millisSinceChange > SHORT_PRESS_MS) {
      shortPress();
    }
    else {
      //Too short to register as a press
    }
  }
  else {
    //Just been pressed - do nothing until released.
  }
  millisSinceChange = millis();
}


DynamicJsonBuffer jsonBuffer(250);


//
// This routine handles state changes and MQTT publishing
//
void handleStatusChange() {

  // publish relay state, pong, event and status messages
  mqttPublish();
}

//
// callback to create custom topics
//
void mqttCallbackCreateTopics() {
  // pointer of topics
  //subscribedTopics[0] = &matrixActionSTopic;
  noSubscribedTopics = 0;
}


//
////////// SETUP //////////
//
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("Initialising"));
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  digitalWrite(LED_PIN, HIGH); //LED off.

  // setup wifi
  wifiSetup(CONFIG_VERSION, false);

  // setup mqtt
  mqttSetup();

}


//
////////// LOOP //////////
//
void loop() {

  // handle wifi
  wifiLoop();

  // handle mqtt
  mqttLoop();

  // Check MQTT connection
  if (millis() - lastMQTTCheck >= MQTT_CHECK_MS) {
    lastMQTTCheck = millis();

    uptime += MQTT_CHECK_MS / 1000;
    mqttCheckConnection();
  }

  // Handle any state change and MQTT publishing
  handleStatusChange();

  delay(50);
}
