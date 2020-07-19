#include <FS.h>
#include <EEPROM.h>
#include <Regexp.h>
#include <TimeLib.h>
#include <WiFiUdp.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <RemoteDebug.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>

char HOSTNAME[11] =   "mqttl_";  //改为变量，后面205行将根据模块信息自动修改，避免多个模块使用同一个clientid连接mqtt
#define AP_PASSWORD   "mqttlpassword"
#define HTTP_PORT      80
#define WIFI_TIMEOUT   30000
#define TIME_ZONE      8;

const char* update_path = "/update";                                          //OTA页面地址
const char* update_username = "admin";                                        //OTA用户名
const char* update_password = "admin";                                     //OTA密码

static unsigned long last_loop;

long LAST_RECONNECT_ATTEMPT = 0;

char MQTT_HOST[64] = "";
char MQTT_PORT[6]  = "";
char MQTT_USER[32] = "";
char MQTT_PASS[32] = "";

String infoBuffer;

RemoteDebug Debug;
WiFiClient espClient;
ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;
PubSubClient mqtt_client(espClient);

void mqtt_callback(char* topic, byte* payload, unsigned int length) {
}

void configModeCallback(WiFiManager *myWiFiManager) {
    Serial.println(F("Entered config mode"));
    Serial.println(WiFi.softAPIP());
    Serial.println(myWiFiManager->getConfigPortalSSID());
}

bool mqtt_reconnect() {
    while (!mqtt_client.connected()) {
        if (mqtt_client.connect(HOSTNAME, MQTT_USER, MQTT_PASS)) {
            Debug.printf("MQTT connected as: %s\n",HOSTNAME);
            mqtt_client.publish("mqttl/status", "alive");
            mqtt_client.subscribe("mqttl/set");
        }
        else {
            Serial.print(F("MQTT Connection failed: rc="));
            Serial.println(mqtt_client.state());
            Serial.println(F(" Retrying in 5 seconds"));
            delay(5000);
        }
    }
    return true;
}

String read_eeprom(int offset, int len) {
    String res = "";
    for (int i = 0; i < len; ++i) {
        res += char(EEPROM.read(i + offset));
    }
    Serial.print(F("read_eeprom(): "));
    Serial.println(res.c_str());
     return res;
}

void write_eeprom(int offset, int len, String value) {
    Serial.print(F("write_eeprom(): "));
    Serial.println(value.c_str());
    for (int i = 0; i < len; ++i) {
        if ((unsigned)i < value.length()) {
            EEPROM.write(i + offset, value[i]);
        }
        else {
            EEPROM.write(i + offset, 0);
        }
    }
}

bool shouldSaveConfig = false;

void save_wifi_config_callback () {
    shouldSaveConfig = true;
}

void setup_ota() {
    httpUpdater.setup(&httpServer, update_path, update_username, update_password);
    httpServer.begin();
    //Serial.printf("HTTPUpdateServer ready! Open http://%s.local%s in your browser and login with username '%s' and password '%s'\n", host, update_path, update_username, update_password);
}

void setup_mdns() {
    bool mdns_result = MDNS.begin(HOSTNAME);
}

//获取串口数据中含有ble_event字符串的json
String get_json(String data) {
    MatchState parse_result;
    char match_result[data.length() - 30];
    char buf[data.length() + 1];
    data.toCharArray(buf, data.length() + 1);
    parse_result.Target(buf);
    if (parse_result.Match("\{.*ble_event.*\}") == REGEXP_MATCHED) {
      return String(parse_result.GetMatch(match_result));
    }
    else {
      return "UNKNOWN";
    }
}

//长度超过1个字节的edata进行逆序调整，顺便修正丢字符现象造成的长度错误，数据很可能还是错的
String revstr(String rawstr) {
  if (rawstr.length() % 2 == 1) {
    Debug.printf("wrong length of hexstr: %s\n",rawstr.c_str());
    rawstr += "0";
    }
  String result = "";
  for (int i=0;i<=rawstr.length()/2;i++)
    result = rawstr.substring(i*2,i*2+2) + result;
  return result;
  }
  
void parse_json(String json) {
  
    //米家智能锁的json类似这样：{"id":1518998071,"method":"_async.ble_event","params":{"dev":{"did":"1011078646","mac":"AA:BB:CC:DD:EE:FF","pdid":794},"evt":[{"eid":7,"edata":"0036f6e45e"}],"frmCnt":97,"gwts":2362}}
    //状态信息在这里：{"eid":7,"edata":"0036f6e45e"}
    char buf[json.length() + 1];
    json.toCharArray(buf, json.length() + 1);
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, buf);
    JsonObject params = doc["params"];
    if (!error) {
    //提取did/eid/edata
    String did = params["dev"]["did"];
    int eid = params["evt"][0]["eid"];
    String edata = params["evt"][0]["edata"];
    if (edata.length() > 2) edata = revstr(edata);//凡长度超过2的edata一律调整逆序
    
    String topic = "mqttl/"+did+"/";
    topic += eid; //核心语句
    mqtt_client.publish( topic.c_str(),edata.c_str(),false);
    Debug.printf("topic: %s; payload: %s\n",topic.c_str(),edata.c_str());
    }
    else {
        Debug.printf("deserializeJson failed: %s\n", error.c_str());
        Debug.printf("Json data: %s\n", buf);
    }
}

