                           /************************************************************************************
 * Watson box 
 * Purpose:  a pair of boxes each with 4 LEDs and 4 buttons.  Pushing a button toggles
 *    the state of the corresponding LED on both boxes.  The purpose is to signal to
 *    the person with the other box that attention is needed (in case they are out of
 *    hearing range or have their headphones on).
 *    
 * // state machine
 * //  off   --> push     --> on (steady)  '1'
 * //        --> double   --> slow blink   'S'
 * //        --> triple   --> fast blink   'F'
 * //        --> long     --> breathe      'B'
 * //  ! off --> any push --> off          '0'
 * //    (other possible 'X' super fast blink, but not settable by button)
 *
 * State is kept via MQTT communication and retained messages
 *     rhatcher/watson/state                  RW  'XXXX"
 *     rhatcher/watson/Watson-MAC4            -W  "client connected" or "client disconnected"
 *     rhatcher/watson/Watson-MAC4/brightness R-  <b1> <b2> <b3> <b4>   # values between 2 - 255
 *
 */

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>

#include <jled.h>
#include <DebounceEvent.h>

// version 2.5.3 of esp8266 doesn't define DX pins
#define D0 16
#define D1  5
#define D2  4
#define D3  0
#define D5 14
#define D6 12
#define D7 13
#define D8 15

// hide these
#include "secrets.h"

WiFiClient*   espWifiClient = 0;
PubSubClient* mqttClient    = 0;
byte macAddr[6] = { 0 };
String clientID = "x";

const unsigned int NLED = 4;
int   buttonpins[NLED]       = {  D1,  D2,  D3,  D4 };
int   ledpins[NLED]          = {  D0,  D5,  D6,  D7 };
JLed* leds[NLED]             = {   0,   0,   0,   0 };
DebounceEvent* buttons[NLED] = {   0,   0,   0,   0 };
const int brightMin          =   2;
const int brightMax          = 255;
int   brightness[NLED]       = { 255, 255, 255, 255 };
// need a trailing null char to print this as a string
char  states[NLED+1]           = { 'X', 'X', 'X', 'X', '\0' };

String willTopic    = "x";
String brightTopic  = "x";
String batteryTopic = "x";
String resetTopic   = "x";

// allowed states  X=unknown, 0=off, 1=on, F=fast blink, S=slow b|ink, B=breathe

#define TimeLapse(t1)  (long)((unsigned long)millis()-(unsigned long)t1)
unsigned long startTime = millis();

 // 5min * 60 sec/min * 1000 milli/sec
 #define FIVEMINUTES ( 5 * 60 * 1000 ) 
 
unsigned long t_heartbeat = millis(); // presume we've had an initial heartbeat
unsigned long t_batcheck  = millis() - FIVEMINUTES; // time to check the battery on A0

const char* eventtextp[] = { "EVENT_NONE", "EVENT_CHANGED", "EVENT_PRESSED ", "EVENT_RELEASED" };

void setLedState(int indx, char state) {
  JLed& thisled  = (*leds[indx]);
  char& ledstate = states[indx];
  ledstate = state;

  auto bvalue = brightness[indx];
  switch (ledstate) {
    case '0': 
      thisled.Off().Update();
      break;
    case '1':
      thisled.Set(bvalue).Update();
      break;
    case 'S':
      thisled.Blink(500,500).Forever().DelayAfter(0).MaxBrightness(bvalue).Update();
      break;
    case 'F':
      thisled.Blink(100,100).Forever().DelayAfter(0).MaxBrightness(bvalue).Update();
      break;
    case 'B':
      thisled.Breathe(1500).Forever().DelayAfter(50).MaxBrightness(bvalue).Update();
      break;
    default:
      thisled.Blink(50,50).Forever().DelayAfter(0).MaxBrightness(bvalue).Update();
      ledstate = 'X';
      break;
  }
}

