#define MQTT_SOCKET_TIMEOUT 2

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include "hexdump.h"
#include <FS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <ESPAsyncWebServer.h>
#include <EEPROM.h>

#define SERVICE_UUID (uint16_t)0x1821
#define CONFIGURE_UUID (uint16_t)0x2AAD
#define MQTT_UUID (uint16_t)0x2ADB
#define BATTERY_UUID (uint16_t)0x2A19
#define EEPROM_SIZE 1

#define BUFFER_SIZE 256
#define WIFI_SSID_CMD 0x01
#define WIFI_PASSWD_CMD 0x02
#define DEV_NAME_CMD 0x03
#define DEV_ID_CMD 0x04
#define DEV_PATH_CMD 0x05
#define MQTT_SERVER_CMD 0x06
#define MQTT_PORT_CMD 0x07
#define MQTT_PUB_CMD 0x08
#define MQTT_SUB_CMD 0x09

#define SLEEP_BUTTON GPIO_NUM_33
#define CONFIG_BUTTON GPIO_NUM_15
#define MAIN_LED GPIO_NUM_5
#define CONFIG_LED  GPIO_NUM_18
#define LONG_PRESS_TIME_CFG 2000
#define LONG_PRESS_TIME_DS 3000
#define INVALID_WIFI_BLINK_MS 200
#define INVALID_MQTT_BLINK_MS 600
#define CONFIG_STATUS_BLINK_MS 1000
#define UPDATE_STATUS_BLINK_MS 500  
#define VERSION "1.0.0"

#define ADC_CHANNEL_PIN 35
volatile byte main_led_state = LOW;
volatile byte config_led_state = LOW;

hw_timer_t *timer = NULL;

uint8_t config_mode = 0;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

char ssid[32] = "";
char passwd[32] = "";
char mqtt_server[64] = "";
int mqtt_port = 1883;
char mqtt_pub[16] = "";
char mqtt_sub[16] = "";
char dev_name[37] = ""; // One byte more to have a \0 in the end of var
char dev_id[37] = "";   // One byte more to have a \0 in the end of var
char dev_path[37] = ""; // One byte more to have a \0 in the end of var
const char *host = "esp32";


WiFiClient espClient;
PubSubClient client(espClient);
WebServer server(80);
BLEServer *pServer = NULL;
BLECharacteristic *pCharacteristic_conf;
BLECharacteristic *pCharacteristic_mqtt;
BLECharacteristic *pCharacteristic_bat;
uint8_t buf[BUFFER_SIZE] = {0x00};
uint8_t buf_len = 0;

long lastMsg = 0;
char msg[50];
int value = 0;

long lastReconnectAttempt = 0;

bool bleConnected = false;
bool wificonnected = false;
bool mqttconfigured = false;
bool mqtt_connected = false;
bool receivefromble = false;
bool transfertoble = false;
bool requireRestart = false;

// Variables will change:
int configBtnPrevLevel = LOW; // the previous state from the input pin
int configBtnCurrentLevel;    // the current reading from the input pin
unsigned long configBtnPressTime = 0;

// Variables for deep sleep
int powerBtnLastLevel = LOW; // the previous state from the input pin
int powerBtnCurrentLevel;    // the current reading from the input pin
unsigned long powerBtnPressTime = 0;

/* Style */
String style =
    "<style>#file-input,input{width:100%;height:44px;border-radius:4px;margin:10px auto;font-size:15px}"
    "input{background:#f1f1f1;border:0;padding:0 15px}body{background:#3498db;font-family:sans-serif;font-size:14px;color:#777}"
    "#file-input{padding:0;border:1px solid #ddd;line-height:44px;text-align:left;display:block;cursor:pointer}"
    "#bar,#prgbar{background-color:#f1f1f1;border-radius:10px}#bar{background-color:#3498db;width:0%;height:10px}"
    "form{background:#fff;max-width:258px;margin:75px auto;padding:30px;border-radius:5px;text-align:center}"
    ".btn{background:#3498db;color:#fff;cursor:pointer}</style>";

