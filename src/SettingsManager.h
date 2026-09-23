#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <Preferences.h>
#include "global.h"

struct AppSettings {
    String mqttServer = "";
    String mqttUsername = "";
    String mqttPassword = "";
    String mqttRootTopic = "fingerprintDoorbell";
    String sensorPairingCode = "";
    bool   sensorPairingValid = false;
};

class SettingsManager {       
  private:
    AppSettings appSettings;

    void saveAppSettings();

  public:
    bool loadAppSettings();

    AppSettings getAppSettings();
    void saveAppSettings(AppSettings newSettings);

    bool deleteAppSettings();

    String generateNewPairingCode();

};

#endif