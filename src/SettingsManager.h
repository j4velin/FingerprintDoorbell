#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <Preferences.h>
#include "global.h"

struct NetworkSettings {
    String hostname = "";
};

struct AppSettings {
    String mqttServer = "";
    String mqttUsername = "";
    String mqttPassword = "";
    String mqttRootTopic = "fingerprintDoorbell";
    String sensorPin = "00000000";
    String sensorPairingCode = "";
    bool   sensorPairingValid = false;
};

class SettingsManager {       
  private:
    NetworkSettings networkSettings;
    AppSettings appSettings;

    void saveNetworkSettings();
    void saveAppSettings();

  public:
    bool loadNetworkSettings();
    bool loadAppSettings();

    NetworkSettings getNetworkSettings();
    void saveNetworkSettings(NetworkSettings newSettings);
    
    AppSettings getAppSettings();
    void saveAppSettings(AppSettings newSettings);

    bool deleteAppSettings();
    bool deleteNetworkSettings();

    String generateNewPairingCode();

};

#endif