void setBrightnessFromPayload(byte* payload,int len) {
  // be wary of whitespace padding when we tokenize
  // incoming payload may not be null \0 terminated
  String input;
  for (int j=0; j<len; ++j) input += (char)payload[j];
  // trim leading whitespace
  //
  //while (input[0] == ' ' || input[0] == '\t' || input[0] == '\n') {
  //  input.erase(0,1);
  //}
  input.trim();
  input.replace("\t"," ");
  input.replace("\n"," ");
  // reduce all the spaces to singles
  while (input.indexOf("  ") != -1) input.replace("  "," ");
  
  Serial.print("setBrightness \""); Serial.print(input.c_str()); Serial.println("\"    ");

  int indx = 0;  // led index
  int k = 0; // position in "input"
  bool lastOne = false;
  while ( k < len && ! lastOne ) {
    int kWhite = input.indexOf(" ",k);
    if (kWhite < 0 ) lastOne = true;
    Serial.print(" k "); Serial.print(k); Serial.print(" len "); Serial.print(len);
    Serial.print(" kWhite "); Serial.println(kWhite);
    String s = input.substring(k,kWhite);
    int value = s.toInt();
    if (value < brightMin) value = brightMin;
    if (value > brightMax) value = brightMax;
 
    Serial.print(" set led "); Serial.print(indx); Serial.print(" to "); Serial.println(value);
    brightness[indx] = value;
    ++indx;
    if (indx >= NLED ) break; // don't set what we ain't got
    if (kWhite > 0) k = kWhite+1;
  }
  // refresh state just to get updates
  for (int i=0; i<NLED; ++i) {
    // same state, but trigger brightness change
    setLedState(i,states[i]);
  }

}

// mqttCallback the MQTT callback for received messages
void mqttCallback(char* topic, byte* payload, unsigned len) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  // assume payloads are chars for this application, but don't count on trailing \0 null
  for (int i=0; i<len; ++i) Serial.print((char)payload[i]);
  Serial.println();

  if ( strcmp(topic,"rhatcher/heartbeat") == 0 ) {
    t_heartbeat = millis();
    // also remind the world we're alive
    mqttClient->publish(willTopic.c_str(),"currently connected"); 
  }

  if ( strcmp(topic,"rhatcher/watson/state") == 0 ) {
    for (int i=0; i<min(len,NLED); ++i) {
      // incoming new state
      setLedState(i,(char)payload[i]);
    }
  }

  if ( strcmp(topic,brightTopic.c_str()) == 0 ) {
    setBrightnessFromPayload(payload,len);
  }
  
}

void mqttReconnect() { // loop until we're connected
  while ( ! mqttClient->connected() ) {
    delay(200);
    Serial.print("Attempting MQTT connection ...");
    // clientID, username, password, willTopic, willQoS, Retain, willMessage);
    willTopic = "rhatcher/watson/" + clientID;
    char* willMessage = "client disconnected";
    brightTopic  = willTopic + "/brightness";
    batteryTopic = willTopic + "/battery";
    resetTopic   = willTopic + "/reset";
    if ( mqttClient->connect(clientID.c_str(), mqtt_user, mqtt_pass, willTopic.c_str(), 2, true, willMessage) ) {
      // connected let the world know
      Serial.println("connected to MQTT");
      delay(500);  // publishing the override to the will too soon seems to get lost sometimes
      mqttClient->publish(willTopic.c_str(),"currently connected");
      mqttClient->subscribe("rhatcher/watson/state");
      mqttClient->subscribe("rhatcher/heartbeat");
      mqttClient->subscribe(brightTopic.c_str());
    } else {
      Serial.print("failed, rc="); Serial.print(mqttClient->state());
      Serial.println(" try again in 2 seconds");
      delay(2000);
    }
  }
}



int buttonGPIO2index(uint8_t pin) {
  for (int i=0; i<NLED; ++i) {
    if ( buttonpins[i] == pin ) return i;
  }
  return -1;
}

char* GPIO2DPin(uint8_t pin) {
  switch (pin) {
    case D0: return "D0";  break;
    case D1: return "D1";  break;
    case D2: return "D2";  break;
    case D3: return "D3";  break;
    case D4: return "D4";  break;
    case D5: return "D5";  break;
    case D6: return "D6";  break;
    case D7: return "D7";  break;
    case D8: return "D8";  break;
    default:  return "??";
  }
}

