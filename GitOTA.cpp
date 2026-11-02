#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <memory>
#include <new>
#include <esp_task_wdt.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>
#include "Log.h"
#include "CABundle.h"
#include "ConfigSettings.h"
#include "GitOTA.h"
#include "Utils.h"
#include "Sockets.h"
#include "Somfy.h"
#include "Web.h"
#include "WResp.h"
#include "Network.h"
#include "MQTT.h"




extern ConfigSettings settings;
extern SocketEmitter sockEmit;
extern SomfyShadeController somfy;
extern rebootDelay_t rebootDelay;
extern Web webServer;
extern Network net;
extern MQTTClass mqtt;
extern Preferences pref;



#define MAX_BUFF_SIZE 4096
// Verify the GitHub server certificate against the root CAs in CABundle.h unless the user chose to
// update without verification.
static void setTrust(WiFiClientSecure &client, bool insecure) {
  if(insecure) client.setInsecure();
  else client.setCACertBundle(ca_bundle);
}
// Two TLS sessions at once need more heap than is reliably free, so an MQTT TLS connection is
// closed while GitHub is contacted and reopened when this goes out of scope.
class MQTTTLSPause {
  public:
    MQTTTLSPause() : _paused(mqtt.pauseTLS()) {}
    ~MQTTTLSPause() { if(this->_paused) mqtt.resume(); }
  private:
    bool _paused;
};
// Logs how much of the task's stack was never used once a GitHub connection is done, to show
// how close the TLS handshake came to overflowing it.
class StackReport {
  public:
    ~StackReport() { LOG_PRINTF("GitHub connection done, %u bytes of stack never used\n", (unsigned)uxTaskGetStackHighWaterMark(NULL)); }
};
// True when the last connection failed because the server certificate could not be verified.
static bool certFailed(WiFiClientSecure &client) {
  char msg[64];
  int err = client.lastError(msg, sizeof(msg));
  if(err == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED || err == MBEDTLS_ERR_X509_FATAL_ERROR || err == MBEDTLS_ERR_SSL_BAD_HS_CERTIFICATE) {
    LOG_PRINTF("GitHub certificate could not be verified: %s\n", msg);
    return true;
  }
  return false;
}
void GitRelease::setReleaseProperty(const char *key, const char *val) {
  if(strcmp(key, "id") == 0) this->id = atol(val);
  else if(strcmp(key, "draft") == 0) this->draft = toBoolean(val, false);
  else if(strcmp(key, "prerelease") == 0) this->preRelease = toBoolean(val, false);
  else if(strcmp(key, "name") == 0) strlcpy(this->name, val, sizeof(this->name));
  else if(strcmp(key, "tag_name") == 0) {
    this->version.parse(val);
  }
  else if(strcmp(key, "published_at") == 0) {
    //DEBUG_PRINTF("Key:[%s] Value:[%s]\n", key, val);
    this->releaseDate = Timestamp::parseUTCTime(val);
  }
}
void GitRelease::setAssetProperty(const char *key, const char *val) {
  if(strcmp(key, "name") == 0) {
    //DEBUG_PRINTLN(val);
    if(strstr(val, "littlefs.bin")) this->hasFS = true;
    else if(strstr(val, "ino.esp32.bin")) {
      if(strlen(this->hwVersions)) strcat(this->hwVersions, ",");
      strcat(this->hwVersions, "32");
    }
    else if(strstr(val, "ino.esp32s3.bin")) {
      if(strlen(this->hwVersions)) strcat(this->hwVersions, ",");
      strcat(this->hwVersions, "s3");
    }
    else if(strstr(val, "ino.esp32s2.bin")) {
      if(strlen(this->hwVersions)) strcat(this->hwVersions, ",");
      strcat(this->hwVersions, "s2");
    }
    else if(strstr(val, "ino.esp32c3.bin")) {
      if(strlen(this->hwVersions)) strcat(this->hwVersions, ",");
      strcat(this->hwVersions, "c3");
    }
    else if(strstr(val, "ino.esp32c2.bin")) {
      if(strlen(this->hwVersions)) strcat(this->hwVersions, ",");
      strcat(this->hwVersions, "c2");
    }
    else if(strstr(val, "ino.esp32c6.bin")) {
      if(strlen(this->hwVersions)) strcat(this->hwVersions, ",");
      strcat(this->hwVersions, "c6");
    }
    else if(strstr(val, "ino.esp32h2.bin")) {
      if(strlen(this->hwVersions)) strcat(this->hwVersions, ",");
      strcat(this->hwVersions, "h2");
    }
  }
}
void GitRelease::toJSON(JsonResponse &json) {
  Timestamp ts;
  char buff[20];
  sprintf(buff, "%llu", this->id);
  json.addElem("id", buff);
  json.addElem("name", this->name);
  json.addElem("date", ts.getISOTime(this->releaseDate));
  json.addElem("draft", this->draft);
  json.addElem("preRelease", this->preRelease);
  json.addElem("main", this->main);
  json.addElem("hasFS", this->hasFS);
  json.addElem("hwVersions", this->hwVersions);
  json.beginObject("version");
  this->version.toJSON(json);
  json.endObject();
}
#define ERR_CLIENT_OFFSET -50

