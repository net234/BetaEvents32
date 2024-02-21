/*************************************************
 *************************************************
    Sketch bnode_minimal.ino   validation of lib betaEvents32 to deal nicely with events programing with Arduino
    Copyright 2020 Pierre HENRY net23@frdev.com All - right reserved.

  This file is part of betaEvents.

    betaEvents is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    betaEvents is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with betaEvents.  If not, see <https://www.gnu.org/licenses/lglp.txt>.


  History
    V1.0 18/12/2023

   ce module se connecte au WIFI et dialogue en UDP sur le port 23234 pour recevoir et informer sur differents capteurs

 *************************************************/


#define APP_NAME "bnode_minimal V1.0b"

#include <ArduinoOTA.h>
static_assert(sizeof(time_t) == 8, "This version works with time_t 64bit  move to ESP8266 kernel 3.0 or more");


#include "EventsManager32.h"


// littleFS
#include <LittleFS.h>  //Include File System Headers
#define MyLittleFS LittleFS

//WiFI
#ifdef ESP8266
#include "ESP8266.h"
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>


//#elif defined(ESP32)
//#include "ESP32.h"
//#include <WiFi.h>
//#include <HTTPClient.h>
#else
#error "ESP8266  uniquement"
#endif

//#include <ESP8266HTTPClient.h>
#include <Arduino_JSON.h>
//WiFiClientSecure client;

// rtc memory to keep date static ram every seconds
//struct __attribute__((packed))
struct {
  // all these values are keep in RTC RAM
  uint8_t crc8;            // CRC for savedRTCmemory
  time_t actualTimestamp;  // time stamp restored on next boot Should be update in the loop() with setActualTimestamp
} savedRTCmemory;

/* Evenements du Manager (voir BetaEvents.h)
  evNill = 0,      // No event  about 1 every milisecond but do not use them for delay Use pushDelayEvent(delay,event)
  ev100Hz,         // tick 100HZ    non cumulative (see betaEvent.h)
  ev10Hz,          // tick 10HZ     non cumulative (see betaEvent.h)
  ev1Hz,           // un tick 1HZ   cumulative (see betaEvent.h)
  ev24H,           // 24H when timestamp pass over 24H
  evInChar,
  evInString,
*/

// Liste des evenements specifique a ce projet
enum tUserEventCode {
  // evenement recu
  evBP0 = 100,
  evLed0,
  evUdp,
  evPostInit,
  evStartOta,
  evStopOta,
  evTimeMasterGrab,   //Annonce le placement en MasterTime
  evTimeMasterSyncr,  //Signale au bNodes periodiquement mon status de TimeMaster
  evReset,

};


// instance EventManager
EventManager Events = EventManager();

// instance clavier
evHandlerSerial Keyboard;  //(115200, 100);

#ifndef NO_DEBUGGER
// instance debugger
evHandlerDebug Debug;  //
#endif


// instances poussoir
evHandlerButton BP0(evBP0, BP0_PIN);

// instance LED
evHandlerLed Led0(evLed0, LED_BUILTIN, false);

// Variable d'application locale
String nodeName = "";  // nom de  la device (a configurer avec NODE=)"


// init UDP
#include "evHandlerUdp.h"
const unsigned int localUdpPort = 23423;  // local port to listen on
evHandlerUdp myUdp(evUdp, localUdpPort, nodeName);




bool WiFiConnected = false;
time_t currentTime;   // timestamp en secondes : now() (local time)
int8_t timeZone = 0;  //-2;  //les heures sont toutes en localtimes (par defaut hivers france)
//int deltaTime = 0;    // delta timestamp UTC en heure
bool configErr = false;
//bool WWWOk = false;
//bool APIOk = false;
int currentMonth = -1;
bool sleepOk = true;
int multi = 0;         // nombre de clic rapide
bool configOk = true;  // global used by getConfig...
const byte postInitDelay = 15;
bool postInit = false;                  // true postInitDelay secondes apres le boot (limitation des messages Slack)
const int delayTimeMaster = 60 * 1000;  // par defaut toute les minutes   TODO:  publier cette valeur dans le groupe ?
bool isTimeMaster = false;
uint32_t bootedSecondes = 0;
time_t webClockLastTry = 0;
int16_t webClockDelta = 0;




