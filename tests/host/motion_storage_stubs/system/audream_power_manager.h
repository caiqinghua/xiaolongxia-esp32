#pragma once

#include <cassert>

class AudreamPowerManager {
public:
    static AudreamPowerManager& GetInstance() { static AudreamPowerManager value; return value; }
    bool BeginStorageRead() { ++acquires; if (!allowed) return false; ++reservations; return true; }
    void EndStorageRead() { assert(reservations > 0); --reservations; }
    bool allowed = true;
    bool sleeping = false;
    unsigned reservations = 0;
    unsigned acquires = 0;
};
