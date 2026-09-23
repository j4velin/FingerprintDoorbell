/***************************************************
  Main of FingerprintDoorbell 
 ****************************************************/

#include <ETH.h>
#include <ESPAsyncWebServer.h>
#include <AsyncElegantOTA.h>
#include <SPIFFS.h>
#include <PubSubClient.h>
#include "FingerprintManager.h"
#include "SettingsManager.h"
#include "global.h"

enum class Mode { scan, enroll, maintenance };

const char* VersionInfo = "0.5.0";
const char* deviceHostname = "FingerprintDoorbell"; // also used as the MQTT client id

const int doorbellPin = 14; // doorbell button
bool doorbellPressed = false;
unsigned long missedRingMillis = 0; // when a ring could not be published because MQTT was down (0 = nothing pending)
const unsigned long missedRingMaxAge = 5UL * 60UL * 1000UL; // don't replay a ring older than this

const int buzzerPin = 15; // buzzer when the doorbell button is pressed

const int logMessagesCount = 5;
String logMessages[logMessagesCount]; // log messages, 0=most recent log message
bool shouldReboot = false;
unsigned long mqttReconnectPreviousMillis = 0;
volatile bool ethHasIp = false;             // set from the ETH event task
volatile bool mqttReconnectPending = true;  // ask loop() to try a connect without waiting for the retry interval
bool mqttDnsFailureReported = false;        // avoid repeating the same DNS warning every 30 seconds

String enrollId;
String enrollName;
Mode currentMode = Mode::scan;

FingerprintManager fingerManager;
SettingsManager settingsManager;
bool needMaintenanceMode = false;

AsyncWebServer webServer(80); // AsyncWebServer  on port 80
AsyncEventSource events("/events"); // event source (Server-Sent events)

WiFiClient espClient;
PubSubClient mqttClient(espClient);
bool mqttConfigValid = true;


Match lastMatch;

void addLogMessage(const String& message) {
  // shift all messages in array by 1, oldest message will die
  for (int i=logMessagesCount-1; i>0; i--)
    logMessages[i]=logMessages[i-1];
  logMessages[0]=message;
}

String getLogMessagesAsHtml() {
  String html = "";
  for (int i=logMessagesCount-1; i>=0; i--) {
    if (logMessages[i]!="")
      html = html + logMessages[i] + "<br>";
  }
  return html;
}

/* wait for maintenance mode or timeout 5s */
bool waitForMaintenanceMode() {
  needMaintenanceMode = true;
  unsigned long startMillis = millis();
  while (currentMode != Mode::maintenance) {
    if ((millis() - startMillis) >= 5000ul) {
      needMaintenanceMode = false;
      return false;
    }
    delay(50);
  }
  needMaintenanceMode = false;
  return true;
}

// Replaces placeholder in HTML pages
String processor(const String& var){
  if(var == "LOGMESSAGES"){
    return getLogMessagesAsHtml();
  } else if (var == "FINGERLIST") {
    return fingerManager.getFingerListAsHtmlOptionList();
  } else if (var == "HOSTNAME") {
    return deviceHostname;
  } else if (var == "VERSIONINFO") {
    return VersionInfo;
  } else if (var == "MQTT_SERVER") {
    return settingsManager.getAppSettings().mqttServer;
  } else if (var == "MQTT_USERNAME") {
    return settingsManager.getAppSettings().mqttUsername;
  } else if (var == "MQTT_PASSWORD") {
    return settingsManager.getAppSettings().mqttPassword;
  } else if (var == "MQTT_ROOTTOPIC") {
    return settingsManager.getAppSettings().mqttRootTopic;
  }

  return String();
}


// send LastMessage to websocket clients
void notifyClients(String message) {
  Serial.println(message);
  addLogMessage(message);
  events.send(getLogMessagesAsHtml().c_str(),"message",millis(),1000);
  
  String mqttRootTopic = settingsManager.getAppSettings().mqttRootTopic;
  mqttClient.publish((String(mqttRootTopic) + "/lastLogMessage").c_str(), message.c_str());
}

void updateClientsFingerlist(String fingerlist) {
  Serial.println("New fingerlist was sent to clients");
  events.send(fingerlist.c_str(),"fingerlist",millis(),1000);
}


bool doPairing() {
  String newPairingCode = settingsManager.generateNewPairingCode();

  if (fingerManager.setPairingCode(newPairingCode)) {
    AppSettings settings = settingsManager.getAppSettings();
    settings.sensorPairingCode = newPairingCode;
    settings.sensorPairingValid = true;
    settingsManager.saveAppSettings(settings);
    notifyClients("Pairing successful.");
    return true;
  } else {
    notifyClients("Pairing failed.");
    return false;
  }

}