/* Login page */
String loginIndex =
    "<form name=loginForm>"
    "<h1>ESP32 Login</h1>"
    "<input name=userid placeholder='User ID'> "
    "<input name=pwd placeholder=Password type=Password> "
    "<input type=submit onclick=check(this.form) class=btn value=Login></form>"
    "<script>"
    "function check(form) {"
    "if(form.userid.value=='admin' && form.pwd.value=='admin')"
    "{window.open('/serverIndex')}"
    "else"
    "{alert('Error Password or Username')}"
    "}"
    "</script>" +
    style;

/* Server Index Page */
String serverIndex =
    "<script src='https://ajax.googleapis.com/ajax/libs/jquery/3.2.1/jquery.min.js'></script>"
    "<form method='POST' action='#' enctype='multipart/form-data' id='upload_form'>"
    "<input type='file' name='update' id='file' onchange='sub(this)' style=display:none>"
    "<label id='file-input' for='file'>   Choose file...</label>"
    "<input type='submit' class=btn value='Update'>"
    "<br><br>"
    "<div id='prg'></div>"
    "<br><div id='prgbar'><div id='bar'></div></div><br></form>"
    "<script>"
    "function sub(obj){"
    "var fileName = obj.value.split('\\\\');"
    "document.getElementById('file-input').innerHTML = '   '+ fileName[fileName.length-1];"
    "};"
    "$('form').submit(function(e){"
    "e.preventDefault();"
    "var form = $('#upload_form')[0];"
    "var data = new FormData(form);"
    "$.ajax({"
    "url: '/update',"
    "type: 'POST',"
    "data: data,"
    "contentType: false,"
    "processData:false,"
    "xhr: function() {"
    "var xhr = new window.XMLHttpRequest();"
    "xhr.upload.addEventListener('progress', function(evt) {"
    "if (evt.lengthComputable) {"
    "var per = evt.loaded / evt.total;"
    "$('#prg').html('progress: ' + Math.round(per*100) + '%');"
    "$('#bar').css('width',Math.round(per*100) + '%');"
    "}"
    "}, false);"
    "return xhr;"
    "},"
    "success:function(d, s) {"
    "console.log('success!') "
    "},"
    "error: function (a, b, c) {"
    "}"
    "});"
    "});"
    "</script>" +
    style;

//define the functions
uint8_t setup_wifi();
void setup_ble();
void storeConfig();
void setup_webserver();

void IRAM_ATTR onTime() {
   portENTER_CRITICAL_ISR(&timerMux);
   if(config_mode == 1){ // config mode
     digitalWrite(CONFIG_LED, LOW); //turn on CONFIG_LED
     digitalWrite(MAIN_LED, HIGH); // turn off MAIN_LED
   }
   else if((config_mode == 0) && (wificonnected == 0)){ // working mode, invalid wifi
      main_led_state = !main_led_state;  
      digitalWrite(MAIN_LED, main_led_state); //blink MAIN_LED
      config_led_state = !config_led_state; 
      digitalWrite(CONFIG_LED, config_led_state); // blink CONFIG_LED
   }
   else if((config_mode == 0) && (mqtt_connected == 0)){ //working mode, invalid mqtt
     digitalWrite(MAIN_LED, LOW); //turn on the MAIN_LED
     config_led_state = !config_led_state;
     digitalWrite(CONFIG_LED, config_led_state); //blink the CONFIG_LED
   }
   else if((config_mode == 0) && (mqtt_connected && wificonnected)){// working mode, normal
     main_led_state = !main_led_state;
     digitalWrite(MAIN_LED, main_led_state); //blink the MAIN_LED
     digitalWrite(CONFIG_LED, HIGH); // turn off the CONFIG_LED
   }
   else{
     digitalWrite(MAIN_LED, HIGH); //turn off the MAIN_LED
     digitalWrite(CONFIG_LED, HIGH); //turn off the CONFIG_LED
   }  
   portEXIT_CRITICAL_ISR(&timerMux);
}

