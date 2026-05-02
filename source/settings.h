#pragma once

struct AppSettings {
    int scrollRepeatDelayMs = 300;
    int scrollRepeatIntervalMs = 80;
    int theme = 0; // 0=Light, 1=Dark
};

bool settingsLoad(AppSettings& out);
bool settingsSave(const AppSettings& cfg);