bool checkPairingValid() {
  AppSettings settings = settingsManager.getAppSettings();

   if (!settings.sensorPairingValid) {
     if (settings.sensorPairingCode.isEmpty()) {
       // first boot, do pairing automatically so the user does not have to do this manually
       return doPairing();
     } else {
      Serial.println("Pairing has been invalidated previously.");   
      return false;
     }
   }

  String actualSensorPairingCode = fingerManager.getPairingCode();
  //Serial.println("Awaited pairing code: " + settings.sensorPairingCode);
  //Serial.println("Actual pairing code: " + actualSensorPairingCode);

  if (actualSensorPairingCode.equals(settings.sensorPairingCode))
    return true;
  else {
    if (!actualSensorPairingCode.isEmpty()) { 
      // An empty code means there was a communication problem. So we don't have a valid code, but maybe next read will succeed and we get one again.
      // But here we just got an non-empty pairing code that was different to the awaited one. So don't expect that will change in future until repairing was done.
      // -> invalidate pairing for security reasons
      // reuse the copy loaded above, just flip the flag and persist it
      settings.sensorPairingValid = false;
      settingsManager.saveAppSettings(settings);
    }
    return false;
  }
}

void startWebserver(){
  
  // Initialize SPIFFS
  if(!SPIFFS.begin(true)){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  // =======================
  // normal operating mode
  // =======================
  events.onConnect([](AsyncEventSourceClient *client){
    if(client->lastId()){
      Serial.printf("Client reconnected! Last message ID it got was: %u\n", client->lastId());
    }
    //send event with message "ready", id current millis
    // and set reconnect delay to 1 second
    client->send(getLogMessagesAsHtml().c_str(),"message",millis(),1000);
  });
  webServer.addHandler(&events);

  
  // Route for root / web page
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/index.html", String(), false, processor);
  });

  webServer.on("/enroll", HTTP_GET, [](AsyncWebServerRequest *request){
    if(request->hasArg("startEnrollment"))
    {
      enrollId = request->arg("newFingerprintId");
      enrollName = request->arg("newFingerprintName");
      currentMode = Mode::enroll;
    }
    request->redirect("/");
  });

  webServer.on("/editFingerprints", HTTP_GET, [](AsyncWebServerRequest *request){
    if(request->hasArg("selectedFingerprint"))
    {
      if(request->hasArg("btnDelete"))
      {
        int id = request->arg("selectedFingerprint").toInt();
        waitForMaintenanceMode();
        fingerManager.deleteFinger(id);
        currentMode = Mode::scan;
      }
      else if (request->hasArg("btnRename"))
      {
        int id = request->arg("selectedFingerprint").toInt();
        String newName = request->arg("renameNewName");
        fingerManager.renameFinger(id, newName);
      }
    }
    request->redirect("/");  
  });

  webServer.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request){
    if(request->hasArg("btnSaveSettings"))
    {
      Serial.println("Save settings");
      AppSettings settings = settingsManager.getAppSettings();
      settings.mqttServer = request->arg("mqtt_server");
      settings.mqttUsername = request->arg("mqtt_username");
      settings.mqttPassword = request->arg("mqtt_password");
      settings.mqttRootTopic = request->arg("mqtt_rootTopic");
      settingsManager.saveAppSettings(settings);
      request->redirect("/");  
      shouldReboot = true;
    } else {
      request->send(SPIFFS, "/settings.html", String(), false, processor);
    }
  });


  webServer.on("/pairing", HTTP_GET, [](AsyncWebServerRequest *request){
    if(request->hasArg("btnDoPairing"))
    {
      Serial.println("Do (re)pairing");
      doPairing();
      request->redirect("/");  
    } else {
      request->send(SPIFFS, "/settings.html", String(), false, processor);
    }
  });



  webServer.on("/factoryReset", HTTP_GET, [](AsyncWebServerRequest *request){
    if(request->hasArg("btnFactoryReset"))
    {
      notifyClients("Factory reset initiated...");
      
      if (!fingerManager.deleteAll())
        notifyClients("Finger database could not be deleted.");
      
      if (!settingsManager.deleteAppSettings())
        notifyClients("App settings could not be deleted.");
      
      request->redirect("/");  
      shouldReboot = true;
    } else {
      request->send(SPIFFS, "/settings.html", String(), false, processor);
    }
  });


  webServer.on("/deleteAllFingerprints", HTTP_GET, [](AsyncWebServerRequest *request){
    if(request->hasArg("btnDeleteAllFingerprints"))
    {
      notifyClients("Deleting all fingerprints...");
      
      if (!fingerManager.deleteAll())
        notifyClients("Finger database could not be deleted.");
      
      request->redirect("/");  
      
    } else {
      request->send(SPIFFS, "/settings.html", String(), false, processor);
    }
  });


  webServer.onNotFound([](AsyncWebServerRequest *request){
    request->send(404);
  });

    
  // end normal operating mode


  // common url callbacks
  webServer.on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request){
    request->redirect("/");
    shouldReboot = true;
  });

  webServer.on("/bootstrap.min.css", HTTP_GET, [](AsyncWebServerRequest *request){
    // the file is stored pre-compressed to save flash and to speed up page loads over SPIFFS
    AsyncWebServerResponse *response = request->beginResponse(SPIFFS, "/bootstrap.min.css.gz", "text/css");
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "max-age=604800"); // static asset, only changes on a filesystem upload
    request->send(response);
  });


  // Enable Over-the-air updates at http://<IPAddress>/update
  AsyncElegantOTA.begin(&webServer);
  
  // Start server
  webServer.begin();

  notifyClients("System booted successfully!");

}


