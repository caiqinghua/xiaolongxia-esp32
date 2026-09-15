#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_


#include "protocol.h"
#include "watch/watch_voice_cloud.h"
#include <optional>

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

// Lifecycle, sends, Poll, and callback registration belong to the main task.
// Network callbacks only access a separately owned ReceiveSession.
class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool CloseAudioChannelWithin(uint32_t timeout_ms) override;
    bool HasPendingClose() const override { return closing_; }
    bool IsAudioChannelOpened() const override;
    bool SupportsConversationTurns() const override;
    bool SuspendIncomingTurn() override;
    void Poll() override;
    bool SetCloudAuthorization(const watch_voice::Bootstrap& bootstrap, const std::string& device_id);
    void ClearCloudAuthorization();

private:
    struct ReceiveSession;
    std::shared_ptr<ReceiveSession> receive_session_;
    std::unique_ptr<WebSocket> websocket_;
    int version_ = 1;
    bool closing_ = false;
    bool cloud_authorization_required_ = false;
    std::optional<watch_voice::Bootstrap> cloud_authorization_;
    std::string cloud_device_id_;

    bool CloudAuthorizationReady() const;

    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
};

#endif
