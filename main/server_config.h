#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H

#include <string>

class ServerConfig {
public:
    // Get server URLs with NVS override support
    static std::string GetOtaUrl();
    static std::string GetWebSocketUrl();
    static std::string GetVisionUrl();

    // Set server URLs to NVS
    static void SetOtaUrl(const std::string& url);
    static void SetWebSocketUrl(const std::string& url);
    static void SetVisionUrl(const std::string& url);

    // Clear all server URL overrides (use defaults)
    static void ClearAll();

    // Get configuration info for display
    static std::string GetConfigInfo();
};

#endif
