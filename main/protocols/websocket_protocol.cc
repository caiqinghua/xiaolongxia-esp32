#include "websocket_protocol.h"
#include "assistant_wire_format.h"
#include "assistant_receive_queue.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"
#include "server_config.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "assets/lang_config.h"

#define TAG "WS"

struct WebsocketProtocol::ReceiveSession {
    AssistantReceiveQueue queue;
    EventGroupHandle_t event_group = xEventGroupCreate();
    const std::function<void()> notify;

    ReceiveSession(int version, std::function<void()> callback)
#if CONFIG_AUDREAM_WATCH_UI
        : queue(version, true), notify(std::move(callback)) {}
#else
        : queue(version), notify(std::move(callback)) {}
#endif
    ~ReceiveSession() { if (event_group) vEventGroupDelete(event_group); }
    void Wake() {
        xEventGroupSetBits(event_group, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
        if (notify) notify();
    }
};

WebsocketProtocol::WebsocketProtocol() = default;

WebsocketProtocol::~WebsocketProtocol() {
    on_audio_channel_closed_ = nullptr;
    CloseAudioChannel(false);
    // Destruction is a final join, never permission to free a live transport.
    // Normal handoffs must first finish CloseAudioChannelWithin successfully.
    websocket_.reset();
    receive_session_.reset();
}

bool WebsocketProtocol::Start() {
    // Only connect to server when audio channel is needed
    return true;
}

bool WebsocketProtocol::CloudAuthorizationReady() const {
    return !cloud_authorization_required_ || (cloud_authorization_ &&
        cloud_authorization_->Usable(static_cast<uint64_t>(esp_timer_get_time() / 1000)));
}

bool WebsocketProtocol::SetCloudAuthorization(const watch_voice::Bootstrap& bootstrap, const std::string& device_id) {
    if (websocket_ || receive_session_ || closing_) return false;
    cloud_authorization_required_ = true;
    cloud_authorization_.reset();
    cloud_device_id_.clear();
    if (!watch_voice::ValidDeviceId(device_id) || !watch_voice::ValidAccessToken(bootstrap.access_token) ||
        (!bootstrap.settings.SupportsManualRuntime() && !bootstrap.settings.SupportsWakeRuntime()) ||
        !assistant_wire::IsSupportedVersion(bootstrap.protocol_version) ||
        bootstrap.settings.revision == 0 || bootstrap.settings.applied_revision > bootstrap.settings.revision ||
        !watch_voice::ValidWebSocketUrl(bootstrap.websocket_url) ||
        !bootstrap.Usable(static_cast<uint64_t>(esp_timer_get_time() / 1000))) return false;
    cloud_authorization_ = bootstrap;
    cloud_device_id_ = device_id;
    return true;
}

void WebsocketProtocol::ClearCloudAuthorization() {
    cloud_authorization_required_ = true;
    cloud_authorization_.reset();
    cloud_device_id_.clear();
    CloseAudioChannel(false);
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!packet || !IsAudioChannelOpened() || !receive_session_->queue.CanSendAudio()) {
        return false;
    }

