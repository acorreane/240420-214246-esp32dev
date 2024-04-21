/* Teste ESP32 com webserver, wifi manager, ble e mqtt.
 * o controle de gpio's pelo webserver funciona após a configuração do wifi
 * o botão do gpio0 pressionado por 3s apaga as configurações mqtt iniciando
 * o gerenciador de wifi mas não apaga as config de wifi, para este é necessário
 * apagar pelo gerenciador.
 * as configurações mqtt são salvas na spiffs em formato json, caso esta não estiver
 * formatada será formatada no momento em que iniciar o wifi manager pelo gpio0.
 * o BLE recebe strings na interface de característa TX
 * para controlar as gpios definidas no codigo com a sintaxe
 * R11 ou R10 ligar/desligar rele1 associado a gpio na array de reles
 * R21 ou R20 ligar/desligar rele2 associado a gpio na array de reles
 * na interface RX do BLE é enviado uma string de um contador de uptime do esp
 * o software usado para a comunicação ble foi o nRF Connect.
 * na configuração do mqtt estão definidos 2 topicos sub e 2 publish para comando e estado
 * relay1_switch e relay2_switch para comando e relay1_state, relay2_state para estado
 * o tipo de dados em todas as interfaces são ascii mesmo sendo 1 ou 0.
 * o webserver faz um xml request a cada 2s para sincronizar o estado das gpios com o browser
 * ainda não foi implementado lista de controle de acesso ble e armazenamento da webpage na spiffs.
 * o buzzer usa a biblioteca nativa do arduino para gerar tom em uma callback de conexão ble.
 * o ble possui funcionalidade limitada a caracteristica TX a dispositivos mais modernos
 * RX é funcional em todos os dispositivos testados.
 * o binário compilado incluso está configurado para a ESP32 Dev Module, 3MB/1MB DIO 80M
 */

#include <Arduino.h>
#include <SPIFFS.h>
#include <FS.h>
#include "ArduinoJson.h" //lib não nativa da ide
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLE2902.h>
#include <WiFi.h>
#include "WebServer.h"
#include "WiFiManager.h"  //lib não nativa da ide
#include "PubSubClient.h" //lib não nativa da ide

uint32_t chipId;
uint32_t uptime;
uint32_t lastCheck;

char mqtt_clientid[8];

bool mqtt_enabled_c = false;

char *mqtt_server;
char *mqtt_user;
char *mqtt_pass;

char json_mqtt_server[24] = "";
char json_mqtt_user[24] = "";
char json_mqtt_pass[24] = "";

#define NUM_RELAYS 2
#define led_wifi 2
#define pin_config 0
#define pin_buzzer 4

uint8_t relayStates[NUM_RELAYS];
uint8_t relayGPIOs[NUM_RELAYS] = {2, 13};

#define SERVICE_UUID "240d5183-819a-4627-9ca9-1aa24df29f18"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

bool BLEdeviceConnected = false;
BLECharacteristic *characteristicTX;
BLEScan *pBLEScan;

class ServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *pServer)
  {
    BLEdeviceConnected = true;
    tone(pin_buzzer, 1000, 500);
    Serial.println("BLE device connected.");
  };
  void onDisconnect(BLEServer *pServer)
  {
    BLEdeviceConnected = false;
    Serial.println("BLE device disconnected.");
    pServer->getAdvertising()->start();
  }
};

class CharacteristicCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *characteristic)
  {
    std::string rxValue = characteristic->getValue();
    if (rxValue.length() > 0)
    {
      char charbuf[8];
      for (int i = 0; i < rxValue.length(); i++)
      {
        charbuf[i] = rxValue[i];
      }
      Serial.print("BLE Received: ");
      Serial.println(charbuf);
      // Relay 1
      if (rxValue.find("R11") != -1)
      {
        digitalWrite(relayGPIOs[0], 1);
      }
      else if (rxValue.find("R10") != -1)
      {
        digitalWrite(relayGPIOs[0], 0);
      }
      // Relay 2
      else if (rxValue.find("R21") != -1)
      {
        digitalWrite(relayGPIOs[1], 1);
      }
      else if (rxValue.find("R20") != -1)
      {
        digitalWrite(relayGPIOs[1], 0);
      }
    }
  }
};

class BtAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks
{
  void onResult(BLEAdvertisedDevice advertisedDevice)
  {
    // Serial.printf("Advertised Device: %s, RSSI: %d \n", advertisedDevice.getAddress().toString().c_str(), advertisedDevice.getRSSI());
  }
};