//Function that prints the reason by which ESP32 has been awaken from sleep
void print_wakeup_reason()
{
  esp_sleep_wakeup_cause_t wakeup_reason;
  wakeup_reason = esp_sleep_get_wakeup_cause();
  switch (wakeup_reason)
  {
  case 1:
    Serial.println("Wakeup caused by external signal using RTC_IO");
    break;
  case 2:
    Serial.println("Wakeup caused by external signal using RTC_CNTL");
    break;
  case 3:
    Serial.println("Wakeup caused by timer");
    break;
  case 4:
    Serial.println("Wakeup caused by touchpad");
    break;
  case 5:
    Serial.println("Wakeup caused by ULP program");
    break;
  default:
    Serial.println("Wakeup was not caused by deep sleep");
    break;
  }
}

class MyServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *pServer)
  {
    bleConnected = true;
    Serial.println("BLE > Connected");
  };

  void onDisconnect(BLEServer *pServer)
  {
    bleConnected = false;
    Serial.println("BLE > Start advertising");
  }
};
class MyCallbacksForConfig : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pCharacteristic)
  {
    std::string value = pCharacteristic->getValue();

    if (value.length() > 0)
    {
      Serial.print(">>>>> New value: ");
      for (int i = 0; i < value.length(); i++)
      {
        Serial.print(value[i]);
        if ((value[0] == WIFI_SSID_CMD) && (i > 0))
        {
          ssid[i - 1] = value[i];
          memset(&ssid[i], '\0', (sizeof(ssid) - i));
        }
        else if ((value[0] == WIFI_PASSWD_CMD) && (i > 0))
        {
          passwd[i - 1] = value[i];
          memset(&passwd[i], '\0', (sizeof(passwd) - i));
        }
        else if ((value[0] == DEV_NAME_CMD) && (i > 0))
        {
          dev_name[i - 1] = value[i];
          memset(&dev_name[i], '\0', (sizeof(dev_name) - i));
        }
        else if ((value[0] == DEV_ID_CMD) && (i > 0))
        {
          dev_id[i - 1] = value[i];
          memset(&dev_id[i], '\0', (sizeof(dev_id) - i));
        }
        else if ((value[0] == DEV_PATH_CMD) && (i > 0))
        {
          dev_path[i - 1] = value[i];
          memset(&dev_path[i], '\0', (sizeof(dev_path) - i));
        }
        else if ((value[0] == MQTT_SERVER_CMD) && (i > 0))
        {
          mqtt_server[i - 1] = value[i];
          memset(&mqtt_server[i], '\0', (sizeof(mqtt_server) - i));
        }
        else if ((value[0] == MQTT_PORT_CMD) && (i > 0))
        {
          mqtt_port = mqtt_port * 10 + (value[i] - '\0');
        }
        else if ((value[0] == MQTT_PUB_CMD) && (i > 0))
        {
          mqtt_pub[i - 1] = value[i];
          memset(&mqtt_pub[i], '\0', (sizeof(mqtt_pub) - i));
        }
        else if ((value[0] == MQTT_SUB_CMD) && (i > 0))
        {
          mqtt_sub[i - 1] = value[i];
          memset(&mqtt_sub[i], '\0', (sizeof(mqtt_sub) - i));
        }
      }
      storeConfig();
    }
  }
};
class MyCallbacksForMqtt : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pCharacteristic)
  {
    std::string value = pCharacteristic->getValue();
    if (value.length() > 0)
    {
      Serial.print(">>>>> New value: ");
      buf_len = value.length();
      for (int i = 0; i < value.length(); i++)
      {
        Serial.print(value[i]);
        buf[i] = value[i];
      }
      receivefromble = true;
      Serial.println(" >>>>>");
    }
  }
};
class MyCallbacksForBatt : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pCharacteristic)
  {
    std::string value = pCharacteristic->getValue();
    if (value.length() > 0)
    {
      Serial.print(">>>>> New value: ");
      buf_len = value.length();
      for (int i = 0; i < value.length(); i++)
      {
        Serial.print(value[i]);
        buf[i] = value[i];
      }
      Serial.println(" >>>>>");
    }
  }
};

