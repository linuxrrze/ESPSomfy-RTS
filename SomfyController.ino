#include <WiFi.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>
#include "Log.h"
#include "ConfigSettings.h"
#include "Network.h"
#include "Web.h"
#include "Sockets.h"
#include "Utils.h"
#include "Somfy.h"
#include "MQTT.h"
#include "GitOTA.h"

ConfigSettings settings;
Web webServer;
SocketEmitter sockEmit;
Network net;
rebootDelay_t rebootDelay;
SomfyShadeController somfy;
MQTTClass mqtt;
GitUpdater git;

uint32_t oldheap = 0;
void setup() {
  Serial.begin(115200);
  LOG_PRINTLN();
  LOG_PRINTLN("Startup/Boot....");
  LOG_PRINTLN("Mounting File System...");
  if(LittleFS.begin()) LOG_PRINTLN("File system mounted successfully");
  else LOG_PRINTLN("Error mounting file system");
  settings.begin();
  if(WiFi.status() == WL_CONNECTED) WiFi.disconnect(true);
  delay(10);
  DEBUG_PRINTLN();
  webServer.startup();
  webServer.begin();
  delay(1000);
  net.setup();  
  somfy.begin();
  //git.checkForUpdate();
  esp_task_wdt_init(7, true); //enable panic so ESP32 restarts
  esp_task_wdt_add(NULL); //add current thread to WDT watch

}

void loop() {
  // put your main code here, to run repeatedly:
  //uint32_t heap = ESP.getFreeHeap();
  if(rebootDelay.reboot && timeReached(rebootDelay.rebootTime)) {
    DEBUG_PRINT("Rebooting after ");
    DEBUG_PRINT(rebootDelay.rebootTime);
    DEBUG_PRINTLN("ms");
    net.end();
    ESP.restart();
    return;
  }
  uint32_t timing = millis();
  
  net.loop();
  if(millis() - timing > 100) DEBUG_PRINTF("Timing Net: %ldms\n", millis() - timing);
  timing = millis();
  esp_task_wdt_reset();
  somfy.loop();
  if(millis() - timing > 100) DEBUG_PRINTF("Timing Somfy: %ldms\n", millis() - timing);
  timing = millis();
  esp_task_wdt_reset();
  if(net.connected() || net.softAPOpened) {
    if(!rebootDelay.reboot && net.connected() && !net.softAPOpened) {
      git.loop();
      esp_task_wdt_reset();
    }
    webServer.loop();
    esp_task_wdt_reset();
    if(millis() - timing > 100) DEBUG_PRINTF("Timing WebServer: %ldms\n", millis() - timing);
    esp_task_wdt_reset();
    timing = millis();
    sockEmit.loop();
    if(millis() - timing > 100) DEBUG_PRINTF("Timing Socket: %ldms\n", millis() - timing);
    esp_task_wdt_reset();
    timing = millis();
  }
  if(rebootDelay.reboot && timeReached(rebootDelay.rebootTime)) {
    net.end();
    ESP.restart();
  }
  esp_task_wdt_reset();
}