void buttonCallback(uint8_t pin, uint8_t event, uint8_t count, uint16_t len) {

    if ( event != EVENT_RELEASED ) {
    // not interested so much in these
    Serial.println();
    return;
  }
  
  Serial.print("Pin: ");     Serial.print(pin);
  Serial.print(" ");         Serial.print(GPIO2DPin(pin)); 
  int indx = buttonGPIO2index(pin);
  if (indx < 0 || indx > NLED ) {
    Serial.print(" bad index: ");  Serial.print(indx);
    Serial.println();
    return;
  }
  char& ledstate = states[indx];
  Serial.print(" Indx: ");   Serial.print(indx);  Serial.print(" '"); Serial.print(ledstate); Serial.print("'");
  Serial.print(" Event: ");  Serial.print(event); Serial.print(" "); Serial.print(eventtextp[event]);
  Serial.print(" Count: ");  Serial.print(count);
  Serial.print(" Length: "); Serial.print(len);

  // state machine
  //  off   --> push   --> on (steady)
  //        --> double --> slow blink
  //        --> triple --> fast blink
  //        --> long   --> breathe

  //  ! off --> push --> off

  // for sciplus buttons normal length ~ 175-200,  long push > 500
  // for         rocker                  200-350             > 500     

//  JLed& thisled  = (*leds[indx]);

  if (ledstate == '0') {
    if (len > 500 ) {
      setLedState(indx,'B');
    } else {
      switch (count) {
        case 1: setLedState(indx,'1'); break;
        case 2: setLedState(indx,'S'); break;
        case 3: setLedState(indx,'F'); break;
        default:
          setLedState(indx,'X'); break;
       }
    }
  } else {
    setLedState(indx,'0');
  }

  Serial.print(" ["); Serial.print(states); Serial.print("]");
  Serial.println();

  if (mqttClient ) {
    Serial.println("publish states");
    // publish a _retained_ message
    // this allow a box coming online to pick up the current/last state
    // if using char* the method takes 3 args (no "length") despite unclear documentation
    mqttClient->publish("rhatcher/watson/state",states,true);
  }

}

char convertToHexDigit(byte b) {
  if ( b  < 10 ) return b + '0';
  return b - 10 + 'A';
}

String convertMACLower4toStr() {
  String s;
  
  Serial.print("MAC: ");
  Serial.print(macAddr[0],HEX);
  Serial.print(":");
  Serial.print(macAddr[1],HEX);
  Serial.print(":");
  Serial.print(macAddr[2],HEX);
  Serial.print(":");
  Serial.print(macAddr[3],HEX);
  Serial.print(":");
  Serial.print(macAddr[4],HEX);
  Serial.print(":");
  Serial.println(macAddr[5],HEX);
  
  Serial.print(" macAddr[4] "); Serial.print(macAddr[4]);
  Serial.print(" macAddr[5] "); Serial.print(macAddr[5]);
  Serial.println();
  
  byte nibbles[4] = { 0, 0, 0, 0 };
  nibbles[0] = ( macAddr[4] >> 4 ) & 0xF;
  nibbles[1] = ( macAddr[4]      ) & 0xF ;
  nibbles[2] = ( macAddr[5] >> 4 ) & 0xF ;
  nibbles[3] = ( macAddr[5]      ) & 0xF ;
  
  for (int j=0; j<4; ++j ) {
    Serial.print("nibble "); Serial.print(j); Serial.print(" value "); Serial.println(nibbles[j]);
    char c = convertToHexDigit(nibbles[j]);
    s += c;
  }
  
//  for (int i=0; i<6; ++i) {
//    char c = (char)macAddr[i]; // already HEX?  convertToHexDigit(macAddr[i]);
//    s += c;
//  }
  Serial.print("convert MACtoStr ");
  Serial.print(s);
  Serial.println();
  return s;
}

