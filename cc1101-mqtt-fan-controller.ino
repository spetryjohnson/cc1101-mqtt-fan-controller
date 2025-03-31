/*
    ----------- CC1101 + MQTT fan controller -----------
    Automates dumb ceiling fans using a CC1101 RF transmitter/receiver on an ESP32 to capture RF commands
    and bridges them to MQTT for Home Assistant.

    Tested using a CC1101 "v2" board using a 303.87 MHz Hampton Bay fan, though this should support other
    brands and frequencies as well (as long as the CC1101 supports them).

    The fan used for development has a remote with 4 buttons:
      - POWER
      - FAN SPEED [cycles between Off, Low, Medium, High]
      - LIGHT ON/OFF
      - LIGHT COLOR TEMP [not implemented in this project]

  Each button (except color temp) has a pair of matching MQTT topics. One is a command topic, one is a 
  state topic.

  The static topic represents the current state of the fan (as known by this component).

  The command topic is used to trigger RF commands via MQTT; when a message is received on a command topic,
  the corresponding RF signal is transmitted and new state values are pushed to the state topic(s).

  Changes to the state topics (either initiated here, or by some external system) trigger this component to
  update its internal state to match.
*/

// -------------------------------------------------------------------------------
// OTA Support, for pushing code updates once the device is in place
//
// REMEMBER: Use the "minimal spiffs" partition scheme in Tools -> Partition Scheme
// -------------------------------------------------------------------------------
#include <ArduinoOTA.h>
#include <ESPmDNS.h>

#if defined(NO_OTA)
    const bool OTA_ENABLED = false;
#else
    const bool OTA_ENABLED = true;
#endif

#if defined(OTA_PASSWORD)
    const char OTA_PASSWORD[] = OTA_PASSWORD;
#else
    char OTA_PASSWORD[] = "";
#endif

// -------------------------------------------------------------------------------
// Project includes
// -------------------------------------------------------------------------------
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <RCSwitch.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <TelnetStream.h>     
#include "myconfig.h"

// -------------------------------------------------------------------------------
// CC1101 config
// -------------------------------------------------------------------------------
// Set receive and transmit pin numbers (GDO0 and GDO2), vary by board
#ifdef ESP32 // for esp32! Receiver on GPIO pin 4. Transmit on GPIO pin 2.
  #define RX_PIN 4 
  #define TX_PIN 2
#elif ESP8266  // for esp8266! Receiver on pin 4 = D2. Transmit on pin 5 = D1.
  #define RX_PIN 4
  #define TX_PIN 5
#else // for Arduino! Receiver on interrupt 0 => that is pin #2. Transmit on pin 6.
  #define RX_PIN 0
  #define TX_PIN 6
#endif 

// These seem necessary for the CC1101 V2 board that I found on Amazon
#define SCK_PIN 18
#define SS_PIN 5
#define MISO_PIN 19
#define MOSI_PIN 23

// -------------------------------------------------------------------------------
// Fan config (these may change based on your fan model)
// -------------------------------------------------------------------------------
#define RF_FREQUENCY 303.87         // Used for RX and TX
#define RF_PROTOCOL 11              // Used for TX, and to identify relevant RX commands
#define RF_REPEATS 10               // Used for TX, and to identify relevant RX commands
#define RF_BITLENGTH 24             // Used for TX, and to identify relevant RX commands
#define RF_DELAY 384                // Used for TX, and to identify relevant RX commands

enum FanSpeed {
  SPEED_OFF   = 0,
  SPEED_LOW   = 1,
  SPEED_MED   = 2,
  SPEED_HIGH  = 3
};

enum FanCodes {
  POWER_ON  = 16539774,
  POWER_OFF = 16539773,

  FAN_OFF   = 16539767,
  FAN_LOW   = 16539766,
  FAN_MED   = 16539765,
  FAN_HIGH  = 16539764,

  LIGHT_ON  = 16539762,
  LIGHT_OFF = 16539761
};