int16_t GitRepo::getReleases(uint8_t num, bool insecure) {
  StackReport report;
  MQTTTLSPause pause;
  WiFiClientSecure sclient;
  setTrust(sclient, insecure);
  sclient.setHandshakeTimeout(3);
  uint8_t ndx = 0;
  uint8_t count = min((uint8_t)GIT_MAX_RELEASES, num);
  char url[128];
  memset(this->releases, 0x00, sizeof(GitRelease) * GIT_MAX_RELEASES);
  sprintf(url, "https://api.github.com/repos/" GIT_REPO "/releases?per_page=%d&page=1", count);
  GitRelease *main = &this->releases[GIT_MAX_RELEASES];
  main->releaseDate = Timestamp::now();
  main->id = 1;
  main->main = true;
  strcpy(main->version.name, "main");
  strcpy(main->name, "Main");
  strcpy(main->hwVersions, "32,s3,c3,s2");
  HTTPClient https;
  https.setReuse(false);
  if(https.begin(sclient, url)) {
    esp_task_wdt_reset();
    int httpCode = https.GET();
    DEBUG_PRINTF("[HTTPS] GET... code: %d\n", httpCode);
    if(httpCode > 0) {
      int len = https.getSize();
      DEBUG_PRINTF("[HTTPS] GET... code: %d - %d\n", httpCode, len);
      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        WiFiClient *stream = https.getStreamPtr();
        uint8_t buff[128] = {0};
        char jsonElem[32] = "";
        char jsonValue[128] = "";
        int arrTok = 0;
        int objTok = 0;
        bool inQuote = false;
        bool inElem = false;
        bool inValue = false;
        bool awaitValue = false;
        bool inAss = false;
        while(https.connected() && (len > 0 || len == -1) && ndx < count) {
          size_t size = stream->available();
          if(size) {
            esp_task_wdt_reset();
            int c = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));
            //Serial.write(buff, c);
            if(len > 0) len -= c;
            // Now we should have some data.
            for(uint8_t i = 0; i < c; i++) {
              // Read the buffer a byte at a time until we have a key value pair.
              char ch = static_cast<char>(buff[i]);
              if(ch == '[') {
                arrTok++;
                if(arrTok == 2 && strcmp(jsonElem, "assets") == 0) {
                  inElem = inValue = awaitValue = false;
                  inAss = true;
                  //DEBUG_PRINTF("%s: %d\n", jsonElem, arrTok);
                }
                else if(arrTok < 2) inAss = false;
              }
              else if(ch == ']') {
                arrTok--;
                if(arrTok < 2) inAss = false;
              }
              else if(ch == '{') {
                objTok++;
                if(objTok != 1 && !inAss) inElem = inValue = awaitValue = false;
              }
              else if(ch == '}') {
                objTok--;
                if(objTok == 0) ndx++;
              }
              else if(objTok == 1 || inAss) {
                // We only want data from the root object.
                //if(inAss) DEBUG_PRINT(ch);
                if(ch == '\"') {
                  inQuote = !inQuote;
                  if(inElem) {
                    inElem = false;
                    awaitValue = true;
                  }
                  else if(inValue) {
                    inValue = false;
                    inElem = false;
                    awaitValue = false;
                    if(inAss)
                      this->releases[ndx].setAssetProperty(jsonElem, jsonValue);
                    else
                      this->releases[ndx].setReleaseProperty(jsonElem, jsonValue);
                    memset(jsonElem, 0x00, sizeof(jsonElem));
                    memset(jsonValue, 0x00, sizeof(jsonValue));
                  }
                  else if(awaitValue) inValue = true;
                  else {
                    inElem = true;
                    awaitValue = false;
                  }
                }
                else if(awaitValue) {
                  if(ch != ' ' && ch != ':') {
                    strncat(jsonValue, &ch, 1);
                    awaitValue = false;
                    inValue = true;
                  }
                }
                else if((!inQuote && ch == ',') || ch == '\r' || ch == '\n') {
                  inElem = inValue = awaitValue = false;
                  if(strlen(jsonElem) > 0) {
                    if(inAss)
                      this->releases[ndx].setAssetProperty(jsonElem, jsonValue);
                    else
                      this->releases[ndx].setReleaseProperty(jsonElem, jsonValue);
                  }
                  memset(jsonElem, 0x00, sizeof(jsonElem));
                  memset(jsonValue, 0x00, sizeof(jsonValue));
                }
                else {
                  if(inElem) {
                    if(strlen(jsonElem) < sizeof(jsonElem) - 1) strncat(jsonElem, &ch, 1);
                  }
                  else if(inValue) {
                    if(strlen(jsonValue) < sizeof(jsonValue) - 1) strncat(jsonValue, &ch, 1);
                  }
                }
              }
            }
            delay(1);
          }
          //else break;
        }
      }
      else {
        LOG_PRINTF("GitHub release list request failed with HTTP %d\n", httpCode);
        https.end();
        sclient.stop();
        return httpCode;
      }
    }
    else if(certFailed(sclient)) {
      https.end();
      sclient.stop();
      return ERR_CERT_VERIFY;
    }
    else {
      LOG_PRINTF("GitHub release list request failed: %s\n", HTTPClient::errorToString(httpCode).c_str());
      https.end();
      sclient.stop();
      return httpCode;
    }
    https.end();  
    sclient.stop();
  }
  else {
    LOG_PRINTLN("GitHub release list request could not be started");
    return HTTPC_ERROR_CONNECTION_REFUSED;
  }
  LOG_PRINTF("Found %u releases on GitHub\n", ndx);
  settings.printAvailHeap();
  return 0;
}
void GitRepo::toJSON(JsonResponse &json) {
  json.beginObject("fwVersion");
  settings.fwVersion.toJSON(json);
  json.endObject();
  json.beginObject("appVersion");
  settings.appVersion.toJSON(json);
  json.endObject();
  json.addElem("error", (int32_t)this->error);
  json.beginArray("releases");
  for(uint8_t i = 0; i < GIT_MAX_RELEASES + 1; i++) {
    if(this->releases[i].id == 0) continue;
    json.beginObject();
    this->releases[i].toJSON(json);
    json.endObject();
  }
  json.endArray();
}
#define UPDATE_ERR_OFFSET 20
#define ERR_DOWNLOAD_HTTP -40
#define ERR_DOWNLOAD_BUFFER -41
#define ERR_DOWNLOAD_CONNECTION -42

