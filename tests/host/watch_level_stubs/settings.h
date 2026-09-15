#pragma once
class Settings {
public:
    inline static int legacy_volume = 0;
    Settings(const char*, bool) {}
    int GetInt(const char*, int) { return legacy_volume; }
    void SetInt(const char*, int value) { legacy_volume = value; }
};