// -------------------------------------------------------------------------------
// MQTT config (all add suffixes to base topic set in config file)
// -------------------------------------------------------------------------------
// On boot, publishes value of "online" to this topic
#define STATUS_TOPIC BASE_TOPIC "/status"

// Command topic for the remote's main on/off button. OFF turns off the fan and light. ON turns on light and sets fan to max.
#define MQTT_TOPIC_POWER_SET BASE_TOPIC "/power/set"

// Command and status topics for the fan speed as a binary sensor. OFF turns off the fan motor, ON sets it to low.
#define MQTT_TOPIC_FAN_STATE BASE_TOPIC "/fan/state"  
#define MQTT_TOPIC_FAN_SET BASE_TOPIC "/fan/set"  

// Command and status topics for the fan speed. Values are 0-3 for Off, Low, Med, High
#define MQTT_TOPIC_SPEED_SET BASE_TOPIC "/speed/set"
#define MQTT_TOPIC_SPEED_STATE BASE_TOPIC "/speed/state"

// Command and status topics for the fan light (ON / OFF)
#define MQTT_TOPIC_LIGHT_SET BASE_TOPIC "/light/set"
#define MQTT_TOPIC_LIGHT_STATE BASE_TOPIC "/light/state"

// -------------------------------------------------------------------------------
// Static vars for MQTT client and the RF transmitter
// -------------------------------------------------------------------------------
RCSwitch mySwitch = RCSwitch();
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// -------------------------------------------------------------------------------
// Current fan state. These vars are updated whenever a new MQTT state message is
// received. They are initialized on boot b/c the state messages use "retain=true"
// so that every time the MQTT client connects, it gets the latest state.
// -------------------------------------------------------------------------------
bool lightState;
FanSpeed fanSpeed;    

// -------------------------------------------------------------------------------
// setup()
// -------------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("Starting up");
  delay(3000);

  InitWifi();
  InitCC1101();
  InitMQTT();

  if (OTA_ENABLED) {
    InitOTA();
  }
  
  if (USE_TELNET_STREAM) {
    StartTelnetDebugStream();
  }
}

// -------------------------------------------------------------------------------
// loop()
// -------------------------------------------------------------------------------
void loop() {
  if (OTA_ENABLED) {
    ArduinoOTA.handle();
  }

  if (USE_TELNET_STREAM) {
    HandleTelnetInput();
  }

  if (!mqttClient.connected()) {
    ReconnectMqtt();
  }
  mqttClient.loop();

  if (mySwitch.available()) {
    HandleIncomingRfSignal();
    mySwitch.resetAvailable();
  }
}

// -------------------------------------------------------------------------------
// InitWifi()
// -------------------------------------------------------------------------------
void InitWifi() {
  Serial.print("Attempting to connect to WPA SSID: ");
  Serial.println(WIFI_SSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  bool connected = false;
  for (int tryCounter=0; tryCounter <= 5; tryCounter++) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint8_t status = WiFi.waitForConnectResult();

    if (status == WL_CONNECTED) {
      connected = true;
      break;
    }
    else {
      Serial.print("Failed to connect to wifi: ");
      Serial.println(status);
      delay(3000);
    }
  }

  if (!connected) {
    Serial.print("Fatal error: unable to connect to wifi: ");
    Serial.print(WIFI_SSID);
    Serial.print(" ");
    Serial.print(WIFI_PASS);
    Serial.println();

    Serial.print("Scanning networks... ");
    WiFi.disconnect();
    delay(100);
    int n = WiFi.scanNetworks();
    if (n == 0) {
        Serial.println("no networks found");
    } 
    else {
      Serial.print(n);
      Serial.println(" networks found");
      for (int i = 0; i < n; ++i) {
        // Print SSID and RSSI for each network found
        Serial.print(i + 1);
        Serial.print(": ");
        Serial.print(WiFi.SSID(i));
        Serial.print(" (");
        Serial.print(WiFi.RSSI(i));
        Serial.print(")");
        Serial.println((WiFi.encryptionType(i) == WIFI_AUTH_OPEN)?" ":"*");
        delay(10);
      }
    }
    
    delay(3000);
    ESP.restart();
  }
  
  Serial.print("Network connection established. Local IP address: ");
  Serial.print(WiFi.localIP());
  Serial.println();  

  if (MDNS.begin(MDNS_NAME)) {
    Serial.print("mDNS responder started for: ");
    Serial.println(MDNS_NAME);
  }
  else {
    Serial.print("Failed to create mDNS responder for: ");
    Serial.println(MDNS_NAME);
  }
}

