/*
   Sonoff-Touch-TeHu
   Firmware to use a Sonoff TH10/16 device as a remote relay and temp/humidity sensor with MQTT and WiFi capabilities.

   Supports OTA update
   Mats Ekberg (C) 2017 GNU GPL v3

   Supports sensors (has to be recompiled if change):
     DHT 11
     DHT 21  (AM2301)  This is the default sensor type
     DHT 22  (AM2302)

   Runs on this harware:
   https://www.itead.cc/wiki/Sonoff_TH_10/16

   Uses these libraries:
   https://github.com/adafruit/DHT-sensor-library
   https://github.com/adafruit/Adafruit_Sensor

   Flashed via USB/OTA in Arduino IDE with these parameters:
   Board:       Generic ESP8266 Module
   Flash size:  1M (64K SPIFFS)

*/

#include <ESP8266WiFi.h>
// must increase max packet size to > 500
#include <PubSubClient.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include "DHT.h"

#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager

#define LONG_PRESS_MS 1000
#define SHORT_PRESS_MS 100
#define CONFIG_WIFI_PRESS_MS 5000
#define CONFIG_TOUCHES_COUNT 3
#define MQTT_CHECK_MS 15000

//#define F(x) (x)

////// config values
//define your default values here, if there are different values in config.json, they are overwritten.
char mqtt_server[40] = "10.0.1.50";
char mqtt_port[6] = "1883";
char mqtt_user[24] = "";
char mqtt_pass[24] = "";
char unit_id[16] = "thsw0";
char group_id[16] = "thswgrp0";

// The extra parameters to be configured (can be either global or just in the setup)
// After connecting, parameter.getValue() will get you the configured value
// id/name placeholder/prompt default length
WiFiManagerParameter custom_mqtt_server = NULL;
WiFiManagerParameter custom_mqtt_port = NULL;
WiFiManagerParameter custom_mqtt_user = NULL;
WiFiManagerParameter custom_mqtt_pass = NULL;
WiFiManagerParameter custom_unit_id = NULL;
WiFiManagerParameter custom_group_id = NULL;


#define OTA_PASS "UPDATE_PW"
#define OTA_PORT 8266

#define BUTTON_PIN 0
#define RELAY_PIN 12
#define LED_PIN 13
#define DHTPIN 14

//#define DHTTYPE DHT11   // DHT 11
//#define DHTTYPE DHT22   // DHT 22  (AM2302), AM2321
#define DHTTYPE DHT21   // DHT 21 (AM2301)

volatile int desiredRelayState = 0;
volatile int relayState = 0;
volatile unsigned long millisSinceChange = 0;
volatile int noOfConfigTouches = 0;

volatile boolean sendGroupEventTopic = false;
volatile boolean configWifi = false;
volatile boolean sendEvent = true;
boolean sendStatus = true;
boolean sendPong = false;
unsigned long uptime = 0;

float humid = NAN;
float temp = NAN;
boolean sendSensors = false;

unsigned long lastMQTTCheck = -MQTT_CHECK_MS; //This will force an immediate check on init.

WiFiClient espClient;
PubSubClient client(espClient);

DHT dht(DHTPIN, DHTTYPE);

bool printedWifiToSerial = false;

// these are defined after wifi connect and parameters are set (in setup())
String eventTopic;       // published when the switch is touched
String groupEventTopic;  // published when the switch was long touched
String statusTopic;      // published when the relay changes state wo switch touch
String sensorTempTopic;  // publish temp sensor value
String sensorHumidTopic; // publish humidity sensor value
String pongStatusTopic;  // publish node status topic
String pongMetaTopic;    // publish node meta topic
String pingTopic;        // subscribe to nodes ping topic
String actionTopic;      // subscribed to to change relay status
String groupActionTopic; // subscribed to to change relay status based on groupid

//
// Connect to MQTT broker
// Subscribe to topics, flash LED etc
//
void checkMQTTConnection() {
  Serial.print(F("MQTT conn? "));
  if (client.connected()) Serial.println(F("OK"));
  else {
    if (WiFi.status() == WL_CONNECTED) {
      //Wifi connected, attempt to connect to server
      Serial.print(F("new connection: "));
      if (client.connect(custom_unit_id.getValue(), custom_mqtt_user.getValue(), custom_mqtt_pass.getValue())) {
        Serial.println(F("connected"));
        client.subscribe(pingTopic.c_str());
        client.subscribe(actionTopic.c_str());
        client.subscribe(groupActionTopic.c_str());
        client.publish(pongStatusTopic.c_str(), "connected");
      } else {
        Serial.print(F("failed, rc="));
        Serial.println(client.state());
      }
    }
    else {
      //Wifi isn't connected, so no point in trying now.
      Serial.println(F(" Not connected to WiFI AP, abandoned connect."));
    }
  }
  //Set the status LED to ON if we are connected to the MQTT server
  if (client.connected())
    digitalWrite(LED_PIN, LOW);
  else
    digitalWrite(LED_PIN, HIGH);
}


