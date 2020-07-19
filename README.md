# mqttl
Integrate all types of Xiaomi (Mijia) BLE devices into Home Assistant, by relaying messages from Gateway's TTL to MQTT, running on ESP-01S (ESP8266).

Frok from https://github.com/killadm/LOCK2MQTT, thks to killadm!

### Usage
1. Flash ESP module (by usb-ttl or OTA);
2. Connect to the TTL pins of BLE Gateway device;
3. Join the AP named *mqttl_xxxx*, with the password of *mqttlpassword*;
4. Configure WiFi and MQTT broker;
5. Configure the sensors by yaml in Home Assistant (this step could be accomplished before flash ESP).

### Connection
[picture](connection.png)