void mqttCallback(char* topic, byte* message, unsigned int length) {
  Serial.print("Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
  String messageTemp;
  
  for (int i = 0; i < length; i++) {
    Serial.print((char)message[i]);
    messageTemp += (char)message[i];
  }
  Serial.println();

  // Check incomming message for interesting topics
  if (String(topic) == String(settingsManager.getAppSettings().mqttRootTopic) + "/ignoreTouchRing") {
    if(messageTemp == "on"){
      fingerManager.setIgnoreTouchRing(true);
    }
    else if(messageTemp == "off"){
      fingerManager.setIgnoreTouchRing(false);
    }
  }
}

/* (Re)connects to the MQTT broker. Safe to call repeatedly, returns quickly if there is nothing to do. */
void connectMqttClient() {
  if (mqttClient.connected() || !mqttConfigValid || !ethHasIp)
    return;

  AppSettings settings = settingsManager.getAppSettings();
  if (settings.mqttServer.isEmpty())
    return;

  // Resolve on every attempt: at boot the network is often not up yet, and the broker's
  // address may change while we are running.
  IPAddress mqttServerIp;
  if (!WiFi.hostByName(settings.mqttServer.c_str(), mqttServerIp)) {
    if (!mqttDnsFailureReported) {
      mqttDnsFailureReported = true; // only warn once, we keep retrying in the background
      notifyClients("MQTT Server '" + settings.mqttServer + "' could not be resolved. Retrying every 30 seconds.");
    } else {
      Serial.println("MQTT server still not resolvable, will retry.");
    }
    return;
  }
  mqttDnsFailureReported = false;

  Serial.print("(Re)connect to MQTT broker at ");
  Serial.print(mqttServerIp);
  Serial.print("...");
  mqttClient.setServer(mqttServerIp, 1883);

  String lastWillTopic = settings.mqttRootTopic + "/lastLogMessage";
  String lastWillMessage = "FingerprintDoorbell disconnected unexpectedly";
  bool connectResult;

  // connect with or witout authentication
  if (settings.mqttUsername.isEmpty() || settings.mqttPassword.isEmpty())
    connectResult = mqttClient.connect(deviceHostname, lastWillTopic.c_str(), 1, false, lastWillMessage.c_str());
  else
    connectResult = mqttClient.connect(deviceHostname, settings.mqttUsername.c_str(), settings.mqttPassword.c_str(), lastWillTopic.c_str(), 1, false, lastWillMessage.c_str());

  if (connectResult) {
    // success
    Serial.println("connected");
    // Subscribe
    mqttClient.subscribe((settings.mqttRootTopic + "/ignoreTouchRing").c_str(), 1); // QoS = 1 (at least once)
    notifyClients("Connected to MQTT broker.");

    // Resync the ring topic, and replay a press that happened while we were disconnected.
    // Publishing "on" followed by the current state turns a missed press into a pulse; if
    // nothing was missed this just repairs a ring state the broker may have missed.
    String ringTopic = settings.mqttRootTopic + "/ring";
    if (missedRingMillis != 0 && (millis() - missedRingMillis) < missedRingMaxAge) {
      mqttClient.publish(ringTopic.c_str(), "on");
      notifyClients("Replayed a doorbell ring that happened while MQTT was disconnected.");
    }
    missedRingMillis = 0;
    mqttClient.publish(ringTopic.c_str(), doorbellPressed ? "on" : "off");
  } else {
    if (mqttClient.state() == 4 || mqttClient.state() == 5) {
      mqttConfigValid = false;
      notifyClients("Failed to connect to MQTT Server: bad credentials or not authorized. Will not try again, please check your settings.");
    } else {
      notifyClients(String("Failed to connect to MQTT Server, rc=") + mqttClient.state() + ", try again in 30 seconds");
    }
  }
}


void doScan()
{
  Match match = fingerManager.scanFingerprint();
  String mqttRootTopic = settingsManager.getAppSettings().mqttRootTopic;
  switch(match.scanResult)
  {
    case ScanResult::noFinger:
      // standard case, occurs every iteration when no finger touchs the sensor
      if (match.scanResult != lastMatch.scanResult) {
        Serial.println("no finger");
        mqttClient.publish((String(mqttRootTopic) + "/matchId").c_str(), "-1");
        mqttClient.publish((String(mqttRootTopic) + "/matchName").c_str(), "");
        mqttClient.publish((String(mqttRootTopic) + "/matchConfidence").c_str(), "-1");
      }
      break; 
    case ScanResult::matchFound:
      notifyClients( String("Match Found: ") + match.matchId + " - " + match.matchName  + " with confidence of " + match.matchConfidence );
      if (match.scanResult != lastMatch.scanResult) {
        if (checkPairingValid()) {
          mqttClient.publish((String(mqttRootTopic) + "/matchId").c_str(), String(match.matchId).c_str());
          mqttClient.publish((String(mqttRootTopic) + "/matchName").c_str(), match.matchName.c_str());
          mqttClient.publish((String(mqttRootTopic) + "/matchConfidence").c_str(), String(match.matchConfidence).c_str());
          Serial.println("MQTT message sent: Open the door!");
        } else {
          notifyClients("Security issue! Match was not sent by MQTT because of invalid sensor pairing! This could potentially be an attack! If the sensor is new or has been replaced by you do a (re)pairing in settings page.");
        }
      }
      delay(3000); // wait some time before next scan to let the LED blink
      break;
    case ScanResult::noMatchFound:
      notifyClients(String("No Match Found (Code ") + match.returnCode + ")");
      if (match.scanResult != lastMatch.scanResult) {
        mqttClient.publish((String(mqttRootTopic) + "/matchId").c_str(), "-1");
        mqttClient.publish((String(mqttRootTopic) + "/matchName").c_str(), "");
        mqttClient.publish((String(mqttRootTopic) + "/matchConfidence").c_str(), "-1");
      } else {
        delay(1000); // wait some time before next scan to let the LED blink
      }
      break;
    case ScanResult::error:
      notifyClients(String("ScanResult Error (Code ") + match.returnCode + ")");
      break;
  };
  lastMatch = match;

}

void doEnroll()
{
  int id = enrollId.toInt();
  if (id < 1 || id > 200) {
    notifyClients("Invalid memory slot id '" + enrollId + "'");
    return;
  }

  NewFinger finger = fingerManager.enrollFinger(id, enrollName);
  if (finger.enrollResult == EnrollResult::ok) {
    notifyClients("Enrollment successfull. You can now use your new finger for scanning.");
    updateClientsFingerlist(fingerManager.getFingerListAsHtmlOptionList());
  }  else if (finger.enrollResult == EnrollResult::error) {
    notifyClients(String("Enrollment failed. (Code ") + finger.returnCode + ")");
  }
}



void reboot()
{
  notifyClients("System is rebooting now...");
  delay(1000);
    
  mqttClient.disconnect();
  espClient.stop();
  webServer.end();
  ESP.restart();
}


// React to Ethernet events:
void WiFiEvent(WiFiEvent_t event)
{
  switch (event) {

    case ARDUINO_EVENT_ETH_START:
      // This will happen during setup, when the Ethernet service starts
      Serial.println("ETH Started");
      //set eth hostname here
      ETH.setHostname(deviceHostname);
      break;

    case ARDUINO_EVENT_ETH_CONNECTED:
      // This will happen when the Ethernet cable is plugged 
      Serial.println("ETH Connected");
      break;

    case ARDUINO_EVENT_ETH_GOT_IP:
    // This will happen when we obtain an IP address through DHCP:
      Serial.print("IPv4: ");
      Serial.println(ETH.localIP());
      ethHasIp = true;
      mqttReconnectPending = true; // connect right away instead of waiting for the retry interval
      break;

    case ARDUINO_EVENT_ETH_DISCONNECTED:
      // This will happen when the Ethernet cable is unplugged 
      Serial.println("ETH Disconnected");
      ethHasIp = false;
      break;

    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("ETH Stopped");
      ethHasIp = false;
      break;

    default:
      Serial.print("ETH event:");
      Serial.println(event);
      break;
  }
}

void setup()
{
  // open serial monitor for debug infos
  Serial.begin(115200);
  while (!Serial);  // For Yun/Leo/Micro/Zero/...
  delay(100);

  // Load settings up front so nothing below runs against unloaded defaults.
  settingsManager.loadAppSettings();

  // Add a handler for network events. This is misnamed "WiFi" because the ESP32 is historically WiFi only,
  // but in our case, this will react to Ethernet events.
  Serial.println("Registering event handler for ETH events...");
  WiFi.onEvent(WiFiEvent);
  
  // Starth Ethernet (this does NOT start WiFi at the same time)
  Serial.println("Starting ETH interface...");
  ETH.begin();

  // initialize GPIOs
  pinMode(doorbellPin, INPUT_PULLUP);
  pinMode(buzzerPin, OUTPUT);
  Serial.print("Doorbell button pin: ");
  Serial.println(doorbellPin);
  Serial.print("Buzzer pin: ");
  Serial.println(buzzerPin);

  fingerManager.connect();
  
  if (!checkPairingValid())
    notifyClients("Security issue! Pairing with sensor is invalid. This could potentially be an attack! If the sensor is new or has been replaced by you do a (re)pairing in settings page. MQTT messages regarding matching fingerprints will not been sent until pairing is valid again.");

  Serial.println("Started normal operating mode");
  currentMode = Mode::scan;

  startWebserver();

  mqttClient.setBufferSize(512); // default of 256 bytes silently drops our longer log messages
  mqttClient.setCallback(mqttCallback);
  if (settingsManager.getAppSettings().mqttServer.isEmpty()) {
    mqttConfigValid = false;
    notifyClients("Error: No MQTT Broker is configured! Please go to settings and enter your server URL + user credentials.");
  }
  // The broker connection itself is established from loop() as soon as ethernet has an IP address.
  // Doing it here with a fixed delay would fail on a PoE cold start, where link negotiation and
  // DHCP regularly take longer than the ESP32 needs to boot.

  if (fingerManager.connected)
    fingerManager.setLedRingReady();
  else
    fingerManager.setLedRingError();
  
  tone(buzzerPin, 200, 500);
  tone(buzzerPin, 300, 500);
  tone(buzzerPin, 400, 500);
}

void loop()
{
  // shouldReboot flag for supporting reboot through webui
  if (shouldReboot) {
    reboot();
  }
  
  // Reconnect handling
  unsigned long currentMillis = millis();

  // reconnect mqtt if down. connectMqttClient() decides itself whether there is anything to do.
  if (ethHasIp && !mqttClient.connected() &&
      (mqttReconnectPending || (currentMillis - mqttReconnectPreviousMillis >= 30000ul))) {
    mqttReconnectPending = false;
    mqttReconnectPreviousMillis = currentMillis;
    connectMqttClient();
  }
  mqttClient.loop();

  // do the actual loop work
  switch (currentMode)
  {
  case Mode::scan:
    if (fingerManager.connected)
      doScan();
    break;
  
  case Mode::enroll:
    doEnroll();
    currentMode = Mode::scan; // switch back to scan mode after enrollment is done
    break;

  case Mode::maintenance:
    // do nothing, give webserver exclusive access to sensor (not thread-safe for concurrent calls)
    break;

  }

  // enter maintenance mode (no continous scanning) if requested
  if (needMaintenanceMode)
    currentMode = Mode::maintenance;

  // read doorbell input and publish by MQTT
  bool doorbellCurrentlyPressed;
  doorbellCurrentlyPressed = (digitalRead(doorbellPin) == LOW);

  if (doorbellCurrentlyPressed != doorbellPressed) {
    String mqttRootTopic = settingsManager.getAppSettings().mqttRootTopic;
    //Serial.print("doorbell pressed:");
    //Serial.println(doorbellCurrentlyPressed);
    if (doorbellCurrentlyPressed) {
      if (!mqttClient.publish((String(mqttRootTopic) + "/ring").c_str(), "on"))
        missedRingMillis = millis(); // broker unreachable, replay this once we are connected again
      tone(buzzerPin, 400, 500);
      tone(buzzerPin, 500, 500);
      tone(buzzerPin, 600, 500);   
    }
    else {
      noTone(buzzerPin);
      mqttClient.publish((String(mqttRootTopic) + "/ring").c_str(), "off");      
    }
  }

  doorbellPressed = doorbellCurrentlyPressed;
}