void storeConfig()
{
  Serial.println("Store config");
  if (SPIFFS.begin())
  {
    Serial.println("  Mounted file system");

    if (SPIFFS.exists("/config.json"))
    {
      //file exists, reading and loading
      Serial.println("  Reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile)
      {
        Serial.println("  Opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buffer(new char[size]);
        configFile.readBytes(buffer.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject &json = jsonBuffer.parseObject(buffer.get());
        if (json.success())
        {
          Serial.println("  Parsed json");
          json.set("ssid", ssid);
          json.set("passwd", passwd);
          json.set("dev_name", dev_name);
          json.set("dev_path", dev_path);
          json.set("dev_id", dev_id);
          json.set("mqtt_server", mqtt_server);
          json.set("mqtt_port", mqtt_port);
          json.set("mqtt_pub", mqtt_pub);
          json.set("mqtt_sub", mqtt_sub);
          configFile.close();
          File configFile = SPIFFS.open("/config.json", "w");
          if (!configFile)
          {
            Serial.println("  [E] Failed to open config file for writing");
          }
          Serial.println("  Saved!");
          json.printTo(configFile);
          configFile.close();
        }
        else
        {
          Serial.println("  [E] Failed to load json config");
          configFile.close();
        }
      }
    }
    else
    {
      Serial.println("  [W] Config json is not existed.");
    }
    SPIFFS.end();
  }
  else
  {
    Serial.println("  [E] SPIFFS Mount Failed");
  }
}

void setupSpiffs()
{
  //clean FS, for testing
  // SPIFFS.format();
  //read configuration from FS json
  Serial.println("Mounting FS");

  if (SPIFFS.begin(true))
  {
    Serial.println("  Mounted file system");
    if (SPIFFS.exists("/config.json"))
    {
      //file exists, reading and loading
      Serial.println("  Reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile)
      {
        Serial.println("  Opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buffer(new char[size]);
        configFile.readBytes(buffer.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject &config_json = jsonBuffer.parseObject(buffer.get());
        if (config_json.success())
        {
          Serial.println("  Parsed json");
          if (sizeof(config_json["ssid"]) > 0)
            strcpy(&ssid[0], config_json["ssid"]);
          if (sizeof(config_json["passwd"]) > 0)
            strcpy(&passwd[0], config_json["passwd"]);
          if (sizeof(config_json["dev_name"]) > 0)
            strcpy(&dev_name[0], config_json["dev_name"]);
          if (sizeof(config_json["dev_id"]) > 0)
            strcpy(&dev_id[0], config_json["dev_id"]);
          if (sizeof(config_json["dev_path"]) > 0)
            strcpy(&dev_path[0], config_json["dev_path"]);
          if (sizeof(config_json["mqtt_server"]) > 0)
            strcpy(&mqtt_server[0], config_json["mqtt_server"]);
          mqtt_port = config_json["mqtt_port"];
          if (sizeof(config_json["mqtt_pub"]) > 0)
            strcpy(&mqtt_pub[0], config_json["mqtt_pub"]);
          if (sizeof(config_json["mqtt_sub"]) > 0)
            strcpy(&mqtt_sub[0], config_json["mqtt_sub"]);
          Serial.println("  Loaded json config");
        }
        else
        {
          Serial.println("[E] Failed to load json config");
        }
      }
    }
    else
    {
      Serial.println("  Config is not existed. create config");

      DynamicJsonBuffer jsonBuffer;
      JsonObject &config_json = jsonBuffer.createObject();
      config_json["ssid"] = ssid;
      config_json["passwd"] = passwd;
      config_json["dev_name"] = dev_name;
      config_json["dev_path"] = dev_path;
      config_json["dev_id"] = dev_id;
      config_json["mqtt_server"] = mqtt_server;
      config_json["mqtt_port"] = mqtt_port;
      config_json["mqtt_pub"] = mqtt_pub;
      config_json["mqtt_sub"] = mqtt_sub;

      File configFile = SPIFFS.open("/config.json", "w");
      if (!configFile)
      {
        Serial.println("  [E] Failed to open config file for writing");
      }

      config_json.prettyPrintTo(Serial);
      config_json.printTo(configFile);
      Serial.println("  Created config json.");
      configFile.close();
      //end save
    }
    SPIFFS.end();
  }
  else
  {
    Serial.println("  [E] SPIFFS Mount Failed");
  }
  //end read
}

uint8_t setup_wifi()
{
  // We start by connecting to a WiFi network
  wificonnected = false;
  if ((ssid[0] == '\0') || (passwd[0] == '\0'))
  {
    Serial.println("  [W] The ssid and password is incorrectly");
    return 0;
  }
  Serial.print("Connecting to ");
  Serial.print(ssid);
  WiFi.setHostname(host);
  WiFi.begin(ssid, passwd);
  long now = millis();
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    if ((millis() - now) > 10000)
    {
      Serial.println("\n  [E] Cannot connect to the WiFi Network. Please check the ssid and password again and then reset the MCU.");
      Serial.print("  SSID: ");
      Serial.println(ssid);
      Serial.print("  Password: ");
      Serial.println(passwd);
      return 0;
    }
  }
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("  IP address: ");
  Serial.println(WiFi.localIP());
  wificonnected = true;
  setup_webserver();
  return 1;
}

void setup_ble()
{
  Serial.println("Init the BLE");
  BLEDevice::init(host);
  pServer = BLEDevice::createServer();

  BLEService *pService = pServer->createService(SERVICE_UUID);
  pServer->setCallbacks(new MyServerCallbacks());

  if (config_mode == 1)
  {
    pCharacteristic_conf = pService->createCharacteristic(
        CONFIGURE_UUID,
        BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_WRITE);
    pCharacteristic_conf->setCallbacks(new MyCallbacksForConfig());
    pCharacteristic_conf->setValue("configuration");
  }
  else
  {
    pCharacteristic_mqtt = pService->createCharacteristic(
        MQTT_UUID,
        BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY |
            BLECharacteristic::PROPERTY_INDICATE);
    pCharacteristic_mqtt->addDescriptor(new BLE2902());
    pCharacteristic_mqtt->setCallbacks(new MyCallbacksForMqtt());
    pCharacteristic_mqtt->setValue("router");
  }
  pCharacteristic_bat = pService->createCharacteristic(
        BATTERY_UUID,
        BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY |
            BLECharacteristic::PROPERTY_INDICATE);
  pCharacteristic_bat->addDescriptor(new BLE2902());
  pCharacteristic_bat->setCallbacks(new MyCallbacksForBatt());
  pCharacteristic_bat->setValue("Battery Monitor");
  
  pService->start();

  BLEAdvertising *pAdvertising = pServer->getAdvertising();
  pAdvertising->start();
}

/**** mqtt callback function ****/
void callback(char *topic, byte *message, unsigned int length)
{
  Serial.print("MQTT Incoming Message\n  Message from: ");
  Serial.print(topic);
  Serial.print("\n  Message: ");
  String messageTemp;

  for (int i = 0; i < length; i++)
  {
    Serial.print((char)message[i]);
    messageTemp += (char)message[i];
  }
  pCharacteristic_mqtt->setValue(messageTemp.c_str());
  pCharacteristic_mqtt->notify();
  Serial.println();
}

/**** mqtt reconnect function ****/
boolean reconnect()
{
  client.setSocketTimeout(2);
  if (client.connect(dev_id))
  {
    Serial.println("MQTT Connecting...");
    String real_sub;
    real_sub = String(dev_id) + "/" + String(dev_path) + "/" + String(mqtt_sub);
    client.subscribe(real_sub.c_str());
    Serial.println("  MQTT connected!");
  }
  return client.connected();
}

void setup_mqtt()
{
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void checkmqttinfo()
{
  if ((mqtt_server[0] != '\0') && (mqtt_port > 0) && (mqtt_pub[0] != '\0') && (mqtt_sub[0] != '\0') && (dev_id[0] != '\0') && (dev_name[0] != '\0') && (dev_path[0] != '\0'))
  {
    mqttconfigured = true;
    setup_mqtt();
  }
  else
  {
    mqttconfigured = false;
    Serial.println("[W] Please configure the mqtt and device info correctly");
  }
}

void writeConfigApi()
{
  if (server.hasArg("plain") == false)
  {
    Serial.println("post data is not correct");
    server.send(200, "text/plain", "error");
  }
  else
  {
    String body = server.arg("plain");
    Serial.print("post data: ");
    Serial.println(body.c_str());

    DynamicJsonBuffer jsonBuffer;
    JsonObject &config_api_json = jsonBuffer.parseObject(body);
    config_api_json.printTo(Serial);
    if (config_api_json.success())
    {
      Serial.println("\n...parsed json in restapi");
      if (config_api_json.containsKey("ssid"))
      {
        memset(ssid, '\0', sizeof(ssid));
        strcpy(&ssid[0], config_api_json["ssid"]);
      }
      if (config_api_json.containsKey("passwd"))
      {
        memset(passwd, '\0', sizeof(passwd));
        strcpy(&passwd[0], config_api_json["passwd"]);
      }
      if (config_api_json.containsKey("dev_name"))
      {
        memset(dev_name, '\0', sizeof(dev_name));
        strcpy(&dev_name[0], config_api_json["dev_name"]);
      }
      if (config_api_json.containsKey("dev_id"))
      {
        memset(dev_id, '\0', sizeof(dev_id));
        strcpy(&dev_id[0], config_api_json["dev_id"]);
      }
      if (config_api_json.containsKey("dev_path"))
      {
        memset(dev_path, '\0', sizeof(dev_path));
        strcpy(&dev_path[0], config_api_json["dev_path"]);
      }
      if (config_api_json.containsKey("mqtt_server"))
      {
        memset(mqtt_server, '\0', sizeof(mqtt_server));
        strcpy(&mqtt_server[0], config_api_json["mqtt_server"]);
      }
      if (config_api_json.containsKey("mqtt_port"))
        mqtt_port = config_api_json["mqtt_port"];
      if (config_api_json.containsKey("mqtt_pub"))
      {
        memset(mqtt_pub, '\0', sizeof(mqtt_pub));
        strcpy(&mqtt_pub[0], config_api_json["mqtt_pub"]);
      }
      if (config_api_json.containsKey("mqtt_sub"))
      {
        memset(mqtt_sub, '\0', sizeof(mqtt_sub));
        strcpy(&mqtt_sub[0], config_api_json["mqtt_sub"]);
      }
      delay(2000);
      Serial.println("...loaded json config");
    }
    else
    {
      Serial.println("failed to load json config in restapi");
    }
    storeConfig();
    server.send(200, "text/plain", "success");
  }
}
void getConfigApi()
{
  Serial.println("GET API");
  String str;
  if (SPIFFS.begin())
  {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json"))
    {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile)
      {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buffer(new char[size]);
        configFile.readBytes(buffer.get(), size);

        DynamicJsonBuffer jsonBuffer;
        JsonObject &json = jsonBuffer.parseObject(buffer.get());
        json.printTo(str);
      }
      else
      {
        str = "{\"result\":\"cannot open the config json\"}";
      }
    }
    else
    {
      str = "{\"result\":\"config is not existed\"}";
    }
  }
  else
  {
    str = "{\"result\":\"spiffs open failed\"}";
  }
  server.send(200, "application/json", str.c_str());
}
void setup_webserver()
{
  if (wificonnected)
  {
    if (MDNS.begin(host))
    {
      Serial.println("  MDNS responder started");
    }
    MDNS.addService("_http", "_tcp", 80);
    /* Config API */
    server.on("/write", HTTP_POST, writeConfigApi);
    server.on("/read", HTTP_GET, getConfigApi);
    server.on("/version", HTTP_GET, []()
              {
                server.sendHeader("Connection", "close");
                server.send(200, "text/plain", VERSION);
              });
    server.on("/battery", HTTP_GET, []()
              {
                server.sendHeader("Connection", "close");
                server.send(200, "application/json", "{\"value\":50}");
              });
    /* OTA web server*/
    server.on("/ota", HTTP_GET, []()
              {
                server.sendHeader("Connection", "close");
                server.send(200, "text/html", loginIndex);
              });
    server.on("/serverIndex", HTTP_GET, []()
              {
                server.sendHeader("Connection", "close");
                server.send(200, "text/html", serverIndex);
              });
    /*handling uploading firmware file */
    server.on(
        "/update", HTTP_POST, []()
        {
          server.sendHeader("Connection", "close");
          server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
          requireRestart = true;
        },
        []()
        {
          timerAlarmDisable(timer);
          digitalWrite(MAIN_LED, LOW);
          digitalWrite(CONFIG_LED, HIGH);
          HTTPUpload &upload = server.upload();
          if (upload.status == UPLOAD_FILE_START)
          {
            Serial.printf("Update: %s\n", upload.filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN))
            { //start with max available size
              Update.printError(Serial);
              timerAlarmEnable(timer);
              digitalWrite(MAIN_LED, HIGH);
              digitalWrite(CONFIG_LED, HIGH);
            }
          }
          else if (upload.status == UPLOAD_FILE_WRITE)
          {
            /* flashing firmware to ESP*/
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
            {
              Update.printError(Serial);
              timerAlarmEnable(timer);
              digitalWrite(MAIN_LED, HIGH);
              digitalWrite(CONFIG_LED, HIGH);
            }
          }
          else if (upload.status == UPLOAD_FILE_END)
          {
            if (Update.end(true))
            { //true to set the size to the current progress
              Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
            }
            else
            {
              Update.printError(Serial);
              timerAlarmEnable(timer);
              digitalWrite(MAIN_LED, HIGH);
              digitalWrite(CONFIG_LED, HIGH);
            }
          }
        });
    server.begin();
    Serial.println("  Start the WebServer");
  }
}
void initTimer()
{
  // Configure the Prescaler at 80 the quarter of the ESP32 is cadence at 80Mhz
   // 80000000 / 80 = 1000000 tics / seconde
  timer = timerBegin(0, 80, true);                
  timerAttachInterrupt(timer, &onTime, true);   
  //timerAlarmWrite(timer, 1000000, true);           
  timerAlarmDisable(timer); 
}
void updateTimer(hw_timer_t *timer, int ms)
{
  timerAlarmDisable(timer);
  timerAlarmWrite(timer, (1000 * ms), true);           
  timerAlarmEnable(timer); 
}
void led_driver()
{
  if(wificonnected == 0){
    updateTimer(timer, 500);
  }
  else if(mqttconfigured == 0){
    updateTimer(timer, 250);
  }
  else if((wificonnected == 1) && (mqttconfigured == 1)){
    updateTimer(timer, 500);
  }
}
int readVoltage()
{
  int ADC_VALUE = 0;
  int voltage_value = 0; 
  ADC_VALUE = analogRead(ADC_CHANNEL_PIN);
  Serial.print("ADC VALUE = ");
  Serial.println(ADC_VALUE);
  voltage_value = (ADC_VALUE * 3.3 ) / (4095);
  Serial.print("Voltage = ");
  Serial.print(voltage_value);
  Serial.print("volts");
  pCharacteristic_bat->setValue(voltage_value);
  pCharacteristic_bat->notify();
  return voltage_value;
}
///*********************************************** Entry Function **************************************///
void setup()
{
  pinMode(CONFIG_BUTTON, INPUT_PULLUP);
  pinMode(SLEEP_BUTTON, INPUT_PULLUP);
  pinMode(MAIN_LED, OUTPUT);
  pinMode(CONFIG_LED, OUTPUT);
  digitalWrite(MAIN_LED, HIGH);
  digitalWrite(CONFIG_LED, HIGH);
  esp_sleep_enable_ext0_wakeup(SLEEP_BUTTON, 0);

  EEPROM.begin(EEPROM_SIZE);
  config_mode = EEPROM.read(0);

  Serial.begin(9600);
  Serial.println(String("\nVersion: ") + VERSION);
  setupSpiffs();
  setup_ble();
  initTimer();
}
///*********************************************** Main loop **************************************///
void loop()
{
  // read the state of the switch/button:
  //Print the wakeup reason for ESP32
  //print_wakeup_reason();
  // Sets an alarm to sound every second
  led_driver();
  readVoltage();
  configBtnCurrentLevel = digitalRead(CONFIG_BUTTON);
  powerBtnCurrentLevel = digitalRead(SLEEP_BUTTON);
  // Serial.printf("config pin status: %d\n", configBtnCurrentLevel);
  if (configBtnPrevLevel == HIGH && configBtnCurrentLevel == LOW) // button is pressed
  {
    configBtnPressTime = millis();
  }
  else if (configBtnPrevLevel == LOW && configBtnCurrentLevel == HIGH)
  { // button is released
    if (abs(millis() - configBtnPressTime) > LONG_PRESS_TIME_CFG)
    {
      Serial.println("Config button is pressed");
      if (config_mode == 0)
      {
        config_mode = 1;
        Serial.println("Now is config-mode");
      }
      else
      {
        config_mode = 0;
        Serial.println("Now is work-mode");
      }
      EEPROM.write(0, config_mode);
      EEPROM.commit();
      requireRestart = true;
    }
  }
  // save the the last state
  configBtnPrevLevel = configBtnCurrentLevel;

  // deep sleep / wake up

  //Serial.printf("sleep pin status: %d\n", powerBtnCurrentLevel);
  if (powerBtnLastLevel == HIGH && powerBtnCurrentLevel == LOW) // button is pressed
  {
    powerBtnPressTime = millis();
  }
  if (powerBtnLastLevel == LOW && powerBtnCurrentLevel == HIGH)
  { // button is released
    if (abs(millis() - powerBtnPressTime) > LONG_PRESS_TIME_DS)
    {
      Serial.println("Sleep button is pressed");
      //Go to sleep now
      esp_deep_sleep_start();
    }
  }
  // save the the last state
  powerBtnLastLevel = powerBtnCurrentLevel;

  if (!bleConnected)
  { //check the BLE connection. if not, the mcu start advertising.
    pServer->startAdvertising();
    delay(500);
  }
  if (!wificonnected)
  { //setup the wifi connection. if not, the MCU try the WiFi connection.
    setup_wifi();
    delay(500);
  }
  else
  {
    server.handleClient();
  }
  if (config_mode == 0)
  {
    if (!mqttconfigured)
    { //setup mqtt server and port. if not, check the mqtt and device info and try the mqtt connection
      checkmqttinfo();
      delay(500);
    }

    if (wificonnected && mqttconfigured)
    { //check the mqtt status if disconnected, try to reconnect, if not, run the subscribe
      if (!requireRestart && !client.connected())
      {
        mqtt_connected = 0;
        long now = millis();
        if (now - lastReconnectAttempt > 10000)
        {
          lastReconnectAttempt = now;
          // Attempt to reconnect
          Serial.println("Attempt to connect");
          if (reconnect())
          {
            lastReconnectAttempt = 0;
          }
        }
      }
      else
      {
        mqtt_connected = 1;
        client.loop();
        if (receivefromble)
        {
          Serial.println("Buffer Data:");
          HexDump(Serial, buf, BUFFER_SIZE);
          String real_pub;
          real_pub = String(dev_id) + "/" + String(dev_path) + "/" + String(mqtt_pub);
          client.publish(real_pub.c_str(), buf, buf_len);
          //reset the buffer
          receivefromble = false;
          buf_len = 0;
          memset(buf, '\0', BUFFER_SIZE);
        }
      }
    }
  }
  if (requireRestart)
  {
    ESP.restart();
  }
}