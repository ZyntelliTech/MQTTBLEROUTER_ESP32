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

#define SERVICE_UUID "80f20f81-82af-4df8-9bf0-520f4997cca6"
#define CONFIGURE_UUID "80f20f82-82af-4df8-9bf0-520f4997cca6"
#define MQTT_UUID "80f20f83-82af-4df8-9bf0-520f4997cca6"

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

WebServer server(80);

const char *host = "esp32";
char ssid[32] = "";
char passwd[32] = "";
uint8_t buf[BUFFER_SIZE] = {0x00};
uint8_t buf_len = 0;

char mqtt_server[64] = "";
int mqtt_port = 1883;
char mqtt_pub[16] = "";
char mqtt_sub[16] = "";
char dev_name[37] = ""; // One byte more to have a \0 in the end of var
char dev_id[37] = "";   // One byte more to have a \0 in the end of var
char dev_path[37] = ""; // One byte more to have a \0 in the end of var

WiFiClient espClient;
PubSubClient client(espClient);
long lastMsg = 0;
char msg[50];
int value = 0;

long lastReconnectAttempt = 0;

bool bleConnected = false;
BLEServer *pServer = NULL;
BLECharacteristic *pCharacteristic_conf;
BLECharacteristic *pCharacteristic_mqtt;

bool wificonnected = false;
bool mqttconfigured = false;
bool receivefromble = false;
bool transfertoble = false;

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
class MyServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *pServer)
  {
    bleConnected = true;
    Serial.println("connected");
  };

  void onDisconnect(BLEServer *pServer)
  {
    bleConnected = false;
    Serial.println("start advertising");
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
void storeConfig()
{
  Serial.println(">>>>> writing the config json.....");
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
        json.printTo(Serial);
        if (json.success())
        {
          Serial.println("\nparsed json");
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
            Serial.println("failed to open config file for writing");
          }
          json.prettyPrintTo(Serial);
          json.printTo(configFile);
          configFile.close();
        }
        else
        {
          Serial.println("failed to load json config");
          configFile.close();
        }
      }
    }
    else
    {
      Serial.println("config json is not existed.");
    }
    SPIFFS.end();
  }
  else
  {
    Serial.println("SPIFFS Mount Failed");
  }
}
void setupSpiffs()
{
  //clean FS, for testing
  // SPIFFS.format();
  //read configuration from FS json
  Serial.println(">>> mounting FS");

  if (SPIFFS.begin(true))
  {
    Serial.println("...mounted file system");
    if (SPIFFS.exists("/config.json"))
    {
      //file exists, reading and loading
      Serial.println("...reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile)
      {
        Serial.println("...opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buffer(new char[size]);
        configFile.readBytes(buffer.get(), size);

        DynamicJsonBuffer jsonBuffer;
        JsonObject &config_json = jsonBuffer.parseObject(buffer.get());
        config_json.printTo(Serial);
        if (config_json.success())
        {
          Serial.println("\n...parsed json");
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
          Serial.println("...loaded json config");
        }
        else
        {
          Serial.println("failed to load json config");
        }
      }
    }
    else
    {
      Serial.println("...config is not existed. create config");

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
        Serial.println("failed to open config file for writing");
      }

      config_json.prettyPrintTo(Serial);
      config_json.printTo(configFile);
      Serial.println("created config json.");
      configFile.close();
      //end save
    }
    SPIFFS.end();
  }
  else
  {
    Serial.println("SPIFFS Mount Failed");
  }
  //end read
}
uint8_t setup_wifi()
{
  // We start by connecting to a WiFi network
  wificonnected = false;
  if ((ssid[0] == '\0') || (passwd[0] == '\0'))
  {
    Serial.println("The ssid and password is incorrectly");
    return 0;
  }
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.setHostname("ESPRESSIF");
  WiFi.begin(ssid, passwd);
  long now = millis();
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    if ((millis() - now) > 10000)
    {
      Serial.println("Cannot connect to the WiFi Network. Please check the ssid and password again and then reset the MCU.\n");
      Serial.print("ssid: ");
      Serial.print(ssid);
      Serial.print("\tpassword: ");
      Serial.println(passwd);
      return 0;
    }
  }
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  wificonnected = true;
  setup_webserver();
  return 1;
}

void setup_ble()
{
  BLEDevice::init("MyESP32");
  pServer = BLEDevice::createServer();

  BLEService *pService = pServer->createService(SERVICE_UUID);
  pServer->setCallbacks(new MyServerCallbacks());

  pCharacteristic_conf = pService->createCharacteristic(
      CONFIGURE_UUID,
      BLECharacteristic::PROPERTY_READ |
          BLECharacteristic::PROPERTY_WRITE);
  pCharacteristic_conf->setCallbacks(new MyCallbacksForConfig());
  pCharacteristic_conf->setValue("configuration");

  pCharacteristic_mqtt = pService->createCharacteristic(
      MQTT_UUID,
      BLECharacteristic::PROPERTY_READ |
          BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_NOTIFY |
          BLECharacteristic::PROPERTY_INDICATE);
  pCharacteristic_mqtt->addDescriptor(new BLE2902());
  pCharacteristic_mqtt->setCallbacks(new MyCallbacksForMqtt());
  pCharacteristic_mqtt->setValue("mqtt_router");

  pService->start();

  BLEAdvertising *pAdvertising = pServer->getAdvertising();
  pAdvertising->start();
}

/**** mqtt callback function ****/
void callback(char *topic, byte *message, unsigned int length)
{
  Serial.print(">>>>> Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
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
  if (client.connect(dev_id))
  {
    Serial.println("mqtt connecting...");
    String real_sub;
    real_sub = String(dev_id) + "/" + String(dev_path) + "/" + String(mqtt_sub);
    client.subscribe(real_sub.c_str());
    Serial.println("mqtt is now connected!");
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
    Serial.println("Please configure the mqtt and device info correctly");
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
      Serial.println("MDNS responder started");
    }
    MDNS.addService("_http", "_tcp", 80);
    /* Config API */
    server.on("/write", HTTP_POST, writeConfigApi);
    server.on("/read", HTTP_GET, getConfigApi);

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
          ESP.restart();
        },
        []()
        {
          HTTPUpload &upload = server.upload();
          if (upload.status == UPLOAD_FILE_START)
          {
            Serial.printf("Update: %s\n", upload.filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN))
            { //start with max available size
              Update.printError(Serial);
            }
          }
          else if (upload.status == UPLOAD_FILE_WRITE)
          {
            /* flashing firmware to ESP*/
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
            {
              Update.printError(Serial);
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
            }
          }
        });
    server.begin();
    Serial.println("Start the WebServer");
  }
}

///*********************************************** Entry Function **************************************///
void setup()
{
  // put your setup code here, to run once:
  Serial.begin(9600);
  setupSpiffs();
  setup_ble();
}
///*********************************************** Main loop **************************************///
void loop()
{
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
  if (!mqttconfigured)
  { //setup mqtt server and port. if not, check the mqtt and device info and try the mqtt connection
    checkmqttinfo();
    delay(500);
  }
  if (wificonnected && mqttconfigured)
  { //check the mqtt status if disconnected, try to reconnect, if not, run the subscribe
    if (!client.connected())
    {
      long now = millis();
      if (now - lastReconnectAttempt > 2000)
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