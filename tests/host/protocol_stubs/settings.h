#pragma once
#include <string>
class Settings {
public:
    inline static int version = 1;
    Settings(const char*, bool) {}
    std::string GetString(const char*) { return {}; }
    int GetInt(const char*) { return version; }
};
