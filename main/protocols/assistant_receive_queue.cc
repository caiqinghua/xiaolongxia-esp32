#include "assistant_receive_queue.h"

#include <cstring>
#include <memory>
#include <climits>

namespace {
bool Equals(const cJSON* item, const char* value) {
    return cJSON_IsString(item) && std::strcmp(item->valuestring, value) == 0;
}

bool ReadTurn(const cJSON* root, int& value) {
    const auto item = cJSON_GetObjectItemCaseSensitive(root, "turn_id");
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 1 && item->valuedouble <= INT_MAX) ||
        item->valuedouble != item->valueint) return false;
    value = item->valueint;
    return true;
}
}

bool AssistantReceiveQueue::PrepareControl(std::string& text, bool& new_turn) {
    std::lock_guard<std::mutex> lock(mutex_);
    new_turn = false;
    if (retired_ || !connected_ || TurnTimedOut()) return false;
    if (!hello_ || !hello_->turns) return true;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        assistant_wire::ParseJson(text.data(), text.size()), cJSON_Delete);
    if (!root) return false;
    const bool start = Equals(cJSON_GetObjectItemCaseSensitive(root.get(), "type"), "listen") &&
        Equals(cJSON_GetObjectItemCaseSensitive(root.get(), "state"), "start");
    if (turn_suspended_ && !start) return false;
    if ((start && turn_id_ == INT_MAX) || (!start && turn_id_ == 0)) return false;
    const int next = turn_id_ + (start ? 1 : 0);
    cJSON_DeleteItemFromObjectCaseSensitive(root.get(), "turn_id");
    cJSON_AddNumberToObject(root.get(), "turn_id", next);
    cJSON_DeleteItemFromObjectCaseSensitive(root.get(), "session_id");
    cJSON_AddStringToObject(root.get(), "session_id", hello_->session_id.c_str());
    std::unique_ptr<char, decltype(&cJSON_free)> serialized(cJSON_PrintUnformatted(root.get()), cJSON_free);
    if (!serialized || std::strlen(serialized.get()) > assistant_wire::kMaxJsonBytes) return false;
    text = serialized.get();
    if (start) {
        turn_id_ = next;
        turn_suspended_ = false;
        turn_accepted_ = false;
        turn_deadline_ = clock_() + std::chrono::seconds(10);
        events_.clear();
        bytes_ = 0;
        audio_ms_ = 0;
        new_turn = true;
    }
    return true;
}

bool AssistantReceiveQueue::CanSendAudio() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !retired_ && connected_ && hello_ && !turn_suspended_ && !TurnTimedOut() &&
        (!hello_->turns || turn_id_ > 0);
}

bool AssistantReceiveQueue::SuspendTurn() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (retired_ || !connected_ || !hello_ || !hello_->turns) return false;
    turn_suspended_ = true;
    events_.clear();
    bytes_ = 0;
    audio_ms_ = 0;
    return true;
}

bool AssistantReceiveQueue::TurnTimedOut() const {
    return hello_ && hello_->turns && !turn_suspended_ && turn_id_ > 0 && !turn_accepted_ && clock_() >= turn_deadline_;
}

bool AssistantReceiveQueue::Receive(const char* data, size_t size, bool binary) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (retired_ || !connected_) return false;
    if (turn_suspended_) return false;
    if (TurnTimedOut()) {
        Terminate(Kind::ProtocolError);
        return true;
    }
    Event event;
    if (binary) {
        assistant_wire::AudioView audio;
        if (!hello_ || !assistant_wire::DecodeAudio(version_, data, size, audio)) return false;
        if (hello_->turns && !turn_accepted_) return false;
        event.kind = Kind::Audio;
        event.timestamp = audio.timestamp;
        event.duration_ms = hello_->frame_duration;
        event.payload.assign(reinterpret_cast<const char*>(audio.data), audio.size);
    } else {
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(assistant_wire::ParseJson(data, size), cJSON_Delete);
        if (!root) return false;
        const auto type = cJSON_GetObjectItemCaseSensitive(root.get(), "type");
        if (std::strcmp(type->valuestring, "hello") == 0) {
            assistant_wire::ServerHello hello;
            if (hello_ || !assistant_wire::ParseHello(root.get(), version_, hello)) return false;
            if (hello.turns && !allow_turns_) return false;
            hello_ = std::move(hello);
            last_received_ = clock_();
            return true;
        }
        if (!hello_) return false;
        if (hello_->turns) {
            int received_turn = 0;
            if (!ReadTurn(root.get(), received_turn) || received_turn != turn_id_ ||
                !Equals(cJSON_GetObjectItemCaseSensitive(root.get(), "session_id"), hello_->session_id.c_str())) return false;
            if (Equals(type, "turn")) {
                if (!Equals(cJSON_GetObjectItemCaseSensitive(root.get(), "state"), "accepted") || turn_accepted_) return false;
                turn_accepted_ = true;
                last_received_ = clock_();
                return true;
            }
            if (!turn_accepted_) return false;
        }
        event.kind = Kind::Json;
        event.payload.assign(data, size);
    }
    if (events_.size() >= kMaxEvents || event.payload.size() > kMaxBytes - bytes_ ||
        event.duration_ms > kMaxAudioMs - audio_ms_) {
        Terminate(Kind::Overflow);
        return true;
    }
    bytes_ += event.payload.size();
    audio_ms_ += event.duration_ms;
    events_.push_back(std::move(event));
    last_received_ = clock_();
    return true;
}

void AssistantReceiveQueue::Terminate(Kind kind) {
    connected_ = false;
    events_.clear();
    bytes_ = 0;
    audio_ms_ = 0;
    terminal_ = kind;
}

bool AssistantReceiveQueue::Disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (retired_ || !connected_) return false;
    Terminate(Kind::Disconnected);
    return true;
}

void AssistantReceiveQueue::Retire() {
    std::lock_guard<std::mutex> lock(mutex_);
    retired_ = true;
    Terminate(Kind::Disconnected);
    terminal_.reset();
}

bool AssistantReceiveQueue::Ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !retired_ && connected_ && hello_.has_value();
}

bool AssistantReceiveQueue::TimedOut() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return TurnTimedOut() || clock_() - last_received_ > std::chrono::seconds(120);
}

std::optional<assistant_wire::ServerHello> AssistantReceiveQueue::Hello() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !retired_ && connected_ ? hello_ : std::nullopt;
}

std::optional<AssistantReceiveQueue::Event> AssistantReceiveQueue::Pop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (retired_) return std::nullopt;
    if (connected_ && TurnTimedOut()) Terminate(Kind::ProtocolError);
    if (terminal_) {
        Event event;
        event.kind = *terminal_;
        terminal_.reset();
        return event;
    }
    if (events_.empty()) return std::nullopt;
    Event event = std::move(events_.front());
    events_.pop_front();
    bytes_ -= event.payload.size();
    audio_ms_ -= event.duration_ms;
    return event;
}

bool AssistantReceiveQueue::HasEvents() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !retired_ && (terminal_.has_value() || !events_.empty());
}
