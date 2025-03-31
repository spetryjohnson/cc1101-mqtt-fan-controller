# cc1101-mqtt-fan-controller

This project was created to automate a [Home Decorators Collection Beckford 52in ceiling fan](https://www.homedepot.com/p/Home-Decorators-Collection-Beckford-52-in-Indoor-Brushed-Nickel-Ceiling-Fan-with-Adjustable-White-Integrated-LED-with-Remote-Control-Included-YG630-BN/318749131) from Home Depot. 

This fan uses a 303MHz RF signal, so my original approach of using a [hacked Sonoff RF Bridge](https://github.com/schmurtzm/RFLink32-For-Sonoff-RF-Bridge) wasn't going to work. 

I spent a bunch of time trying to figure out how to automate the 303MHz fan and came across a bunch of partially-working-but-partially-outdated forum posts, Github projects, etc.

This project combines everything I learned from those resources into a single project, specifically for the CC1101 version that is readily available in March 2025.

This project:
  - Will help you capture RF commands from your remote
  - Will convert RF commands --> MQTT state updates
  - Will convert MQTT commands --> RF transmissions
  - Will create a MQTT Fan element in Home Assistant
  - Supports OTA code updates, so you can push changes after the device is in place

## Software dependencies

Install these via the Arduino IDE library manager:

  - [SmartRC-CC1101-Driver-Lib](https://github.com/LSatan/SmartRC-CC1101-Driver-Lib)
  - [rc-switch](https://github.com/sui77/rc-switch)
  - [PubSubClient](https://docs.arduino.cc/libraries/pubsubclient/)

## Materials

### CC1101 RF transceiver

The CC1101 is a common RF transceiver that supports multiple frequencies including 303MHz and 433MHz.

**There are multiple versions of this module** and many of the resources I found were for an older one. This project was written and tested using the 8-pin "v2.0" module found on [Amazon](https://www.amazon.com/dp/B0D2TMTV5Z) or [AliExpress](https://www.aliexpress.us/item/3256806768036185.html).

![Image of CC1101 module](/assets/CC1101.png)

### Ceiling fan w/ RF Remote

My fan uses this remote, which has the model number "TX028C-2" stamped on the back.

![Image of fan remote with power button, speed control button, light button, and light temp button](/assets/remote.png)


### ESP32

Any ESP32 (or even an ESP8266) should work, but I'm using one of the [WROOM 32 dev boards from Amazon](https://www.amazon.com/dp/B09GK74F7N).


![Image of ESP32 board](/assets/ESP32.png)

## Wiring

![Wiring schematic](/assets/wiring.png)

Pinout was found on [this datasheet](https://github.com/NorthernMan54/rtl_433_ESP/blob/main/docs/E07-M1101D-TH_Usermanual_EN_v1.30.pdf)


## Getting started 

### Step 1 - Figure out your frequency

If you don't know what frequency your fan uses you can try a web search search for the remote's FCC identifier, or you can use the Frequency Analyzer feature of a Flipper Zero.

![Frequency Analyzer flipper zero](/assets/flipper-zero-freq-analyzer.png)

### Step 2 - Capture your RF codes

Set the `RF_FREQUENCY` variable to your frequency and then install the sketch to your board. (The other values don't matter yet)

After the board connects to Wifi and the MQTT broker it will start listening for RF commands. Press buttons on your remote and you should see the captured commands in the Arduino IDE Serial Monitor.

![Screenshot of commands being captured](/assets/command-capture.png)

### Step 3 - Update the sketch

Set the `RF_PROTOCOL`, `RF_BITLENGTH`, and `RF_DELAY` variables to match the values you captured. These will be the same for all commands.

Then update the `FanCodes` enum with the values you captured.

### Step 4 - Create the MQTT Fan element in Home Assistant

```
mqtt:
  fan:
    - name: "Guest Room Ceiling Fan Speed"
      unique_id: "guest_room_ceiling_fan_mqtt"
      
      # On/Off state
      state_topic: "home/rooms/guestroom/hampton-bay-fan/fan/state"
      command_topic: "home/rooms/guestroom/hampton-bay-fan/fan/set"
      
      # Percentage (speed)
      percentage_command_topic: "home/rooms/guestroom/hampton-bay-fan/speed/set"
      percentage_state_topic: "home/rooms/guestroom/hampton-bay-fan/speed/state"
      speed_range_min: 1
      speed_range_max: 3

      availability:
        topic: "home/rooms/guestroom/hampton-bay-fan/status"
        payload_available: "online"
        payload_not_available: "offline"

      optimistic: false
      retain: true

  switch:
    - name: "Guest Room Ceiling Fan Light"
      unique_id: "switch.guest_room_ceiling_fan_light"
      device_class: "switch"
      state_topic: "home/rooms/guestroom/hampton-bay-fan/light/state"
      command_topic: "home/rooms/guestroom/hampton-bay-fan/light/set"
      availability_topic: "home/rooms/guestroom/hampton-bay-fan/status"
      payload_available: "online"
      payload_not_available: "offline"
      optimistic: false
      retain: true
```
![Home Assistant fan control](/assets/ha-fan-control.png)