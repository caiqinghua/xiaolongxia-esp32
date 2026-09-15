#pragma once
#include <atomic>
class WifiStation {
public:
    std::atomic<bool> connected{true};
    static WifiStation& GetInstance() { static WifiStation station; return station; }
    bool IsConnected() const { return connected.load(); }
};