void setup() {
  enableWiFiAtBootTime();  // mendatory for autoconnect WiFi with ESP8266 kernel 3.0

  // Start instance
  Events.begin();
  Serial.println(F("\r\n\n" APP_NAME));
  jobUpdateLed0();

  DV_println(WiFi.getMode());

  //  normaly not needed
  if (WiFi.getMode() != WIFI_STA) {
    Serial.println(F("!!! Force WiFi to STA mode !!!"));
    WiFi.mode(WIFI_STA);
    WiFi.setAutoConnect(true);
    WiFi.begin();

    //WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  // System de fichier
  if (!MyLittleFS.begin()) {
    Serial.println(F("erreur MyLittleFS"));
    //fatalError(3);
  }

  // recuperation de l'heure dans la static ram de l'ESP
  if (!getRTCMemory()) {
    savedRTCmemory.actualTimestamp = 0;
  }
  // little trick to leave timeStatus to timeNotSet
  // TODO: see with https://github.com/PaulStoffregen/Time to find a way to say timeNeedsSync

  currentTime = savedRTCmemory.actualTimestamp;
  setSyncInterval(5 * 60);                //nextSyncTime = sysTime + syncInterval;
  if (currentTime) setTime(currentTime);  //nextSyncTime = sysTime + syncInterval;



  // recuperation de la config dans config.json
  nodeName = jobGetConfigStr(F("nodename"));
  if (!nodeName.length()) {
    Serial.println(F("!!! Configurer le nom de la device avec 'NODE=nodename' !!!"));
    configErr = true;
    nodeName = "checkMyBox_";
    nodeName += WiFi.macAddress().substring(12, 14);
    nodeName += WiFi.macAddress().substring(15, 17);
  }
  DV_println(nodeName);

  // recuperation de la timezone dans la config
  timeZone = jobGetConfigInt(F("timezone"));
  if (!configOk) {
    timeZone = -2;  // par defaut France hivers
    Serial.println(F("!!! timezone !!!"));
  }
  DV_println(timeZone);
  Serial.println("Wait a for wifi");
  for (int N = 0; N < 50; N++) {
    if (WiFi.status() == WL_CONNECTED) break;
    delay(100);
  }

  ArduinoOTA.setHostname(nodeName.c_str());

  // lisen UDP 23423
  Serial.println("Listen broadcast");
  myUdp.begin();

  jobUpdateLed0();



  Serial.println("Bonjour ....");
}

void loop() {
  // test
  ArduinoOTA.handle();
  Events.get();
  Events.handle();
  switch (Events.code) {
    case evInit:
      {
        Serial.println("Init");
        Events.delayedPushMilli(delayTimeMaster + (WiFi.localIP()[3] * 100) + 500, evTimeMasterGrab);
        Events.delayedPushMilli(2000, evStartOta);


        myUdp.broadcast("{\"Info\":\"Boot\"}");
      }
      break;
    case ev1Hz:
      bootedSecondes++;
      // check for connection to local WiFi  1 fois par seconde c'est suffisant
      jobCheckWifi();

      // save current time in RTC memory
      currentTime = now();
      savedRTCmemory.actualTimestamp = currentTime;  // save time in RTC memory
      saveRTCmemory();

      // If we are not connected we warn the user every 30 seconds that we need to update credential
      if (!WiFiConnected && second() % 30 == 15) {
        // every 30 sec
        Serial.print(F("module non connecté au Wifi local "));
        DTV_println("SSID", WiFi.SSID());
        Serial.println(F("taper WIFI= pour configurer le Wifi"));
      }


      /*
        // au chagement de mois a partir 7H25 on envois le mail (un essais par heure)
        if (WiFiConnected && currentMonth != month() && hour() > 7 && minute() == 25 && second() == 0) {
        if (sendHistoTo(mailSendTo)) {
          if (currentMonth > 0) eraseHisto();
          currentMonth = month();
          writeHisto(F("Mail send ok"), mailSendTo);
        } else {
          writeHisto(F("Mail erreur"), mailSendTo);
        }
        }
      */

      break;


    case evBP0:
      switch (Events.ext) {
        case evxOn:
          jobUpdateLed0();
          Serial.println(F("BP0 On"));
          break;
        case evxOff:
          jobUpdateLed0();
          Serial.println(F("BP0 Off"));
          break;
        case evxLongOn:
          Serial.println(F("BP0 Long On"));
          break;
        case evxLongOff:
          Serial.println(F("BP0 Long Off"));
          break;
      }
      break;
      // Gestion du TimeMaster
      // si un master est preset il envoi un evgrabMaster toute les minutes
      // dans ce cas je reporte mon ev grabmaster et je perd mon status master
    case evTimeMasterSyncr:
      T_println(evTimeMasterSyncr);
      isTimeMaster = false;
      Events.delayedPushMilli(delayTimeMaster + (WiFi.localIP()[3] * 100), evTimeMasterGrab);
      //LedLife[1].setcolor((isMaster) ? rvb_blue : rvb_green, 30, 0, delayTimeMaster);
      jobUpdateLed0();  //synchro de la led de vie
      break;
    //deviens master en cas d'absence du master local
    case evTimeMasterGrab:
      T_println(evTimeMasterGrab);
      {
        String aStr = F("{\"event\":\"evMasterSyncr\"}");
        myUdp.broadcastEvent(F("evTimeMasterSyncr"));
        Events.delayedPushMilli(delayTimeMaster, evTimeMasterGrab);
        DT_println("evGrabMaster");
        //LedLife[1].setcolor((isMaster) ? rvb_blue : rvb_green, 30, 0, delayGrabMaster);
        jobUpdateLed0();
        if (!isTimeMaster) {
          isTimeMaster = true;
          //Events.push(evStartAnim1);
          //Events.push(evStartAnim3);
          //LedLife[1].setcolor(rvb_red, 30, 0, delayGrabMaster);
          // TODO placer cela dans le nodemcu de base
          // pour l'instant c'est je passe master je donne mon heure (si elle est valide ou presque :)  )

          // syncho de l'heure pour tout les bNodes
          static int8_t timeSyncrCnt = 0;
          if (timeSyncrCnt-- <= 0) {
            timeSyncrCnt = 15;
            if (timeStatus() != timeNotSet) {
              //{"TIME":{"timestamp":1707515817,"timezone":-1,"date":"2024-02-09 22:56:57"}}}
              String aStr = F("{\"TIME\":{\"timestamp\":");
              aStr += currentTime + timeZone * 3600;
              aStr += F(",\"timezone\":");
              aStr += timeZone;
              aStr += F("}}");
              myUdp.broadcast(aStr);
            }
          }
        }
      }
      break;



    case evUdp:
      if (Events.ext == evxUdpRxMessage) {
        DTV_print("from", myUdp.rxFrom);
        DTV_println("got an Event UDP", myUdp.rxJson);
        JSONVar rxJson = JSON.parse(myUdp.rxJson);

        // event
        // evTimeMasterSyncr est toujour accepté
        JSONVar rxJson2 = rxJson["Event"];
        if (JSON.typeof(rxJson2).equals("string")) {
          String aStr = rxJson2;
          DTV_println("external event", aStr);
          if (aStr.equals(F("evTimeMasterSyncr"))) {
            Events.delayedPushMilli(0, evTimeMasterSyncr);
            break;
          }

          // autre event
          ///and (from.length() > 3 and from.startsWith(pannelName))
        }

        //detection BOOT   (sent time if a member boot)
        //{"bLed256C":{"info":"Boot"}}
        rxJson2 = rxJson["Info"];
        //DV_println(JSON.typeof(rxJson2));
        if ((year(currentTime) > 2000) and isTimeMaster and JSON.typeof(rxJson2).equals("string")) {
          //DV_println((String)rxJson2);
          if (((String)rxJson2).equals("Boot")) {  //it is a memeber who boot
            //{"TIME":{"timestamp":1707515817,"timezone":-1,"date":"2024-02-09 22:56:57"}}}
            String aStr = F("{\"TIME\":{\"timestamp\":");
            aStr += currentTime + timeZone * 3600;
            aStr += F(",\"timezone\":");
            aStr += timeZone;
            aStr += F("}}");
            myUdp.broadcast(aStr);
          }
        }

        //CMD
        //./bNodeCmd.pl bNode FREE -n
        //bNodeCmd.pl  V1.4
        //broadcast:BETA82	net234	{"CMD":{"bNode":"FREE"}}
        rxJson2 = rxJson["CMD"];
        if (JSON.typeof(rxJson2).equals("object")) {
          String dest = rxJson2.keys()[0];  //<nodename called>
          // Les CMD acceptée doivent etre adressé a ce module
          if (dest.equals("ALL") or (dest.length() > 3 and nodeName.startsWith(dest))) {
            String aCmd = rxJson2[dest];
            aCmd.trim();
            DV_println(aCmd);
            if (aCmd.startsWith("NODE=") and !nodeName.equals(dest)) break;  // NODE= not allowed on aliases
            if (aCmd.length()) Keyboard.setInputString(aCmd);
          } else {
            DTV_println("CMD not for me.", dest);
          }
          break;
        }

        //TIME  TODO: a finir time
        rxJson2 = rxJson["TIME"];
        if (JSON.typeof(rxJson2).equals("object")) {
          int aTimeZone = (int)rxJson2["timezone"];
          DV_println(aTimeZone);
          if (aTimeZone != timeZone) {
            //             writeHisto( F("Old TimeZone"), String(timeZone) );
            timeZone = aTimeZone;
            jobSetConfigInt("timezone", timeZone);
          }
          time_t aTime = (unsigned long)rxJson2["timestamp"] - (timeZone * 3600);
          DV_println(niceDisplayTime(currentTime, true));
          DV_println(niceDisplayTime(aTime, true));
          int delta = aTime - currentTime;
          DV_println(delta);
          //if (abs(delta) < 5000) {
          //  adjustTime(delta);
          //  currentTime = now();
          //} else {
          //  currentTime = aTime;
          setTime(aTime);  // this will rearm the call to the
          //}
          //DV_println(currentTime);

          //DV_println(niceDisplayTime(currentTime, true));
        }
      }
      break;  //evudp

    //    case doReset:
    //      Events.reset();
    //      break;
    case evStopOta:
      Serial.println("Stop OTA");
      myUdp.broadcast("{\"info\":\"stop OTA\"}");
      ArduinoOTA.end();
      writeHisto(F("Stop OTA"), nodeName);
      break;

    case evStartOta:
      {
        // start OTA
        String deviceName = nodeName;  // "ESP_";

        ArduinoOTA.begin();
        Events.delayedPushMilli(1000L * 15 * 60, evStopOta);  // stop OTA dans 15 Min

        //MDNS.update();
        Serial.print(F("OTA on '"));
        Serial.print(deviceName);
        Serial.print(F("' started SSID:"));
        Serial.println(WiFi.SSID());
        myUdp.broadcastInfo("start OTA");
        //end start OTA
      }
      break;

    case evReset:
      T_println("Reset");
      delay(1000);
      Events.reset();
      break;




    case evInString:
      //D_println(Keyboard.inputString);
      if (Keyboard.inputString.startsWith(F("?"))) {
        Serial.println(F("Liste des commandes"));
        Serial.println(F("NODE=nodename (nom du module)"));
        Serial.println(F("WIFI=ssid,paswword"));
        //        Serial.println(F("MAILTO=adresse@mail    (mail du destinataire)"));
        //        Serial.println(F("MAILFROM=adresse@mail  (mail emetteur 'NODE' sera remplacé par nodename)"));
        //        Serial.println(F("SMTPSERV=mail.mon.fai,login,password  (SMTP serveur et credential) "));
        //        Serial.println(F("SONDENAMES=name1,name2...."));
        //        Serial.println(F("SWITCHENAMES=name1,name2...."));
        //        Serial.println(F("RAZCONF      (efface la config sauf le WiFi)"));
        //        Serial.println(F("MAIL         (envois un mail de test)"));
        //        Serial.println(F("API          (envois une commande API timezone)"));
        //        Serial.println(F("BCAST        (envoi un broadcast)"));
      }

      if (Keyboard.inputString.startsWith(F("NODE="))) {
        Serial.println(F("SETUP NODENAME : 'NODE= nodename'  ( this will reset)"));
        String aStr = Keyboard.inputString;
        grabFromStringUntil(aStr, '=');
        aStr.replace(" ", "_");
        aStr.trim();

        if (aStr != "") {
          nodeName = aStr;
          DV_println(nodeName);
          jobSetConfigStr(F("nodename"), nodeName);
          delay(1000);
          Events.reset();
        }
      }


      if (Keyboard.inputString.startsWith(F("WIFI="))) {
        Serial.println(F("SETUP WIFI : 'WIFI= WifiName, password"));
        String aStr = Keyboard.inputString;
        grabFromStringUntil(aStr, '=');
        String ssid = grabFromStringUntil(aStr, ',');
        ssid.trim();
        DV_println(ssid);
        if (ssid != "") {
          String pass = aStr;
          pass.trim();
          DV_println(pass);
          bool result = WiFi.begin(ssid, pass);
          //WiFi.setAutoConnect(true);
          DV_println(WiFi.getAutoConnect());
          Serial.print(F("WiFi begin "));
          DV_println(result);
        }
      }
      //      if (Keyboard.inputString.equals(F("RAZCONF"))) {
      //        Serial.println(F("RAZCONF this will reset"));
      //        eraseConfig();
      //        delay(1000);
      //        Events.reset();
      //      }

      if (Keyboard.inputString.equals(F("RESET"))) {
        DT_println("CMD RESET");
        Events.push(evReset);
      }
      if (Keyboard.inputString.equals(F("FREE"))) {
        String aStr = F("Ram=");
        aStr += String(Events.freeRam());
        aStr += F(",APP=" APP_NAME);
        Serial.println(aStr);
        myUdp.broadcastInfo(aStr);
      }
      //num {timeNotSet, timeNeedsSync, timeSet
      if (Keyboard.inputString.equals(F("TIME?"))) {
        String aStr = F("mydate=");
        aStr += niceDisplayTime(currentTime, true);
        aStr += F(" timeZone=");
        aStr += timeZone;
        aStr += F(" timeStatus=");
        aStr += timeStatus();
        aStr += F(" webClockDelta=");
        aStr += webClockDelta;
        aStr += F("ms  webClockLastTry=");
        aStr += niceDisplayTime(webClockLastTry, true);


        Serial.println(aStr);
        myUdp.broadcastInfo(aStr);
      }



      if (Keyboard.inputString.equals(F("INFO"))) {
        String aStr = F("node=");
        aStr += nodeName;
        aStr += F(" isTimeMaster=");
        aStr += String(isTimeMaster);
        aStr += F(" CPU=");
        aStr += String(Events._percentCPU);
        aStr += F("% ack=");
        aStr += String(myUdp.ackPercent);
        aStr += F("%  booted=");
        aStr += niceDisplayDelay(bootedSecondes);


        //ledLifeVisible = true;
        //Events.delayedPushMilli(5 * 60 * 1000, evHideLedLife);
        myUdp.broadcastInfo(aStr);
        DV_println(aStr)
      }
      if (Keyboard.inputString.equals("OTA")) {
        Events.push(evStartOta);
        DT_println("Start OTA");
      }


      break;
  }
}