    bool sent = false;
    if (version_ == 1 && !packet->payload.empty() &&
        packet->payload.size() <= assistant_wire::kMaxAudioBytes) {
        sent = websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    } else {
        std::string serialized;
        if (!assistant_wire::EncodeAudio(version_, packet->payload.data(), packet->payload.size(),
                                         packet->timestamp, serialized)) return false;
        sent = websocket_->Send(serialized.data(), serialized.size(), true);
    }
    if (!sent) {
        ESP_LOGE(TAG, "Failed to send audio packet (%u bytes)", static_cast<unsigned>(packet->payload.size()));
        SetError(Lang::Strings::SERVER_ERROR);
    }
    return sent;
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (closing_ || !CloudAuthorizationReady() || text.empty() || text.size() > assistant_wire::kMaxJsonBytes ||
        websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    std::string message = text;
    bool new_turn = false;
    if (receive_session_ && !receive_session_->queue.PrepareControl(message, new_turn)) {
        SetError(Lang::Strings::SERVER_ERROR);
        CloseAudioChannel(false);
        return false;
    }
    if (new_turn) InvalidateEvents();
    if (!websocket_->Send(message)) {
        ESP_LOGE(TAG, "Failed to send text message (%u bytes)", static_cast<unsigned>(text.size()));
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return !closing_ && CloudAuthorizationReady() && receive_session_ && receive_session_->queue.Ready() && websocket_ &&
        !error_occurred_ && !receive_session_->queue.TimedOut();
}

bool WebsocketProtocol::SupportsConversationTurns() const {
    if (!IsAudioChannelOpened()) return false;
    const auto hello = receive_session_->queue.Hello();
    return hello && hello->turns;
}

bool WebsocketProtocol::SuspendIncomingTurn() {
    if (!SupportsConversationTurns() || !receive_session_->queue.SuspendTurn()) return false;
    InvalidateEvents();
    return true;
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;  // Websocket doesn't need to send goodbye message
#if CONFIG_AUDREAM_WATCH_UI
    if (!CloseAudioChannelWithin(0)) {
        ESP_LOGI(TAG, "Assistant transport shutdown is pending");
    }
#else
    InvalidateEvents();
    const bool had_session = receive_session_ != nullptr;
    if (receive_session_) receive_session_->queue.Retire();
    websocket_.reset();
    receive_session_.reset();
    if (had_session && on_audio_channel_closed_) on_audio_channel_closed_();
#endif
}

bool WebsocketProtocol::CloseAudioChannelWithin(uint32_t timeout_ms) {
    if (!websocket_ && !receive_session_) return true;
#if CONFIG_AUDREAM_WATCH_UI
    if (!closing_) {
        closing_ = true;
        InvalidateEvents();
        if (receive_session_) receive_session_->queue.Retire();
    }
    if (websocket_ && !websocket_->DisconnectWithin(timeout_ms)) return false;
    const bool had_session = receive_session_ != nullptr;
    websocket_.reset();
    receive_session_.reset();
    closing_ = false;
    if (had_session && on_audio_channel_closed_) on_audio_channel_closed_();
    return true;
#else
    (void)timeout_ms;
    return false;
#endif
}

bool WebsocketProtocol::OpenAudioChannel() {
#if CONFIG_AUDREAM_WATCH_UI
    if (!CloseAudioChannelWithin(0)) {
        ESP_LOGW(TAG, "Assistant connection rejected while the previous transport is stopping");
        return false;
    }
#else
    CloseAudioChannel(false);
#endif
    InvalidateEvents();
    server_sample_rate_ = 24000;
    server_frame_duration_ = 60;
    session_id_.clear();
    std::string url;
    std::string token;
    if (cloud_authorization_required_) {
        if (!CloudAuthorizationReady()) {
            cloud_authorization_.reset();
            ESP_LOGW(TAG, "Fresh device authorization is required before connecting");
            return false;
        }
        url = cloud_authorization_->websocket_url;
        token = cloud_authorization_->access_token;
        version_ = cloud_authorization_->protocol_version;
    } else {
        Settings settings("websocket", false);
        url = settings.GetString("url");
        if (url.empty()) url = ServerConfig::GetWebSocketUrl();
        token = settings.GetString("token");
        const int version = settings.GetInt("version");
        version_ = version == 0 ? 1 : version;
    }
    if (!assistant_wire::IsSupportedVersion(version_)) {
        ESP_LOGE(TAG, "Unsupported WebSocket protocol version: %d", version_);
        return false;
    }

    error_occurred_ = false;

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "Network interface is unavailable");
        return false;
    }
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }
    auto session = std::make_shared<ReceiveSession>(version_, on_receive_ready_);
    if (session->event_group == nullptr) {
        websocket_.reset();
        ESP_LOGE(TAG, "Failed to allocate receive event group");
        return false;
    }
    receive_session_ = session;

    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", cloud_authorization_required_ ? cloud_device_id_.c_str() : SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    websocket_->OnData([session](const char* data, size_t len, bool binary) {
        if (session->queue.Receive(data, len, binary)) session->Wake();
    });

    websocket_->OnDisconnected([session]() {
        if (session->queue.Disconnect()) session->Wake();
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_);
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server, code=%d", websocket_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        CloseAudioChannel(false);
        return false;
    }

    // Send hello message to describe the client
    auto message = GetHelloMessage();
    if (!SendText(message)) {
        CloseAudioChannel(false);
        return false;
    }

    // Wait for server hello
    EventBits_t bits = xEventGroupWaitBits(session->event_group, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT) || !IsAudioChannelOpened()) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        CloseAudioChannel(false);
        return false;
    }
    auto hello = session->queue.Hello();
    if (!hello) {
        CloseAudioChannel(false);
        return false;
    }
    server_sample_rate_ = hello->sample_rate;
    server_frame_duration_ = hello->frame_duration;
    session_id_ = std::move(hello->session_id);

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

std::string WebsocketProtocol::GetHelloMessage() {
    // keys: message type, version, audio_params (format, sample_rate, channels)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
#if CONFIG_AUDREAM_WATCH_UI
    cJSON_AddBoolToObject(features, "audream_turns", true);
#endif
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::Poll() {
    if (closing_) {
        (void)CloseAudioChannelWithin(0);
        return;
    }
    if (!CloudAuthorizationReady()) {
        cloud_authorization_.reset();
        CloseAudioChannel(false);
        return;
    }
    const auto session = receive_session_;
    if (!session) return;
    auto event = session->queue.Pop();
    if (!event) return;
    if (event->kind == AssistantReceiveQueue::Kind::Disconnected ||
        event->kind == AssistantReceiveQueue::Kind::Overflow ||
        event->kind == AssistantReceiveQueue::Kind::ProtocolError) {
        const bool overflow = event->kind == AssistantReceiveQueue::Kind::Overflow;
        const bool protocol_error = event->kind == AssistantReceiveQueue::Kind::ProtocolError;
        CloseAudioChannel(false);
        if (overflow) {
            ESP_LOGE(TAG, "Assistant receive queue overflow; ending the audio session");
            SetError(Lang::Strings::SERVER_ERROR);
        }
        if (protocol_error) {
            ESP_LOGE(TAG, "Assistant turn acknowledgement timed out; ending the audio session");
            SetError(Lang::Strings::SERVER_TIMEOUT);
        }
        return;
    }
    if (event->kind == AssistantReceiveQueue::Kind::Audio) {
        if (on_incoming_audio_) {
            on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                .sample_rate = server_sample_rate_,
                .frame_duration = server_frame_duration_,
                .timestamp = event->timestamp,
                .payload = std::vector<uint8_t>(event->payload.begin(), event->payload.end())
            }));
        }
    } else {
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
            assistant_wire::ParseJson(event->payload.data(), event->payload.size()), cJSON_Delete);
        if (root && on_incoming_json_) on_incoming_json_(root.get());
    }
    // Yield after one event so scheduled TTS state changes run before its audio.
    if (session == receive_session_ && session->queue.HasEvents() && on_receive_ready_) on_receive_ready_();
}