// -------------------------------------------------------------------------------
// InitOTA()
// -------------------------------------------------------------------------------
void InitOTA() {
  Serial.println("Setting up OTA");
  
  // Port defaults to 3232
  // ArduinoOTA.setPort(3232);
  // Hostname defaults to esp3232-[MAC]
  ArduinoOTA.setHostname(MDNS_NAME);

  // No authentication by default
  if (strlen(OTA_PASSWORD) != 0) {
    ArduinoOTA.setPassword(OTA_PASSWORD);
    Serial.printf("OTA Password: %s\n\r", OTA_PASSWORD);
  } 
  else {
    Serial.println("No OTA password has been set!");
  }
  
  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
        type = "filesystem";
  
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      Serial.println("\r\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });
    
  ArduinoOTA.begin();
}

// -------------------------------------------------------------------------------
// InitMQTT()
// -------------------------------------------------------------------------------
void InitMQTT() {
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(HandleMqttMessage);  
}

// -------------------------------------------------------------------------------
// InitCC1101()
// -------------------------------------------------------------------------------
void InitCC1101() {
  ELECHOUSE_cc1101.setSpiPin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);

  if (ELECHOUSE_cc1101.getCC1101()) {       
    LogPrintln("CC1101 connection OK");
  }
  else{
    LogPrintln("CC1101 connection error");
  }  

  ELECHOUSE_cc1101.Init();  

  LogPrint("CC1101 using ");
  LogPrintln(RF_FREQUENCY);
  ELECHOUSE_cc1101.setMHZ(RF_FREQUENCY);

  // default in rx mode
  ELECHOUSE_cc1101.SetRx();
  mySwitch.enableReceive(RX_PIN);
}

// -------------------------------------------------------------------------------
// StartTelnetDebugStream()
// -------------------------------------------------------------------------------
void StartTelnetDebugStream() {
  Serial.println("Starting Telnet stream. Connect Telnet client to view output.");
  Serial.println("Send 'R' to reboot, 'C' to stop Telnet stream.");
  TelnetStream.begin();
}

// -------------------------------------------------------------------------------
// HandleTelnetInput()
// -------------------------------------------------------------------------------
void HandleTelnetInput() {
  switch (TelnetStream.read()) {
    case 'R':
      Serial.println("Rebooting");
      TelnetStream.println("Rebooting");
      TelnetStream.stop();
      delay(1000);
      ESP.restart();
      break;

    case 'C':
      Serial.println("Closing Telnet connection");
      TelnetStream.println("Closing connection");
      TelnetStream.stop();
      break;
  }
}

// -------------------------------------------------------------------------------
// ReconnectMQTT()
// -------------------------------------------------------------------------------
void ReconnectMqtt() {
  // Loop until we're reconnected
  while (!mqttClient.connected()) {
    LogPrint("Attempting MQTT connection...");
    
    // Attempt to connect
    if (mqttClient.connect(MQTT_CLIENT_NAME, MQTT_USER, MQTT_PASS, STATUS_TOPIC, 0, true, "offline")) {
      LogPrintln("connected");
      
      // Once connected, publish an announcement
      mqttClient.publish(STATUS_TOPIC, "online", true);
      
      // Resubscribe to the command topics 
      mqttClient.subscribe(MQTT_TOPIC_POWER_SET);
      mqttClient.subscribe(MQTT_TOPIC_FAN_SET);
      mqttClient.subscribe(MQTT_TOPIC_SPEED_SET);
      mqttClient.subscribe(MQTT_TOPIC_LIGHT_SET);

      // Resubscribe to the status topics
      mqttClient.subscribe(MQTT_TOPIC_SPEED_STATE);
      mqttClient.subscribe(MQTT_TOPIC_LIGHT_STATE);
    } 
    else {
      LogPrint("failed, rc=");
      LogPrint(mqttClient.state());
      LogPrintln(" try again in 5 seconds");
      delay(5000);
    }
  }
}

