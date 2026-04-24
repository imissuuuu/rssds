#pragma once

struct AppSettings {
    int scrollRepeatDelayMs = 300;
    int scrollRepeatIntervalMs = 80;
};

bool settingsLoad(AppSettings& out);
bool settingsSave(const AppSettings& cfg);