void GitUpdater::loop() {
  if(!net.connected()) return;
  if(this->status == GIT_STATUS_READY) {
    if((settings.checkForUpdate || settings.autoUpdate) && 
      (millis() - net.connectTime > 60000) && // Wait a minute before checking after connection.
      (millis() - this->lastCheck > 86400000 || this->lastCheck == 0) && !rebootDelay.reboot) { // 1 day
      this->checkForUpdate();
    }
    else if(settings.autoUpdate && this->updateAvailable && !rebootDelay.reboot && this->autoUpdateDue()) {
      LOG_PRINTF("Installing %s automatically\n", this->latest.name);
      this->autoUpdateTime = millis();
      pref.begin("GIT");
      pref.putString("autoVer", this->latest.name);
      pref.end();
      strlcpy(this->targetRelease, this->latest.name, sizeof(this->targetRelease));
      this->status = GIT_AWAITING_UPDATE;
    }
  }
  else if(this->status == GIT_AWAITING_UPDATE) {
    DEBUG_PRINTLN("Starting update process.........");
    this->status = GIT_UPDATING;
    this->beginUpdate(this->targetRelease);
    // The update failed before anything was written, so a later automatic install may try it again.
    if(!rebootDelay.reboot) {
      pref.begin("GIT");
      pref.remove("autoVer");
      pref.end();
    }
    this->status = GIT_STATUS_READY;
    this->emitUpdateCheck();
  }
  else if(this->status == GIT_UPDATE_CANCELLING) {
    DEBUG_PRINTLN("Cancelling update process..........");
    if(!this->lockFS) {
      this->status = GIT_UPDATE_CANCELLED;
      this->cancelled = true;
      this->emitUpdateCheck();
    }
  }
}
// An automatic install starts between 3:00 and 4:00 local time, when the shades are least likely
// to be needed, and at most once a day so a failing one is not repeated over and over. Without the
// time from NTP it starts right away. A version that was installed automatically before is not
// installed again, in case a release reports a wrong version and would be installed after every boot.
bool GitUpdater::autoUpdateDue() {
  // This runs on every pass of the loop, so only look once a minute.
  static uint32_t lastLook = 0;
  if(lastLook != 0 && millis() - lastLook < 60000) return false;
  lastLook = millis();
  if(this->autoUpdateTime != 0 && millis() - this->autoUpdateTime < 82800000) return false; // 23 hours
  time_t now = time(nullptr);
  if(now >= 1700000000) {
    struct tm dt;
    localtime_r(&now, &dt);
    if(dt.tm_hour != 3) return false;
  }
  char tried[sizeof(this->latest.name)] = "";
  pref.begin("GIT");
  pref.getString("autoVer", tried, sizeof(tried));
  pref.end();
  return strcmp(tried, this->latest.name) != 0;
}
void GitUpdater::checkForUpdate() {
  if(this->status != 0) return; // If we are already checking.
  DEBUG_PRINTLN("Check github for updates...");
  
  this->status = GIT_STATUS_CHECK;
  MQTTTLSPause pause;
  settings.printAvailHeap();  
  this->lastCheck = millis();
  // Keep the release list off the stack, the TLS connection below needs it.
  std::unique_ptr<GitRepo> prepo(new (std::nothrow) GitRepo);
  if(!prepo) this->error = -1;
  else if((this->error = this->checkInternet()) == 0) {
    GitRepo &repo = *prepo;
    this->updateAvailable = false;
    this->error = repo.getReleases(2);
    if(this->error == 0) { // Get 2 releases so we can filter our pre-releases
      this->setCurrentRelease(repo);
    }
    else {
      this->emitUpdateCheck();
    }
  }
  this->status = GIT_STATUS_READY;
}
void GitUpdater::setCurrentRelease(GitRepo &repo) {
  this->updateAvailable = false;
  for(uint8_t i = 0; i < 2; i++) {
    if(repo.releases[i].draft || repo.releases[i].preRelease || repo.releases[i].id == 0) continue;
    // Compare the versions.  
    this->latest.copy(repo.releases[i].version);
    if(repo.releases[i].version.compare(settings.fwVersion) > 0) {
      // We have a new release.
      this->updateAvailable = true;
    }
    break;
  }
  this->emitUpdateCheck();
}
void GitUpdater::toJSON(JsonResponse &json) {
  json.addElem("available", this->updateAvailable);
  json.addElem("status", this->status);
  json.addElem("error", (int32_t)this->error);
  json.addElem("cancelled", this->cancelled);
  json.addElem("checkForUpdate", settings.checkForUpdate);
  json.addElem("autoUpdate", settings.autoUpdate);
  json.addElem("checked", this->lastCheck != 0);
  json.addElem("inetAvailable", this->inetAvailable);
  json.beginObject("fwVersion");
  settings.fwVersion.toJSON(json);
  json.endObject();
  json.beginObject("appVersion");
  settings.appVersion.toJSON(json);
  json.endObject();
  json.beginObject("latest");
  this->latest.toJSON(json);
  json.endObject();
}
void GitUpdater::emitUpdateCheck(uint8_t num) {
  JsonSockEvent *json = sockEmit.beginEmit("fwStatus");
  json->beginObject();
  json->addElem("available", this->updateAvailable);
  json->addElem("status", this->status);
  json->addElem("error", (int32_t)this->error);
  json->addElem("cancelled", this->cancelled);
  json->addElem("checkForUpdate", settings.checkForUpdate);
  json->addElem("autoUpdate", settings.autoUpdate);
  json->addElem("checked", this->lastCheck != 0);
  json->addElem("inetAvailable", this->inetAvailable);
  json->beginObject("fwVersion");
  settings.fwVersion.toJSON(json);
  json->endObject();
  json->beginObject("appVersion");
  settings.appVersion.toJSON(json);
  json->endObject();
  json->beginObject("latest");
  this->latest.toJSON(json);
  json->endObject();
  json->endObject();
  sockEmit.endEmit(num);
}
int GitUpdater::checkInternet() {
  int err = 500;
  uint32_t t = millis();
  WiFiClientSecure sclient;
  // This only probes the connection and nothing from the response is used, so it does not need
  // to verify the certificate. A certificate problem is reported when the releases are fetched.
  sclient.setInsecure();
  sclient.setHandshakeTimeout(3);
  esp_task_wdt_reset();
  HTTPClient https;
  https.setReuse(false);
  if(https.begin(sclient, "https://github.com/" GIT_REPO)) {
    https.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    https.setTimeout(3000);
    esp_task_wdt_reset();
    int httpCode = https.sendRequest("HEAD");
    esp_task_wdt_reset();
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY || httpCode == HTTP_CODE_FOUND) {
      err = 0;
      DEBUG_PRINTF("Internet is Available: %ldms\n", millis() - t);
      this->inetAvailable = true;
    }
    else {
      err = httpCode;
      DEBUG_PRINTF("Internet is Unavailable: %d: %ldms\n", err, millis() - t);
      this->inetAvailable = false;
    }
    https.end();
    sclient.stop();
  }
  esp_task_wdt_reset();
  return err;
}
void GitUpdater::emitDownloadProgress(size_t total, size_t loaded, const char *evt) { this->emitDownloadProgress(255, total, loaded, evt); }
void GitUpdater::emitDownloadProgress(uint8_t num, size_t total, size_t loaded, const char *evt) {
  JsonSockEvent *json = sockEmit.beginEmit(evt);
  json->beginObject();
  json->addElem("ver", this->targetRelease);
  json->addElem("part", (int32_t)this->partition);
  json->addElem("file", this->currentFile);
  json->addElem("total", (uint32_t)total);
  json->addElem("loaded", (uint32_t)loaded);
  json->addElem("error", (uint32_t)this->error);
  json->endObject();
  sockEmit.endEmit(num);
  /*
  char buf[420];
  snprintf(buf, sizeof(buf), "{\"ver\":\"%s\",\"part\":%d,\"file\":\"%s\",\"total\":%d,\"loaded\":%d, \"error\":%d}", this->targetRelease, this->partition, this->currentFile, total, loaded, this->error);
  if(num >= 255) sockEmit.sendToClients(evt, buf);
  else sockEmit.sendToClient(num, evt, buf);
  */
  sockEmit.loop();
  webServer.loop();
}
void GitUpdater::setFirmwareFile() {
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    switch(ci.model) {
      case esp_chip_model_t::CHIP_ESP32S3:
        strcpy(this->currentFile, "SomfyController.ino.esp32s3.bin");
        break;
      case esp_chip_model_t::CHIP_ESP32S2:
        strcpy(this->currentFile, "SomfyController.ino.esp32s2.bin");
        break;
      case esp_chip_model_t::CHIP_ESP32C3:
        strcpy(this->currentFile, "SomfyController.ino.esp32c3.bin");
        break;
      default:
        strcpy(this->currentFile, "SomfyController.ino.esp32.bin");
        break;
    }
}