// -------------------------------------------------------------------------------
// SendRfSignal()
// -------------------------------------------------------------------------------
void SendRfSignal(int value) {
    LogPrint("Sending RF signal ");
    LogPrint(value);
    LogPrint(" (");
    LogPrint(GetFanCodeDescription(value));
    LogPrintln(")");

    mySwitch.disableReceive();
    mySwitch.enableTransmit(TX_PIN);
    ELECHOUSE_cc1101.SetTx();         
    mySwitch.setRepeatTransmit(RF_REPEATS); 
    mySwitch.setProtocol(RF_PROTOCOL);
    mySwitch.setPulseLength(RF_DELAY);
    mySwitch.send(value, RF_BITLENGTH); 

    ELECHOUSE_cc1101.SetRx();
    mySwitch.disableTransmit();
    mySwitch.enableReceive(RX_PIN);
}

// -------------------------------------------------------------------------------
// HandleIncomingRfSignal() - returns TRUE if the signal was recognized and handled
// -------------------------------------------------------------------------------
bool HandleIncomingRfSignal() {
    LogPrint("Received RF signal ");
    LogPrint(mySwitch.getReceivedValue());
    LogPrint(" / ");
    LogPrint(mySwitch.getReceivedBitlength());
    LogPrint(" bit / Protocol: ");
    LogPrint(mySwitch.getReceivedProtocol());
    LogPrint(" / Delay: ");
    LogPrint(mySwitch.getReceivedDelay());
    LogPrint(" (");
    LogPrint(GetFanCodeDescription(mySwitch.getReceivedValue()));
    LogPrintln(")");

    // This could pick up RF signals we don't care about; ignore the ones that don't match
    // this fan's config
    if (mySwitch.getReceivedProtocol() != RF_PROTOCOL || mySwitch.getReceivedBitlength() != RF_BITLENGTH) {
      return false;
    }

    // The RF signal will have been picked up by the fan, so we want to publish a new MQTT state message
    // that represents the new state of the fan
    ConvertRfSignalToMqttState(mySwitch.getReceivedValue());

    return true;
}

// -------------------------------------------------------------------------------
// ConvertRfSignalToMqttState()
// -------------------------------------------------------------------------------
void ConvertRfSignalToMqttState(int rfCode) {
    switch (rfCode) {
      case FanCodes::POWER_OFF:
        PostFanSpeedToMqtt(FanSpeed::SPEED_OFF);
        PostLightStateToMqtt(false);
        break;
      case FanCodes::POWER_ON:
        PostFanSpeedToMqtt(FanSpeed::SPEED_HIGH);
        PostLightStateToMqtt(true);
        break;
      case FanCodes::FAN_OFF:
        PostFanSpeedToMqtt(FanSpeed::SPEED_OFF);
        break;
      case FanCodes::FAN_LOW:
        PostFanSpeedToMqtt(FanSpeed::SPEED_LOW);
        break;
      case FanCodes::FAN_MED:
        PostFanSpeedToMqtt(FanSpeed::SPEED_MED);
        break;
      case FanCodes::FAN_HIGH:
        PostFanSpeedToMqtt(FanSpeed::SPEED_HIGH);
        break;
      case FanCodes::LIGHT_OFF:
        PostLightStateToMqtt(false);
        break;
      case FanCodes::LIGHT_ON:
        PostLightStateToMqtt(true);
        break;
      default:
        LogPrint("Aborting MQTT status sync: unknown RF code ");
        LogPrintln(rfCode);
    }
}

