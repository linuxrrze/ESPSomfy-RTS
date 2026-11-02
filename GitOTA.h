#ifndef GITOTA_H
#define GITOTA_H
#include <Arduino.h>
#include "ArduinoJsonConfig.h"
#include <time.h>
#include "ConfigSettings.h"
#include "WResp.h"

// GitHub repository the updates come from, and the branch the Main channel installs. The Main
// channel downloads the binaries committed in the root of that branch.
#define GIT_REPO "linuxrrze/ESPSomfy-RTS"
#define GIT_MAIN_BRANCH "main"
#define GIT_MAX_RELEASES 5
#define GIT_STATUS_READY 0
#define GIT_STATUS_CHECK 1
#define GIT_AWAITING_UPDATE 2
#define GIT_UPDATING 3
#define GIT_UPDATE_COMPLETE 4
#define GIT_UPDATE_CANCELLING 5
#define GIT_UPDATE_CANCELLED 6
// The GitHub server certificate could not be verified against the root CAs in CABundle.h.
#define ERR_CERT_VERIFY -44

class GitRelease {
  public:
    uint64_t id = 0;
    bool draft = false;
    bool preRelease = false;
    bool main = false;
    bool hasFS = false;
    char hwVersions[128] = "";
    time_t releaseDate;
    char name[32] = "";   
    appver_t version;
    void setReleaseProperty(const char *key, const char *val);
    void setAssetProperty(const char *key, const char *val);
    void toJSON(JsonResponse &json);
};
class GitRepo {
  public:
    int16_t getReleases(uint8_t num = GIT_MAX_RELEASES, bool insecure = false);
    GitRelease releases[GIT_MAX_RELEASES + 1];
    // The result of getReleases, sent along so the web UI can say why only Main is listed.
    int16_t error = 0;
    void toJSON(JsonResponse &json);
};
class GitUpdater {
  public:
    bool lockFS = false;
    bool canCancel = true;
    uint8_t status = 0;
    uint32_t lastCheck = 0;
    // When latest was last read from GitHub, by the update check or the update dialog.
    uint32_t latestTime = 0;
    bool updateAvailable = false;
    bool inetAvailable = false;
    appver_t latest;
    bool cancelled = false;
    int16_t error = 0;
    // When the last automatic install was started, to try a failed one again a day later.
    uint32_t autoUpdateTime = 0;
    char targetRelease[32];
    char currentFile[64] = "";
    char baseUrl[128] = "";
    int partition = 0;
    // Skip the certificate check for the current update. Set when the user chooses to update
    // although the GitHub certificate could not be verified.
    bool insecure = false;
    void checkForUpdate();
    bool autoUpdateDue();
    bool beginUpdate(const char *release);
    bool endUpdate();
    int8_t downloadFile();
    void setFirmwareFile();
    void setCurrentRelease(GitRepo &repo);
    void loop();
    void toJSON(JsonResponse &json);
    bool recoverFilesystem();
    int checkInternet();
    void emitUpdateCheck(uint8_t num=255);
    void emitDownloadProgress(size_t total, size_t loaded, const char *evt = "updateProgress");
    void emitDownloadProgress(uint8_t num, size_t total, size_t loaded, const char *evt = "updateProgress");    
};
#endif