//
// MQTT message arrived, decode
// Ok payload: 1/on, 0/off, X/toggle, S/status
//
void MQTTcallback(char* topic, byte* payload, unsigned int length) {
  Serial.print(F("MQTT sub: "));
  Serial.println(topic);

  if (!strcmp(topic, actionTopic.c_str()) || !strcmp(topic, groupActionTopic.c_str()))
  {
    if ((char)payload[0] == '1' || ! strncasecmp_P((char *)payload, "on", length))
    {
      desiredRelayState = 1;
    }
    else if ((char)payload[0] == '0' || ! strncasecmp_P((char *)payload, "off", length))
    {
      desiredRelayState = 0;
    }
    else if ((char)payload[0] == 'X' || ! strncasecmp_P((char *)payload, "toggle", length))
    {
      desiredRelayState = !desiredRelayState;
    }
    else if ((char)payload[0] == 'S' || ! strncasecmp_P((char *)payload, "status", length))
    {
      sendStatus = true;
    }
  }
  if (!strcmp(topic, pingTopic.c_str()))
  {
    sendPong = true;
  }
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


//
// This routine handles state changes and MQTT publishing
//
void handleStatusChange() {

  //Relay state is updated via the interrupt *OR* the MQTT callback.
  if (relayState != desiredRelayState) {
    Serial.print(F("Chg state to "));
    Serial.println(desiredRelayState);

    digitalWrite(RELAY_PIN, desiredRelayState);
    relayState = desiredRelayState;
    sendStatus = true;
  }

  if (sendPong)
  {
    Serial.print(F("MQTT pub: "));
    String meta = getDeviceMeta();
    Serial.print(meta);
    Serial.print(F(" to "));    
    Serial.println(pongMetaTopic);
    client.publish(pongMetaTopic.c_str(), meta.c_str());
    sendPong = false;
  }

  // publish event if touched
  if (sendEvent) {
    const char* payload = (relayState == 0) ? "0" : "1";
    Serial.print(F("MQTT pub: "));
    Serial.print(payload);
    Serial.print(F(" to "));
    if (sendGroupEventTopic) {
      Serial.println(groupEventTopic);
      client.publish(groupEventTopic.c_str(), payload);
    } else {
      Serial.println(eventTopic);
      client.publish(eventTopic.c_str(), payload);
    }
    sendEvent = false;
  }

  // publish state when requested to do so
  if (sendStatus) {
    const char* payload = (relayState == 0) ? "0" : "1";
    Serial.print(F("MQTT pub: "));
    Serial.print(payload);
    Serial.print(F(" to "));
    Serial.println(statusTopic);
    client.publish(statusTopic.c_str(), payload);
    sendStatus = false;
  }

  if (sendSensors)
  {
    if (!isnan(temp))
    {
      Serial.print(F("MQTT pub: "));
      Serial.print(temp);
      Serial.print(F(" to "));
      Serial.println(sensorTempTopic);
      client.publish(sensorTempTopic.c_str(), String(temp).c_str());
    }
    else
    {
      Serial.println(F("No temp data"));
    }
    if (!isnan(humid))
    {
      Serial.print(F("MQTT pub: "));
      Serial.print(humid);
      Serial.print(F(" to "));
      Serial.println(sensorHumidTopic);
      client.publish(sensorHumidTopic.c_str(), String(humid).c_str());
    }
    else
    {
      Serial.println(F("No humidity data"));
    }
    sendSensors = false;
  }
}


//
////////// SETUP //////////
//
void setup() {
  Serial.begin(115200);
  Serial.println(F("Initialising"));
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  digitalWrite(LED_PIN, HIGH); //LED off.

  initWifiManager(false);

  // after wifi and parameters are configured, create publish topics
  eventTopic = String(F("event/")) + custom_unit_id.getValue() + String(F("/switch"));
  groupEventTopic = String(F("event/")) + custom_group_id.getValue() + String(F("/switch"));
  statusTopic = String(F("status/")) + custom_unit_id.getValue() + String(F("/relay"));
  sensorTempTopic = String(F("sensor/")) + custom_unit_id.getValue() + String(F("/temp"));
  sensorHumidTopic = String(F("sensor/")) + custom_unit_id.getValue() + String(F("/humid"));
  pongStatusTopic = String(F("pong/")) + custom_unit_id.getValue() + String(F("/status"));
  pongMetaTopic = String(F("pong/")) + custom_unit_id.getValue() + String(F("/meta"));
  // and subscribe topic
  actionTopic = String(F("action/")) + custom_unit_id.getValue() + String(F("/relay"));
  groupActionTopic = String(F("action/")) + custom_group_id.getValue() + String(F("/relay"));
  pingTopic = String(F("ping/nodes"));

  client.setServer(custom_mqtt_server.getValue(), atoi(custom_mqtt_port.getValue()));
  client.setCallback(MQTTcallback);

  // OTA setup
  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setHostname(custom_unit_id.getValue());
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.begin();

  dht.begin();

  // Enable interrupt for button press
  Serial.println(F("Enabling touch switch interrupt"));
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonChangeCallback, CHANGE);
}


//
////////// LOOP //////////
//
void loop() {
  // If we haven't printed WiFi details to Serial port yet, and WiFi now connected,
  // do so now. (just the once)
  if (!printedWifiToSerial && WiFi.status() == WL_CONNECTED) {
    Serial.println(F("WiFi connected"));
    Serial.println(F("IP address: "));
    Serial.println(WiFi.localIP());
    printedWifiToSerial = true;
  }

  // Check MQTT connection
  if (millis() - lastMQTTCheck >= MQTT_CHECK_MS) {
    uptime += MQTT_CHECK_MS / 1000;
    checkMQTTConnection();
    // Sensor readings may also be up to 2 seconds 'old' (its a very slow sensor)
    humid = dht.readHumidity();
    // Read temperature as Celsius (the default)
    temp = dht.readTemperature();
    lastMQTTCheck = millis();
    sendSensors = true;
  }

  // Handle any pending MQTT messages
  client.loop();

  // Handle any pending OTA SW updates
  ArduinoOTA.handle();

  // Handle any state change and MQTT publishing
  handleStatusChange();

  // Handle looong touch to reconfigure all parameters
  if (configWifi) {
    espClient.stop();
    delay(1000);
    initWifiManager(true);
  }

  delay(50);
}
