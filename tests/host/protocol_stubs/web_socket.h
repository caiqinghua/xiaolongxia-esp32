#pragma once
#include <functional>
#include <string>
#include <vector>
#include <map>
#include "freertos/event_groups.h"

class WebSocket {
public:
    inline static WebSocket* current = nullptr;
    inline static bool connect_ok = true;
    inline static bool send_ok = true;
    inline static bool disconnect_ready = true;
    inline static int destructions = 0;
    inline static std::function<void(WebSocket&)> hello_hook;
    bool connected = false;
    std::vector<std::string> sent;
    std::map<std::string, std::string> headers;
    std::string url;
    std::function<void(const char*, size_t, bool)> data_callback;
    std::function<void()> disconnect_callback;

    WebSocket() { current = this; }
    ~WebSocket() {
        assert(host_event_groups > 0);
        Disconnect();
        current = nullptr;
        ++destructions;
    }
    void SetHeader(const char* name, const char* value) { headers[name] = value; }
    bool IsConnected() const { return connected; }
    bool Connect(const char* value) { url = value; connected = connect_ok; return connected; }
    int GetLastError() const { return -1; }
    bool Send(const std::string& text) {
        if (!send_ok) return false;
        sent.push_back(text);
        if (hello_hook && text.find("\"type\":\"hello\"") != std::string::npos) hello_hook(*this);
        return true;
    }
    bool Send(const void* data, size_t size, bool) {
        if (!send_ok) return false;
        sent.emplace_back(static_cast<const char*>(data), size);
        return true;
    }
    void OnData(std::function<void(const char*, size_t, bool)> callback) { data_callback = std::move(callback); }
    void OnDisconnected(std::function<void()> callback) { disconnect_callback = std::move(callback); }
    void Receive(const std::string& text, bool binary = false) {
        // Deliberately omit the trailing NUL to catch unbounded string parsing.
        std::vector<char> bytes(text.begin(), text.end());
        data_callback(bytes.data(), bytes.size(), binary);
    }
    void Disconnect() {
        if (!connected) return;
        connected = false;
        if (disconnect_callback) disconnect_callback();
    }
#if CONFIG_AUDREAM_WATCH_UI
    bool DisconnectWithin(uint32_t) {
        if (!disconnect_ready) return false;
        Disconnect();
        return true;
    }
#endif
};