// -------------------------------------------------------------------------------
// HandleMqttMessage()
//
// This handles both the SET topics and STATE topics. 
//
// The SET topics result in an RF signal being sent and a STATE message being posted.
// The STATE topics result in the local state of this device being updated to match current state.
//
// This keeps the local state in sync as the device is used, but also initializes it
// on boot based on the last published state topics. (The down side is that if the MQTT system
// isn't working, then the local state is never updated)
// -------------------------------------------------------------------------------
void HandleMqttMessage(char* topic, byte* payload, unsigned int length) {
  LogPrint("Received MQTT message [");
  LogPrint(topic);
  LogPrint("]: ");

  String payloadString;
  for (int i = 0; i < length; i++) {
    payloadString += (char)payload[i];
  }
  LogPrintln(payloadString);

  int rfCodeToSend = 0;

  // COMMAND: POWER SET (turns fan and light off, or fan to max and light to on)
  if (strcmp(topic, MQTT_TOPIC_POWER_SET) == 0) {
    if (payloadString == "OFF") {
      rfCodeToSend = FanCodes::POWER_OFF;
    } 
    else if (payloadString == "ON") {
      rfCodeToSend = FanCodes::POWER_ON;
    } 
    else {
      LogPrint("Unrecognized power value: ");
      LogPrintln(payloadString);
    }
  } 

  // COMMAND: FAN SET (binary on/off, resolves to either fan off or fan low)
  else if (strcmp(topic, MQTT_TOPIC_FAN_SET) == 0) {
    if (payloadString == "OFF") {
      rfCodeToSend = FanCodes::FAN_OFF;
    } 
    else if (payloadString == "ON") {
      rfCodeToSend = FanCodes::FAN_LOW;
    } 
    else {
      LogPrint("Unrecognized fan on/off value: ");
      LogPrintln(payloadString);
    }
  }

  // COMMAND: SPEED SET 
  else if (strcmp(topic, MQTT_TOPIC_SPEED_SET) == 0) {
    if (payloadString == "0") {
      rfCodeToSend = FanCodes::FAN_OFF;
    } 
    else if (payloadString == "1") {
      rfCodeToSend = FanCodes::FAN_LOW;
    } 
    else if (payloadString == "2") {
      rfCodeToSend = FanCodes::FAN_MED;
    } 
    else if (payloadString == "3") {
      rfCodeToSend = FanCodes::FAN_HIGH;
    } 
    else {
      LogPrint("Unrecognized speed value: ");
      LogPrintln(payloadString);
    }
  } 

  // COMMAND: LIGHT SET 
  else if (strcmp(topic, MQTT_TOPIC_LIGHT_SET) == 0) {
    if (payloadString == "OFF") {
      rfCodeToSend = FanCodes::LIGHT_OFF;
    } 
    else if (payloadString == "ON") {
      rfCodeToSend = FanCodes::LIGHT_ON;
    } 
    else {
      LogPrint("Unrecognized light value: ");
      LogPrintln(payloadString);
    }
  } 

  // STATE: FAN SPEED (update internal state to match MQTT)
  else if (strcmp(topic, MQTT_TOPIC_SPEED_STATE) == 0) {
    if (payloadString == "0") {
      LogPrintln("Setting local state: speed off");
      fanSpeed = FanSpeed::SPEED_OFF;
    } 
    else if (payloadString == "1") {
      LogPrintln("Setting local state: speed low");
      fanSpeed = FanSpeed::SPEED_LOW;
    } 
    else if (payloadString == "2") {
      LogPrintln("Setting local state: speed medium");
      fanSpeed = FanSpeed::SPEED_MED;
    } 
    else if (payloadString == "3") {
      LogPrintln("Setting local state: speed high");
      fanSpeed = FanSpeed::SPEED_HIGH;
    } 
    else {
      LogPrint("Unrecognized speed value to sync: ");
      LogPrintln(payloadString);
    }
  } 

  // STATE: LIGHT STATE (update internal state to match MQTT)
  else if (strcmp(topic, MQTT_TOPIC_LIGHT_STATE) == 0) {
    if (payloadString == "OFF") {
      LogPrintln("Setting local state: light off");
      lightState = false;
    } 
    else if (payloadString == "ON") {
      LogPrintln("Setting local state: light on");
      lightState = true;
    } 
    else {
      LogPrint("Unrecognized light value to sync: ");
      LogPrintln(payloadString);
    }
  } 

  if (rfCodeToSend > 0) {
    SendRfSignal(rfCodeToSend);
    ConvertRfSignalToMqttState(rfCodeToSend);
  }
}