void setup() {
  
  Serial.begin(115200);
  Serial.println(" "); // move beyond upload noise
  Serial.print("setup: ");

  // initialize LEDs and turn them off as the objects are created, record this state
  for (int i=0; i<NLED; ++i) {
    Serial.println();
    Serial.print("i="); Serial.print(i);
    auto ledpin = ledpins[i];
    pinMode(ledpin,OUTPUT);
    Serial.print(" led "); Serial.print(GPIO2DPin(ledpin));
    JLed* ledptr = new JLed(ledpin);
    leds[i] = ledptr;
    states[i] = '0';
    (*ledptr).Off();
    auto bpin = buttonpins[i];
    Serial.print(" button "); Serial.print(GPIO2DPin(bpin));
    // there something special with D8 ... won't flash program with it pulled up, so avoid it
    DebounceEvent* buttonptr = new DebounceEvent(bpin,buttonCallback, BUTTON_PUSHBUTTON | BUTTON_DEFAULT_HIGH );
    buttons[i] = buttonptr;
    Serial.print(" i="); Serial.print(i); 
    Serial.print(" ["); Serial.print(ledpin); Serial.print(","); Serial.print(bpin); Serial.print("] ");
  }  

  int wifiStatus = WL_IDLE_STATUS;
  WiFi.mode(WIFI_STA);
  wifiStatus = WiFi.begin(ssid,password);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed!  Rebooting...");
    delay(5000);
    ///ESP.restart();
    return;
  } 

   WiFi.macAddress(macAddr);
   // hostname defaults to sep8266-[ChipID]

   // create an unique clientID for MQTT
   clientID = "Watson-";
   clientID += convertMACLower4toStr();
   Serial.print("clientID is ");
   Serial.println(clientID);

  // put ArduinoOTA stuff here
  ArduinoOTA.setHostname(clientID.c_str());
  // No authentication by default
  // ArduinoOTA.setPassword((const char*)"1234");
  ArduinoOTA.onStart([]() {
    Serial.println("ArduinoOTA.onStart");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nArduinoOTA.onEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("ArduinoOTA.onProgress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("ArduinoOTA.onError[%u]: ", error);
    if      ( error == OTA_AUTH_ERROR    ) Serial.println("Auth Failed");
    else if ( error == OTA_BEGIN_ERROR   ) Serial.println("Begin Failed");
    else if ( error == OTA_CONNECT_ERROR ) Serial.println("Connect Failed");
    else if ( error == OTA_RECEIVE_ERROR ) Serial.println("Receive Failed");
    else if ( error == OTA_END_ERROR     ) Serial.println("End Failed");
    else { Serial.println("Unknown error"); }
  });
  ArduinoOTA.begin();

  Serial.print("local IP Address: "); Serial.println(WiFi.localIP());

  // MQTT setup
  espWifiClient = new WiFiClient();
  mqttClient    = new PubSubClient(*espWifiClient);
  mqttClient->setServer(mqtt_server, 1883);
  mqttClient->setCallback(mqttCallback);
  
  Serial.println(" ... end setup");
}

void loop() {

  ArduinoOTA.handle();

  if (mqttClient) {
    if (! mqttClient->connected() ) mqttReconnect();
    mqttClient->loop();  // look for messages we subscribed to
  }

  // update buttons, leds
  for (int i=0; i<NLED; ++i) {
    buttons[i]->loop();
    leds[i]->Update();
  }

  if (TimeLapse(t_batcheck) > FIVEMINUTES ) {
    t_batcheck = millis();
    int val = analogRead(A0);
    float f = 3.3 * (float)val/1024.0;
    String msg = "battery at ";
    msg += f;
    msg += " V  (";
    msg += val;
    msg += ")";
    mqttClient->publish(batteryTopic.c_str(),msg.c_str());
  }

  // have we heard a heartbeat lately 
  if (TimeLapse(t_heartbeat) > FIVEMINUTES ) {
    mqttClient->publish(resetTopic.c_str(),"hard reset from lack of heartbeats");   
    Serial.println("force reset due to lack of heartbeats");
    ESP.reset();
  }
  
}
