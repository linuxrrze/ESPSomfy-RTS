#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "ArduinoJsonConfig.h"
#include <esp_task_wdt.h>
#include "Log.h"
#include "ConfigSettings.h"
#include "MQTT.h"
#include "Somfy.h"
#include "Network.h"
#include "Utils.h"
#include "CABundle.h"

#define MQTT_RETRY_MIN 10000
#define MQTT_RETRY_MAX 120000

WiFiClient tcpClient;
// Created for every connection attempt so no verification mode from an earlier attempt sticks.
static WiFiClientSecure *tlsClient = nullptr;
// setCACert() keeps the pointer, so the CA certificate stays allocated with the client.
static char *g_caCert = nullptr;
PubSubClient mqttClient(tcpClient);

#define MQTT_MAX_RESPONSE 2048
static char g_content[MQTT_MAX_RESPONSE];

extern ConfigSettings settings;
extern SomfyShadeController somfy;
extern Network net;
extern rebootDelay_t rebootDelay;


bool MQTTClass::begin() {
  this->suspended = false;
  return true;
}
void MQTTClass::resetRetry() {
  this->retryDelay = MQTT_RETRY_MIN;
  this->lastConnect = millis() - MQTT_RETRY_MIN;
}
bool MQTTClass::pauseTLS() {
  if(!settings.MQTT.enabled || !settings.MQTT.useTLS() || this->suspended || !mqttClient.connected()) return false;
  // A clean disconnect does not send the last will.
  this->publish("status", "offline", true);
  this->end();
  return true;
}
void MQTTClass::resume() {
  this->begin();
  this->resetRetry();
}
// Selects plain TCP or TLS for the next connection. For TLS the connection is opened here so the
// certificate is checked before PubSubClient sends the credentials.
static bool openTransport() {
  if(!settings.MQTT.useTLS()) {
    mqttClient.setClient(tcpClient);
    return true;
  }
  // Detach the previous TLS client before freeing it.
  mqttClient.setClient(tcpClient);
  delete tlsClient;
  free(g_caCert);
  g_caCert = nullptr;
  tlsClient = new WiFiClientSecure();
  // Limit how long a connection attempt blocks the main loop.
  tlsClient->setTimeout(3);
  tlsClient->setHandshakeTimeout(4);
  switch(settings.MQTT.tlsVerify) {
    case mqtt_tls_verify::PublicCA:
      tlsClient->setCACertBundle(ca_bundle);
      break;
    case mqtt_tls_verify::CustomCA:
      g_caCert = (char *)malloc(MQTT_MAX_CA_CERT + 1);
      if(!g_caCert || !settings.MQTT.loadCACert(g_caCert, MQTT_MAX_CA_CERT + 1)) {
        LOG_PRINTLN("MQTT: no CA certificate stored for the broker");
        return false;
      }
      tlsClient->setCACert(g_caCert);
      break;
    case mqtt_tls_verify::None:
    case mqtt_tls_verify::Fingerprint:
      // The fingerprint is checked after the handshake.
      tlsClient->setInsecure();
      break;
    default:
      LOG_PRINTLN("MQTT: invalid certificate verification mode");
      return false;
  }
  if(!tlsClient->connect(settings.MQTT.hostname, settings.MQTT.port)) {
    char msg[96];
    tlsClient->lastError(msg, sizeof(msg));
    LOG_PRINTF("MQTT: TLS connection to %s:%u failed: %s\n", settings.MQTT.hostname, settings.MQTT.port, msg);
    return false;
  }
  if(settings.MQTT.tlsVerify == mqtt_tls_verify::Fingerprint && !tlsClient->verify(settings.MQTT.fingerprint, nullptr)) {
    LOG_PRINTLN("MQTT: the broker certificate does not match the fingerprint");
    tlsClient->stop();
    return false;
  }
  mqttClient.setClient(*tlsClient);
  return true;
}
bool MQTTClass::end() {
  this->suspended = true;
  this->disconnect();
  return true;
}
void MQTTClass::reset() {
  this->disconnect();
  this->resetRetry();
  this->connect();
}
bool MQTTClass::loop() {
  if(settings.MQTT.enabled && !rebootDelay.reboot && !this->suspended && !mqttClient.connected()) {
    esp_task_wdt_reset();
    if(!this->connected() && net.connected()) this->connect();
  }
  esp_task_wdt_reset();
  if(settings.MQTT.enabled) mqttClient.loop();
  return true;
}
void MQTTClass::receive(const char *topic, byte*payload, uint32_t length) {
  esp_task_wdt_reset(); // Make sure we do not reboot here.
  DEBUG_PRINT("MQTT Topic:");
  DEBUG_PRINT(topic);
  DEBUG_PRINT(" payload:");
  for(uint32_t i=0; i<length; i++)
    DEBUG_PRINT((char)payload[i]);
  DEBUG_PRINTLN();

  // We need to start at the last slash in the data
  uint8_t len = strlen(topic);
  
  uint8_t slashes = 0;
  uint16_t ndx = strlen(topic) - 1;
  while(ndx > 0) {
    if(topic[ndx] == '/') slashes++;
    if(slashes == 4) break;
    ndx--;
  }
  char entityId[4];
  char command[32];
  char entityType[7];
  char value[10];
  memset(command, 0x00, sizeof(command));
  memset(entityId, 0x00, sizeof(entityId));
  memset(entityType, 0x00, sizeof(entityType));
  memset(value, 0x00, sizeof(value));
  uint8_t i = 0;
  while(topic[ndx] == '/' && ndx < len) ndx++;
  while(ndx < len) {
    if(topic[ndx] != '/' && i < sizeof(entityType))
      entityType[i++] = topic[ndx];
    ndx++;
    if(topic[ndx] == '/') break;
  }
  i = 0;
  while(topic[ndx] == '/' && ndx < len) ndx++;
  while(ndx < len) {
    if(topic[ndx] != '/' && i < sizeof(entityId))
      entityId[i++] = topic[ndx];
    ndx++;
    if(topic[ndx] == '/') break;
  }
  i = 0;
  while(topic[ndx] == '/' && ndx < len) ndx++;
  while(ndx < len) {
    if(topic[ndx] != '/' && i < sizeof(command))
      command[i++] = topic[ndx];
    ndx++;
    if(topic[ndx] == '/') break;
  }
  for(uint8_t j = 0; j < length && j < sizeof(value); j++)
    value[j] = payload[j];
  
  DEBUG_PRINT("MQTT type:[");
  DEBUG_PRINT(entityType);
  DEBUG_PRINT("] command:[");
  DEBUG_PRINT(command);
  DEBUG_PRINT("] entityId:");
  DEBUG_PRINT(entityId);
  DEBUG_PRINT(" value:");
  DEBUG_PRINTLN(value);
  if(strncmp(entityType, "shades", sizeof(entityType)) == 0) {
    SomfyShade* shade = somfy.getShadeById(atoi(entityId));
    if (shade) {
      int val = atoi(value);
      if(strncmp(command, "target", sizeof(command)) == 0) {
        if(val >= 0 && val <= 100)
          shade->moveToTarget(shade->transformPosition(atoi(value)));
      }
      if(strncmp(command, "tiltTarget", sizeof(command)) == 0) {
        if(val >= 0 && val <= 100)
          shade->moveToTiltTarget(atoi(value));
      }
      else if(strncmp(command, "direction", sizeof(command)) == 0) {
        if(val < 0)
          shade->sendCommand(somfy_commands::Up);
        else if(val > 0)
          shade->sendCommand(somfy_commands::Down);
        else
          shade->sendCommand(somfy_commands::My);
      }
      else if(strncmp(command, "mypos", sizeof(command)) == 0) {
        if(val >= 0 && val <= 100)
          shade->setMyPosition(val);
      }
      else if(strncmp(command, "myTiltPos", sizeof(command)) == 0) {
        if(val >= 0 && val <= 100)
          shade->setMyPosition(shade->myPos, val);
      }
      else if(strncmp(command, "sunFlag", sizeof(command)) == 0) {
        if(val > 0) shade->sendCommand(somfy_commands::SunFlag);
        else shade->sendCommand(somfy_commands::Flag);
      }
      else if(strncmp(command, "position", sizeof(command)) == 0) {
        if(val >= 0 && val <= 100) {
          shade->target = shade->currentPos = shade->transformPosition((float)val);
          shade->emitState();
        }
      }
      else if(strncmp(command, "tiltPosition", sizeof(command)) == 0) {
        if(val >= 0 && val <= 100) {
          shade->tiltTarget = shade->currentTiltPos = (float)val;
          shade->emitState();
        }
      }
      else if(strncmp(command, "sunny", sizeof(command)) == 0) {
        if(val >= 0) shade->sendSensorCommand(-1, val, shade->repeats);
      }
      else if(strncmp(command, "windy", sizeof(command)) == 0) {
        if(val >= 0) shade->sendSensorCommand(val, -1, shade->repeats);
      }
    }
  }
  else if(strncmp(entityType, "groups", sizeof(entityType)) == 0) {
    SomfyGroup* group = somfy.getGroupById(atoi(entityId));
    if (group) {
      int val = atoi(value);
      if(strncmp(command, "direction", sizeof(command)) == 0) {
        if(val < 0)
          group->sendCommand(somfy_commands::Up);
        else if(val > 0)
          group->sendCommand(somfy_commands::Down);
        else
          group->sendCommand(somfy_commands::My);
      }
      else if(strncmp(command, "sunFlag", sizeof(command)) == 0) {
        if(val > 0)
          group->sendCommand(somfy_commands::Flag);
        else
          group->sendCommand(somfy_commands::SunFlag);
      }
      else if(strncmp(command, "sunny", sizeof(command)) == 0) {
        if(val >= 0) group->sendSensorCommand(-1, val, group->repeats);
      }
      else if(strncmp(command, "windy", sizeof(command)) == 0) {
        if(val >= 0) group->sendSensorCommand(val, -1, group->repeats);
      }
    }
  }
  esp_task_wdt_reset(); // Make sure we do not reboot here.
}
bool MQTTClass::connect() {
  esp_task_wdt_reset(); // Make sure we do not reboot here.
  if(mqttClient.connected()) {
    if(!settings.MQTT.enabled || this->suspended)
      return this->disconnect();
    else
      return true;
  }
  if(settings.MQTT.enabled && !this->suspended) {
    if(millis() - this->lastConnect < this->retryDelay) return false;
    uint64_t mac = ESP.getEfuseMac();
    snprintf(this->clientId, sizeof(this->clientId), "client-%08x%08x", (uint32_t)((mac >> 32) & 0xFFFFFFFF), (uint32_t)(mac & 0xFFFFFFFF));
    if(strlen(settings.MQTT.protocol) > 0 && strlen(settings.MQTT.hostname) > 0) {
      mqttClient.setServer(settings.MQTT.hostname, settings.MQTT.port);
      mqttClient.setSocketTimeout(4);
      char lwtTopic[128] = "status";
      if(strlen(settings.MQTT.rootTopic) > 0)
        snprintf(lwtTopic, sizeof(lwtTopic), "%s/status", settings.MQTT.rootTopic);
      // Resolving the host, the TLS handshake and waiting for the broker are each limited by a
      // timeout, but together they can exceed the task watchdog.
      esp_task_wdt_delete(NULL);
      bool ok = openTransport() && mqttClient.connect(this->clientId, settings.MQTT.username, settings.MQTT.password, lwtTopic, 0, true, "offline");
      esp_task_wdt_add(NULL);
      esp_task_wdt_reset();
      if(ok) {
        DEBUG_PRINT("Successfully connected MQTT client ");
        DEBUG_PRINTLN(this->clientId);
        this->publish("status", "online", true);
        this->publish("ipAddress", settings.IP.ip.toString().c_str(), true);
        this->publish("host", settings.hostname, true);
        this->publish("firmware", settings.fwVersion.name, true);
        this->publish("serverId", settings.serverId, true);
        this->publish("mac", net.mac.c_str());
        somfy.publish();
        this->subscribe("shades/+/target/set");
        this->subscribe("shades/+/tiltTarget/set");
        this->subscribe("shades/+/direction/set");
        this->subscribe("shades/+/mypos/set");
        this->subscribe("shades/+/myTiltPos/set");
        this->subscribe("shades/+/sunFlag/set");
        this->subscribe("shades/+/sunny/set");
        this->subscribe("shades/+/windy/set");
        this->subscribe("shades/+/position/set");
        this->subscribe("shades/+/tiltPosition/set");
        this->subscribe("groups/+/direction/set");
        this->subscribe("groups/+/sunFlag/set");
        this->subscribe("groups/+/sunny/set");
        this->subscribe("groups/+/windy/set");
        mqttClient.setCallback(MQTTClass::receive);
        DEBUG_PRINTLN("MQTT Startup Completed");
        esp_task_wdt_reset();
        this->lastConnect = millis();
        this->retryDelay = MQTT_RETRY_MIN;
        return true;
      }
      else {
        LOG_PRINT("MQTT Connection failed for: ");
        LOG_PRINTLN(mqttClient.state());
        this->lastConnect = millis();
        // Every attempt blocks the main loop for up to several seconds, so back off.
        this->retryDelay = min((uint32_t)MQTT_RETRY_MAX, this->retryDelay * 2);
        return false;
      }
    }
    else
      return true;
  }
  return true;
}
bool MQTTClass::disconnect() {
  if(mqttClient.connected()) {
    this->unsubscribe("shades/+/target/set");
    this->unsubscribe("shades/+/direction/set");
    this->unsubscribe("shades/+/tiltTarget/set");
    this->unsubscribe("shades/+/mypos/set");
    this->unsubscribe("shades/+/myTiltPos/set");
    this->unsubscribe("shades/+/sunFlag/set");
    this->unsubscribe("groups/+/direction/set");
    this->unsubscribe("shades/+/sunny/set");
    this->unsubscribe("shades/+/windy/set");
    this->unsubscribe("shades/+/position/set");
    this->unsubscribe("shades/+/tiltPosition/set");
    this->unsubscribe("groups/+/direction/set");
    this->unsubscribe("groups/+/sunFlag/set");
    this->unsubscribe("groups/+/sunny/set");
    this->unsubscribe("groups/+/windy/set");
    mqttClient.disconnect();
  }
  return true;
}
bool MQTTClass::unsubscribe(const char *topic) {
  if(mqttClient.connected()) {
    char top[128];
    if(strlen(settings.MQTT.rootTopic) > 0)
      snprintf(top, sizeof(top), "%s/%s", settings.MQTT.rootTopic, topic);
    else
      strlcpy(top, topic, sizeof(top));
    DEBUG_PRINT("MQTT Unsubscribed from:");
    DEBUG_PRINTLN(top);
    return mqttClient.unsubscribe(top);
  }
  return true;
}
bool MQTTClass::subscribe(const char *topic) {
  if(mqttClient.connected()) {
    esp_task_wdt_reset(); // Make sure we do not reboot here.
    char top[128];
    if(strlen(settings.MQTT.rootTopic) > 0)
      snprintf(top, sizeof(top), "%s/%s", settings.MQTT.rootTopic, topic);
    else
      strlcpy(top, topic, sizeof(top));
    DEBUG_PRINT("MQTT Subscribed to:");
    DEBUG_PRINTLN(top);
    return mqttClient.subscribe(top);
  }
  return true;
}
bool MQTTClass::publish(const char *topic, const char *payload, bool retain) {
  if(mqttClient.connected()) {
    char top[128];
    if(strlen(settings.MQTT.rootTopic) > 0)
      snprintf(top, sizeof(top), "%s/%s", settings.MQTT.rootTopic, topic);
    else
      strlcpy(top, topic, sizeof(top));
    esp_task_wdt_reset(); // Make sure we do not reboot here.
    mqttClient.publish(top, payload, retain);
    return true;
  }
  return false;
}
bool MQTTClass::publish(const char *topic, uint32_t val, bool retain) {
  snprintf(g_content, sizeof(g_content), "%u", val);
  return this->publish(topic, g_content, retain);
}
bool MQTTClass::unpublish(const char *topic) {
  if(mqttClient.connected()) {
    char top[128];
    if(strlen(settings.MQTT.rootTopic) > 0)
      snprintf(top, sizeof(top), "%s/%s", settings.MQTT.rootTopic, topic);
    else
      strlcpy(top, topic, sizeof(top));
    esp_task_wdt_reset(); // Make sure we do not reboot here.
    mqttClient.publish(top, (const uint8_t *)"", 0, true);
    return true;
  }
  return false;

//  mqttClient.beginPublish(topic, 0, true);
//  mqttClient.endPublish();
}

