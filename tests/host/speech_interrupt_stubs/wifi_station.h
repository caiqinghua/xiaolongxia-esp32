#pragma once
class WifiStation {
public:
    bool connected = true;
    static WifiStation& GetInstance() { static WifiStation value; return value; }
    bool IsConnected() const { return connected; }
};
