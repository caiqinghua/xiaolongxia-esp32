#pragma once
#include "web_socket.h"
#include <memory>

class HostNetwork {
public:
    bool create_ok = true;
    std::unique_ptr<WebSocket> CreateWebSocket(int) {
        return create_ok ? std::make_unique<WebSocket>() : nullptr;
    }
};
class Board {
public:
    HostNetwork network;
    bool available = true;
    static Board& GetInstance() { static Board board; return board; }
    HostNetwork* GetNetwork() { return available ? &network : nullptr; }
    std::string GetUuid() { return "host-client"; }
};