bool MQTTClass::publishBuffer(const char *topic, uint8_t *data, uint16_t len, bool retain) {
  size_t res;
  uint16_t offset = 0;
  uint16_t to_write = len;
  uint16_t buff_len;
  esp_task_wdt_reset(); // Make sure we do not reboot here.
  mqttClient.beginPublish(topic, len, retain);
  do { 
    buff_len = to_write;
    if(buff_len > 128) buff_len = 128;
    res = mqttClient.write(data+offset, buff_len);
    offset += buff_len;
    to_write -= buff_len;
  } while(res == buff_len && to_write > 0);
  mqttClient.endPublish();
  return true;
}
bool MQTTClass::publishDisco(const char *topic, JsonObject &obj, bool retain) {
  serializeJson(obj, g_content, sizeof(g_content));
  this->publishBuffer(topic, (uint8_t *)g_content, strlen(g_content), retain);
  return true;
}
bool MQTTClass::publish(const char *topic, int8_t val, bool retain) {
  snprintf(g_content, sizeof(g_content), "%d", val);
  return this->publish(topic, g_content, retain);
}
bool MQTTClass::publish(const char *topic, uint8_t val, bool retain) {
  snprintf(g_content, sizeof(g_content), "%u", val);
  return this->publish(topic, g_content, retain);
}
bool MQTTClass::publish(const char *topic, uint16_t val, bool retain) {
  snprintf(g_content, sizeof(g_content), "%u", val);
  return this->publish(topic, g_content, retain);
}
bool MQTTClass::publish(const char *topic, bool val, bool retain) {
  snprintf(g_content, sizeof(g_content), "%s", val ? "true" : "false");
  return this->publish(topic, g_content, retain);
}
bool MQTTClass::connected() {
  if(settings.MQTT.enabled) return mqttClient.connected();
  return false;
}