// -------------------------------------------------------------------------------
// PostFanSpeedToMqtt()
// -------------------------------------------------------------------------------
void PostFanSpeedToMqtt(int speed) {
  String speedString = String(speed);

  LogPrint("Sending speed [");
  LogPrint(speed);
  LogPrint("] to topic [");
  LogPrint(MQTT_TOPIC_SPEED_STATE);
  LogPrintln("]");

  mqttClient.publish(MQTT_TOPIC_SPEED_STATE, speedString.c_str(), true);
  mqttClient.publish(MQTT_TOPIC_FAN_STATE, speed == 0 ? "OFF" : "ON", true);
}

// -------------------------------------------------------------------------------
// PostLightStateToMqtt()
// -------------------------------------------------------------------------------
void PostLightStateToMqtt(bool state) {
  String stateString = state ? "ON" : "OFF";

  LogPrint("Sending light status [");
  LogPrint(state);
  LogPrint("] to topic [");
  LogPrint(MQTT_TOPIC_LIGHT_STATE);
  LogPrintln("]");

  mqttClient.publish(MQTT_TOPIC_LIGHT_STATE, stateString.c_str(), true);
}

// -------------------------------------------------------------------------------
// GetFanCodeDescription()
// -------------------------------------------------------------------------------
String GetFanCodeDescription(int value) {
    switch (value) {
      case FanCodes::POWER_OFF:
        return "POWER OFF";
      case FanCodes::POWER_ON:
        return "POWER ON";
      case FanCodes::FAN_OFF:
        return "FAN OFF";
      case FanCodes::FAN_LOW:
        return "FAN LOW";
      case FanCodes::FAN_MED:
        return "FAN MED";
      case FanCodes::FAN_HIGH:
        return "FAN HIGH";
      case FanCodes::LIGHT_OFF:
        return "LIGHT OFF";
      case FanCodes::LIGHT_ON:
        return "LIGHT ON";
      default:
        return "** UNRECOGNIZED COMMAND **";
    }
}

// -------------------------------------------------------------------------------
// LogPrint()
// -------------------------------------------------------------------------------
void LogPrint(String msg) {
  Serial.print(msg);

  if (USE_TELNET_STREAM) {
    TelnetStream.print(msg);
  }
}
void LogPrint(long unsigned int value) {
  Serial.print(value);

  if (USE_TELNET_STREAM) {
    TelnetStream.print(value);
  }
}
void LogPrint(unsigned int value) {
  Serial.print(value);

  if (USE_TELNET_STREAM) {
    TelnetStream.print(value);
  }
}
void LogPrint(int value) {
  Serial.print(value);

  if (USE_TELNET_STREAM) {
    TelnetStream.print(value);
  }
}
void LogPrint(double value) {
  Serial.print(value, 3);

  if (USE_TELNET_STREAM) {
    TelnetStream.print(value, 3);
  }
}

// -------------------------------------------------------------------------------
// LogPrintln()
// -------------------------------------------------------------------------------
void LogPrintln(String msg) {
  Serial.println(msg);

  if (USE_TELNET_STREAM) {
    TelnetStream.println(msg);
  }
}
void LogPrintln(long unsigned int value) {
  Serial.println(value);

  if (USE_TELNET_STREAM) {
    TelnetStream.println(value);
  }
}
void LogPrintln(unsigned int value) {
  Serial.println(value);

  if (USE_TELNET_STREAM) {
    TelnetStream.println(value);
  }
}
void LogPrintln(int value) {
  Serial.println(value);

  if (USE_TELNET_STREAM) {
    TelnetStream.println(value);
  }
}
void LogPrintln(double value) {
  Serial.println(value, 3);

  if (USE_TELNET_STREAM) {
    TelnetStream.println(value, 3);
  }
}
