#pragma once
#include <string>
struct ServerConfig {
    static std::string GetWebSocketUrl() { return "ws://host.test"; }
};