//void mqtt_publish(String topic, String payload,boolean retained) {
//    mqtt_client.publish(topic.c_str(), payload.c_str(), retained);
//}

void setup() {
    //尝试解决串口乱码
    //关闭串口输出
    Serial.begin(115200, SERIAL_8N1, SERIAL_RX_ONLY);
    Serial.setDebugOutput (false);
    //接收缓冲区增大一倍
    Serial.setRxBufferSize (512);
    //超时时间减半
    Serial.setTimeout(500);
    
    EEPROM.begin(512);
    Debug.begin("mqttl");
    Debug.setResetCmdEnabled(true);

    String settings_available = read_eeprom(134, 1);
    if (settings_available == "1") {
        read_eeprom(0, 64).toCharArray(MQTT_HOST, 64);   // * 0-63
        read_eeprom(64, 6).toCharArray(MQTT_PORT, 6);    // * 64-69
        read_eeprom(70, 32).toCharArray(MQTT_USER, 32);  // * 70-101
        read_eeprom(102, 32).toCharArray(MQTT_PASS, 32); // * 102-133
    }
    WiFiManagerParameter CUSTOM_MQTT_HOST("host", "MQTT服务器","", 64);
    WiFiManagerParameter CUSTOM_MQTT_PORT("port", "MQTT端口", "", 6);
    WiFiManagerParameter CUSTOM_MQTT_USER("user", "MQTT账号","", 32);
    WiFiManagerParameter CUSTOM_MQTT_PASS("pass", "MQTT密码","", 32);

    WiFiManager wifiManager;

    wifiManager.setAPCallback(configModeCallback);
    wifiManager.setConfigPortalTimeout(WIFI_TIMEOUT);
    wifiManager.setSaveConfigCallback(save_wifi_config_callback);

    wifiManager.addParameter(&CUSTOM_MQTT_HOST);
    wifiManager.addParameter(&CUSTOM_MQTT_PORT);
    wifiManager.addParameter(&CUSTOM_MQTT_USER);
    wifiManager.addParameter(&CUSTOM_MQTT_PASS);

    String ssid = "mqttl_" + String(ESP.getChipId()).substring(0, 4);
    strcpy(HOSTNAME,ssid.c_str());
    if (!wifiManager.autoConnect(ssid.c_str(), AP_PASSWORD)) {
        Serial.println(F("Failed to connect to WIFI and hit timeout"));
        ESP.reset();
        delay(WIFI_TIMEOUT);
    }

    if (settings_available != "1") {
    strcpy(MQTT_HOST, CUSTOM_MQTT_HOST.getValue());
    strcpy(MQTT_PORT, CUSTOM_MQTT_PORT.getValue());
    strcpy(MQTT_USER, CUSTOM_MQTT_USER.getValue());
    strcpy(MQTT_PASS, CUSTOM_MQTT_PASS.getValue());
    }

    if (shouldSaveConfig) {
        Serial.println(F("Saving WiFiManager config"));
        write_eeprom(0, 64, MQTT_HOST);   // * 0-63
        write_eeprom(64, 6, MQTT_PORT);   // * 64-69
        write_eeprom(70, 32, MQTT_USER);  // * 70-101
        write_eeprom(102, 32, MQTT_PASS); // * 102-133
        write_eeprom(134, 1, "1");        // * 134 --> always "1"
        EEPROM.commit();
    }

    setup_ota();
    setup_mdns();

    mqtt_client.setServer(MQTT_HOST, atoi(MQTT_PORT));
    mqtt_client.setCallback(mqtt_callback);

    Debug.printf("MQTT initiated: %s:%s\n", MQTT_HOST, MQTT_PORT);
}

void loop() {
    last_loop = millis();

    Debug.handle();
    httpServer.handleClient();

    if (!mqtt_client.connected()) {
        long now = millis();
        if (now - LAST_RECONNECT_ATTEMPT > 5000) {
            LAST_RECONNECT_ATTEMPT = now;
            if (mqtt_reconnect()) {
                LAST_RECONNECT_ATTEMPT = 0;
                }
            }
        }
    else {
        mqtt_client.loop();
        }

    while (Serial.available()){
        //只匹配含有method的json
        String result = get_json(Serial.readStringUntil('\n'));
        if ( result != "UNKNOWN" ){
          
            //处理数据
            parse_json(result);
            
            //telnet到8266的ip可以查看日志
            //Debug.println(result);
            }
        else {
            //Debug.println("UNKNOWN");
        }
    }
    delay(1);//100有丢消息现象，10偶尔有（几天可见1次），1待观察
}
