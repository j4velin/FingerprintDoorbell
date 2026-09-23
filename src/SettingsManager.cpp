#include "SettingsManager.h"
#include <Crypto.h>


bool SettingsManager::loadAppSettings() {
    Preferences preferences;
    if (preferences.begin("appSettings", true)) {
        appSettings.mqttServer = preferences.getString("mqttServer", String(""));
        appSettings.mqttUsername = preferences.getString("mqttUsername", String(""));
        appSettings.mqttPassword = preferences.getString("mqttPassword", String(""));
        appSettings.mqttRootTopic = preferences.getString("mqttRootTopic", String("fingerprintDoorbell"));
        appSettings.sensorPairingCode = preferences.getString("pairingCode", "");
        appSettings.sensorPairingValid = preferences.getBool("pairingValid", false);
        preferences.end();
        return true;
    } else {
        return false;
    }
}
   

void SettingsManager::saveAppSettings() {
    Preferences preferences;
    preferences.begin("appSettings", false); 
    preferences.putString("mqttServer", appSettings.mqttServer);
    preferences.putString("mqttUsername", appSettings.mqttUsername);
    preferences.putString("mqttPassword", appSettings.mqttPassword);
    preferences.putString("mqttRootTopic", appSettings.mqttRootTopic);
    preferences.putString("pairingCode", appSettings.sensorPairingCode);
    preferences.putBool("pairingValid", appSettings.sensorPairingValid);
    preferences.end();
}



AppSettings SettingsManager::getAppSettings() {
    return appSettings;
}

void SettingsManager::saveAppSettings(AppSettings newSettings) {
    appSettings = newSettings;
    saveAppSettings();
}

bool SettingsManager::deleteAppSettings() {
    bool rc;
    Preferences preferences;
    rc = preferences.begin("appSettings", false); 
    if (rc)
        rc = preferences.clear();
    preferences.end();
    return rc;
}


String SettingsManager::generateNewPairingCode() {

    /* Create a SHA256 hash */
    SHA256 hasher;

    /* Put some unique values as input in our new hash */
    hasher.doUpdate( String(esp_random()).c_str() ); // random number
    hasher.doUpdate( String(millis()).c_str() ); // time since boot
    hasher.doUpdate(appSettings.mqttUsername.c_str());
    hasher.doUpdate(appSettings.mqttPassword.c_str());

    /* Compute the final hash */
    byte hash[SHA256_SIZE];
    hasher.doFinal(hash);
    
    // Convert our 32 byte hash to 32 chars long hex string. When converting the entire hash to hex we would need a length of 64 chars.
    // But because we only want a length of 32 we only use the first 16 bytes of the hash. I know this will increase possible collisions,
    // but for detecting a sensor replacement (which is the use-case here) it will still be enough.
    char hexString[33];
    hexString[32] = 0; // null terminatation byte for converting to string later
    for (byte i=0; i < 16; i++) // use only the first 16 bytes of hash
    {
        sprintf(&hexString[i*2], "%02x", hash[i]);
    }

    return String((char*)hexString);
}