WebServer server(80);
WiFiClient mqtt_client;
PubSubClient mqtt(mqtt_client);

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    html {font-family: Arial; display: inline-block; text-align: center;}
    h2 {font-size: 3.0rem;}
    p {font-size: 3.0rem;}
    body {max-width: 600px; margin:0px auto; padding-bottom: 25px;}
    .switch {position: relative; display: inline-block; width: 120px; height: 68px} 
    .switch input {display: none}
    .slider {position: absolute; top: 0; left: 0; right: 0; bottom: 0; background-color: #ccc; border-radius: 34px}
    .slider:before {position: absolute; content: ""; height: 52px; width: 52px; left: 8px; bottom: 8px; background-color: #fff; -webkit-transition: .4s; transition: .4s; border-radius: 68px}
    input:checked+.slider {background-color: #2196F3}
    input:checked+.slider:before {-webkit-transform: translateX(52px); -ms-transform: translateX(52px); transform: translateX(52px)}
  </style>
</head>
<body>
  <h2>ESP32 WS+BLE+MQTT</h2>
<script>
function toggleCheckbox(element) {
  var xhr = new XMLHttpRequest();
  if(element.checked){ xhr.open("GET", "/relay"+element.id+"1", true); }
  else { xhr.open("GET", "/relay"+element.id+"0", true); }
  xhr.send();
}

setInterval(function ( ) {
  var xhttp = new XMLHttpRequest();
  xhttp.onreadystatechange = function() {
    if (this.readyState == 4 && this.status == 200) {
      var inputChecked1;
      var inputChecked2;
      if (this.responseText.includes('R11')) { inputChecked1 = true; }
      if (this.responseText.includes('R10')) { inputChecked1 = false; }
      if (this.responseText.includes('R21')) { inputChecked2 = true; }
      if (this.responseText.includes('R20')) { inputChecked2 = false; }
  
      document.getElementById("1").checked = inputChecked1;
      document.getElementById("2").checked = inputChecked2;
    }
  };
  xhttp.open("GET", "/state", true);
  xhttp.send();
}, 2000 ) ;

</script>
</body>
</html>
)rawliteral";

String relayState(int numRelay)
{
  if (digitalRead(relayGPIOs[numRelay - 1]))
  {
    return "checked";
  }
  else
  {
    return "";
  }
}

String processor()
{
  String buttons = "";
  for (int i = 1; i <= NUM_RELAYS; i++)
  {
    String relayStateValue = relayState(i);
    buttons += "<h4>Relay #" + String(i) + " - GPIO " + relayGPIOs[i - 1] + "</h4><label class=\"switch\"><input type=\"checkbox\" onchange=\"toggleCheckbox(this)\" id=\"" + String(i) + "\" " + relayStateValue + "><span class=\"slider\"></span></label>";
  }
  return index_html + buttons;
}

void handleRoot() { server.send(200, "text/html", processor()); }
void handleState()
{

  char statebuffer[] = "00";
  digitalRead(relayGPIOs[0]) ? statebuffer[0] = '1' : statebuffer[0] = '0';
  digitalRead(relayGPIOs[1]) ? statebuffer[1] = '1' : statebuffer[1] = '0';

  char stringbuffer[] = "R1 R2 ";
  stringbuffer[2] = statebuffer[0];
  stringbuffer[5] = statebuffer[1];

  server.send(200, "text/plain", stringbuffer);
}
void handleRelay1on()
{
  digitalWrite(relayGPIOs[0], 1);
  server.send(200, "text/plain", "OK");
}
void handleRelay1off()
{
  digitalWrite(relayGPIOs[0], 0);
  server.send(200, "text/plain", "OK");
}
void handleRelay2on()
{
  digitalWrite(relayGPIOs[1], 1);
  server.send(200, "text/plain", "OK");
}
void handleRelay2off()
{
  digitalWrite(relayGPIOs[1], 0);
  server.send(200, "text/plain", "OK");
}

void reconnect()
{
  if (!mqtt.connected())
  {
    digitalWrite(led_wifi, !digitalRead(led_wifi));

    if (mqtt.connect(mqtt_clientid, mqtt_user, mqtt_pass))
    {
      digitalWrite(led_wifi, 1);

      mqtt.subscribe("relay1_switch");
      mqtt.subscribe("relay2_switch");
    }
  }
}

bool readConfigFile()
{
  File f = SPIFFS.open("/config.json");

  if (!f)
  {
    return false;
  }
  else
  {
    size_t size = f.size();
    std::unique_ptr<char[]> buf(new char[size]);

    f.readBytes(buf.get(), size);
    f.close();

    DynamicJsonBuffer jsonBuffer;

    JsonObject &json = jsonBuffer.parseObject(buf.get());

    if (json.containsKey("mqttserver"))
    {
      strcpy(json_mqtt_server, json["mqttserver"]);
    }
    if (json.containsKey("mqttuser"))
    {
      strcpy(json_mqtt_user, json["mqttuser"]);
    }
    if (json.containsKey("mqttpass"))
    {
      strcpy(json_mqtt_pass, json["mqttpass"]);
    }
    if (!json.success())
    {
      return false;
    }
  }
  return true;
}

bool writeConfigFile()
{
  DynamicJsonBuffer jsonBuffer;

  JsonObject &json = jsonBuffer.createObject();

  json["mqttserver"] = json_mqtt_server;
  json["mqttuser"] = json_mqtt_user;
  json["mqttpass"] = json_mqtt_pass;

  File f = SPIFFS.open("/config.json", FILE_WRITE);

  if (!f)
  {
    return false;
  }

  json.printTo(f);
  f.close();

  return true;
}

void led_format()
{
  for (uint8_t i; i < 8; i++)
  {
    digitalWrite(led_wifi, !digitalRead(led_wifi));
    delay(50);
  }
}

void mqttcallback(char *topic, byte *payload, unsigned int length)
{
  int i;
  char buffer[length];
  for (i = 0; i < length; i++)
  {
    buffer[i] = payload[i];
  }

  Serial.print("MQTT Topic:");
  Serial.println(topic);
  Serial.print("MQTT Payload:");
  Serial.println(buffer);

  if (!strcmp(topic, "relay1_switch") && buffer[0] == '1')
  {
    digitalWrite(relayGPIOs[0], 1);
  }
  if (!strcmp(topic, "relay1_switch") && buffer[0] == '0')
  {
    digitalWrite(relayGPIOs[0], 0);
  }

  if (!strcmp(topic, "relay2_switch") && buffer[0] == '1')
  {
    digitalWrite(relayGPIOs[1], 1);
  }
  if (!strcmp(topic, "relay2_switch") && buffer[0] == '0')
  {
    digitalWrite(relayGPIOs[1], 0);
  }
}

void manager_call()
{
  if (!digitalRead(pin_config))
  {
    server.stop();
    if (!SPIFFS.begin(true))
    {
      SPIFFS.format();
      Serial.println("Formatting Spiffs, please wait...");
    }
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    digitalWrite(led_wifi, HIGH);
    delay(1000);
    SPIFFS.remove("/config.json");
    led_format();

    WiFiManagerParameter p_mqtt_server("mqttserver", "MQTT Server IP/Domain", json_mqtt_server, 24);
    WiFiManagerParameter p_mqtt_user("mqttuser", "MQTT User", json_mqtt_user, 24);
    WiFiManagerParameter p_mqtt_pass("mqttpass", "MQTT Pass", json_mqtt_pass, 24);
    WiFiManager wifiManager;
    wifiManager.autoConnect();

    wifiManager.addParameter(&p_mqtt_server);
    wifiManager.addParameter(&p_mqtt_user);
    wifiManager.addParameter(&p_mqtt_pass);

    wifiManager.startConfigPortal();

    strcpy(json_mqtt_server, p_mqtt_server.getValue());
    strcpy(json_mqtt_user, p_mqtt_user.getValue());
    strcpy(json_mqtt_pass, p_mqtt_pass.getValue());
    if (writeConfigFile())
    {
      Serial.println("Config Saved, Restarting...");
    }
    ESP.restart();
  }
}

void check_reset()
{
  if (!digitalRead(pin_config))
  {
    delay(3000);
    manager_call();
  }
}

void start_services()
{
  server.begin();
  Serial.println("Starting Webserver.");

  if (mqtt_enabled_c)
  {
    mqtt.setServer(mqtt_server, 1883);
    mqtt.setCallback(mqttcallback);
    Serial.println("Starting MQTT Client.");
  }
  server.on("/", handleRoot);
  server.on("/state", handleState);
  server.on("/relay11", handleRelay1on);
  server.on("/relay10", handleRelay1off);
  server.on("/relay21", handleRelay2on);
  server.on("/relay20", handleRelay2off);
}

void setup_wifi()
{
  delay(1000);
  if (WiFi.status() != WL_CONNECTED)
  {
    WiFi.begin();
    WiFi.mode(WIFI_STA);
    Serial.println("Trying to connect Wifi...");
  }
  while (WiFi.status() != WL_CONNECTED)
  {
    digitalWrite(led_wifi, !digitalRead(led_wifi));
    delay(100);
    if (!digitalRead(pin_config))
    {
      manager_call();
    }
  }
  Serial.print("Wifi Connected, IP: ");
  Serial.println(WiFi.localIP());
  start_services();
}

void setup_ble()
{
  BLEDevice::init("ESP32-BLE");

  BLEScan *pBLEScan = BLEDevice::getScan();
  BLEServer *btserver = BLEDevice::createServer();
  BLEService *btservice = btserver->createService(SERVICE_UUID);

  btserver->setCallbacks(new ServerCallbacks());

  characteristicTX = btservice->createCharacteristic(
      CHARACTERISTIC_UUID_TX,
      BLECharacteristic::PROPERTY_NOTIFY);

  characteristicTX->addDescriptor(new BLE2902());
  BLECharacteristic *characteristic = btservice->createCharacteristic(
      CHARACTERISTIC_UUID_RX,
      BLECharacteristic::PROPERTY_WRITE);

  characteristic->setCallbacks(new CharacteristicCallbacks());

  pBLEScan->setAdvertisedDeviceCallbacks(new BtAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true); // active scan uses more power, but get results faster
  pBLEScan->setInterval(0x50);
  pBLEScan->setWindow(0x30);

  Serial.println("Performing BLE Scan...");
  BLEScanResults foundDevices = pBLEScan->start(5);
  int count = foundDevices.getCount();

  for (int i = 0; i < count; i++)
  {
    BLEAdvertisedDevice d = foundDevices.getDevice(i);
    String currDevAddr = d.getAddress().toString().c_str();
    Serial.print("BLE Found device: ");
    Serial.println(currDevAddr);
  }

  btservice->start();
  btserver->getAdvertising()->start();

  Serial.println("BLE is ready.");
}

void setup()
{
  Serial.begin(115200);
  for (int i = 0; i < 17; i = i + 8)
  {
    chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
  }
  for (int i = 1; i <= NUM_RELAYS; i++)
  {
    pinMode(relayGPIOs[i - 1], OUTPUT);
  }

  itoa(chipId, mqtt_clientid, DEC);

  pinMode(led_wifi, OUTPUT);
  digitalWrite(led_wifi, 1);

  SPIFFS.begin();
  delay(100);

  bool mqtt_config = readConfigFile();

  mqtt_server = json_mqtt_server;
  mqtt_user = json_mqtt_user;
  mqtt_pass = json_mqtt_pass;

  mqtt_config ? mqtt_enabled_c = true : mqtt_enabled_c = false;

  check_reset();
  setup_ble();
  setup_wifi();
}

void loop()
{
  long now = millis();
  server.handleClient();

  if (mqtt_enabled_c)
  {
    mqtt.loop();
  }

  if (now - lastCheck > 1000)
  {
    lastCheck = now;
    uptime++;
    if (!digitalRead(pin_config))
    {
      manager_call();
    }

    if (WiFi.status() != WL_CONNECTED)
    {
      setup_wifi();
    }
    if (!mqtt.connected() && (mqtt_enabled_c))
    {
      reconnect();
    }

    char charbuf[1];
    if (relayStates[0] != digitalRead(relayGPIOs[0]))
    {
      relayStates[0] = digitalRead(relayGPIOs[0]);
      itoa(relayStates[0], charbuf, DEC);
      Serial.print("Relay1 State Changed to: ");
      Serial.println(charbuf);
      if (mqtt.connected())
      {
        mqtt.publish("relay1_state", charbuf);
      }
    }

    if (relayStates[1] != digitalRead(relayGPIOs[1]))
    {
      relayStates[1] = digitalRead(relayGPIOs[1]);
      itoa(relayStates[1], charbuf, DEC);
      Serial.print("Relay2 State Changed to: ");
      Serial.println(charbuf);
      if (mqtt.connected())
      {
        mqtt.publish("relay2_state", charbuf);
      }
    }

    if (BLEdeviceConnected)
    {
      char txString[8];
      dtostrf(uptime, 0, 0, txString);
      characteristicTX->setValue(txString);
      characteristicTX->notify();
    }
  }
}