bool GitUpdater::beginUpdate(const char *version) {
  DEBUG_PRINTLN("Begin update called...");
  if(strcmp(version, "Main") == 0)  strcpy(this->baseUrl, "https://raw.githubusercontent.com/" GIT_REPO "/" GIT_MAIN_BRANCH "/");
  else sprintf(this->baseUrl, "https://github.com/" GIT_REPO "/releases/download/%s/", version);
  
  strcpy(this->targetRelease, version);
  this->emitUpdateCheck();
  this->setFirmwareFile();
  this->partition = U_FLASH;
  this->lockFS = this->cancelled = false;
  this->error = 0;
  this->error = this->downloadFile();
  if(this->error == 0 && !this->cancelled) {
    somfy.commit();
    strcpy(this->currentFile, "SomfyController.littlefs.bin");
    this->partition = U_SPIFFS;
    this->lockFS = true;
    this->error = this->downloadFile();
    this->lockFS = false;
    if(this->error == 0) {
      settings.fwVersion.parse(version);
      delay(100);
      DEBUG_PRINTLN("Committing Configuration...");
      somfy.commit();
    }
    rebootDelay.reboot = true;
    rebootDelay.rebootTime = millis() + 500;
  }
  this->insecure = false;
  this->status = GIT_UPDATE_COMPLETE;
  this->emitUpdateCheck();
  return true;
}
bool GitUpdater::recoverFilesystem() {
  sprintf(this->baseUrl, "https://github.com/" GIT_REPO "/releases/download/%s/", settings.fwVersion.name);
  strcpy(this->currentFile, "SomfyController.littlefs.bin");
  this->status = GIT_UPDATING;
  this->partition = U_SPIFFS;
  this->lockFS = true;
  this->error = this->downloadFile();
  this->lockFS = false;
  if(this->error == 0) {
    delay(100);
    DEBUG_PRINTLN("Committing Configuration...");
    somfy.commit();
  }
  this->status = GIT_UPDATE_COMPLETE;
  rebootDelay.reboot = true;
  rebootDelay.rebootTime = millis() + 500;
  return true;
}
bool GitUpdater::endUpdate() { return true; }
int8_t GitUpdater::downloadFile() {
  StackReport report;
  DEBUG_PRINTF("Begin update %s\n", this->currentFile);
  MQTTTLSPause pause;
  WiFiClientSecure sclient;
  setTrust(sclient, this->insecure);
  HTTPClient https;
  char url[196];
  sprintf(url, "%s%s", this->baseUrl, this->currentFile);
  DEBUG_PRINTLN(url);
  esp_task_wdt_reset();
  if(https.begin(sclient, url)) {
    https.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    DEBUG_PRINT("[HTTPS] GET...\n");
    int httpCode = https.GET();
    if(httpCode > 0) {
      size_t len = https.getSize();
      size_t total = 0;
      uint8_t pct = 0;
      DEBUG_PRINTF("[HTTPS] GET... code: %d - %d\n", httpCode, len);
      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY || httpCode == HTTP_CODE_FOUND) {
        WiFiClient *stream = https.getStreamPtr();
        if(!Update.begin(len, this->partition)) {
          LOG_PRINTLN("Update Error detected!!!!!");
          Update.printError(Serial);
          https.end();
          return -(Update.getError() + UPDATE_ERR_OFFSET);
        }
        uint8_t *buff = (uint8_t *)malloc(MAX_BUFF_SIZE);
        if(buff) {
          this->emitDownloadProgress(len, total);
          int timeouts = 0;
          while(https.connected() && (len > 0 || len == -1) && total < len) {
            size_t size = stream->available();
            esp_task_wdt_reset();
            if(size) {
              timeouts = 0;
              if(this->cancelled && !this->lockFS) {
                Update.abort();
                free(buff);
                https.end();
                return -(Update.getError() + UPDATE_ERR_OFFSET);
              }
              int c = stream->readBytes(buff, ((size > MAX_BUFF_SIZE) ? MAX_BUFF_SIZE : size));
              total += c;
              //DEBUG_PRINTLN(total);
              if (Update.write(buff, c) != c) {
                Update.printError(Serial);
                LOG_PRINTF("Upload of %s aborted invalid size %d\n", url, c);
                free(buff);
                https.end();
                sclient.stop();
                return -(Update.getError() + UPDATE_ERR_OFFSET);
              }
              // Calculate the percentage.
              uint8_t p = (uint8_t)floor(((float)total / (float)len) * 100.0f);
              if(p != pct) {
                pct = p;
                DEBUG_PRINTF("LEN:%d TOTAL:%d %d%%\n", len, total, pct);
                this->emitDownloadProgress(len, total);
              }
              delay(1);
              if(total >= len) {
                if(!Update.end(true)) {
                  LOG_PRINTLN("Error downloading update...");
                  Update.printError(Serial);
                }
                else {
                  DEBUG_PRINTLN("Update.end Called...");
                }
                https.end();
                sclient.stop();
              }
            }
            else {
              timeouts++;
              if(timeouts >= 500) {
                Update.abort();
                https.end();
                free(buff);
                DEBUG_PRINTLN("Stream timeout!!!");
                return -43;
              }
              sockEmit.loop();
              webServer.loop();
              delay(100);
            }
          }
          free(buff);
          if(len > total) {
            Update.abort();
            somfy.commit();
            LOG_PRINTLN("Error downloading file!!!");
            return -42;
          }
          else
            DEBUG_PRINTF("Update %s complete\n", this->currentFile);
        }
        else {
          // TODO: memory allocation error.
          LOG_PRINTLN("Unable to allocate memory for update!!!");
        }
      }
      else {
        LOG_PRINTF("Invalid HTTP Code... %d", httpCode);
        return httpCode;
      }
    }        
    else {
      LOG_PRINTF("Invalid HTTP Code: %d\n", httpCode);
      // Do not report success when nothing was downloaded.
      int8_t err = certFailed(sclient) ? ERR_CERT_VERIFY : ERR_DOWNLOAD_CONNECTION;
      https.end();
      sclient.stop();
      return err;
    }
    https.end(); 
    sclient.stop(); 
    DEBUG_PRINTF("End update %s\n", this->currentFile);
  }
  else return ERR_DOWNLOAD_CONNECTION;
  esp_task_wdt_reset();
  return 0;
}
