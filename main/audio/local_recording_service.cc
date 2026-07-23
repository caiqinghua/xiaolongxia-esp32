#include "audio/local_recording_service.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <sys/stat.h>

#include "audio/audio_service.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_rtc_time.h"
#include "esp_timer.h"
#include "storage/sd_card_storage.h"
#include "system/audream_power_manager.h"
#include "system/device_time_service.h"

namespace {

constexpr char kTag[] = "LocalRecording";
constexpr uint32_t kSampleRate = 16000;
constexpr uint8_t kChannels = 1;
constexpr uint32_t kFrameSamples = 960;
constexpr uint32_t kStopDrainTimeoutMs = 3000;

void XorBytes(uint8_t* target, size_t target_size, const uint8_t* source, size_t source_size) {
    for (size_t i = 0; i < source_size; ++i) {
        target[i % target_size] ^= source[i];
    }
}

std::string BuildRecordingID() {
    static uint32_t sequence = 0;

    uint8_t uuid[16] = {};
    esp_fill_random(uuid, sizeof(uuid));

    uint8_t mac[6] = {};
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        XorBytes(uuid, sizeof(uuid), mac, sizeof(mac));
    }

    const uint64_t uptime_us = static_cast<uint64_t>(esp_timer_get_time());
    const uint32_t current_sequence = ++sequence;
    XorBytes(uuid, sizeof(uuid), reinterpret_cast<const uint8_t*>(&uptime_us), sizeof(uptime_us));
    XorBytes(
        uuid,
        sizeof(uuid),
        reinterpret_cast<const uint8_t*>(&current_sequence),
        sizeof(current_sequence)
    );

    uuid[6] = static_cast<uint8_t>((uuid[6] & 0x0f) | 0x40);
    uuid[8] = static_cast<uint8_t>((uuid[8] & 0x3f) | 0x80);

    char buffer[48] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        static_cast<unsigned int>(uuid[0]),
        static_cast<unsigned int>(uuid[1]),
        static_cast<unsigned int>(uuid[2]),
        static_cast<unsigned int>(uuid[3]),
        static_cast<unsigned int>(uuid[4]),
        static_cast<unsigned int>(uuid[5]),
        static_cast<unsigned int>(uuid[6]),
        static_cast<unsigned int>(uuid[7]),
        static_cast<unsigned int>(uuid[8]),
        static_cast<unsigned int>(uuid[9]),
        static_cast<unsigned int>(uuid[10]),
        static_cast<unsigned int>(uuid[11]),
        static_cast<unsigned int>(uuid[12]),
        static_cast<unsigned int>(uuid[13]),
        static_cast<unsigned int>(uuid[14]),
        static_cast<unsigned int>(uuid[15])
    );
    return std::string(buffer);
}

uint32_t FileSizeBytes(const std::string& path) {
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0) {
        return 0;
    }
    return static_cast<uint32_t>(st.st_size);
}

std::string EscapeJsonString(const std::string& value) {
    std::string output;
    output.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '\\':
                output += "\\\\";
                break;
            case '"':
                output += "\\\"";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                output.push_back(ch);
                break;
        }
    }
    return output;
}

std::string BuildHighlightID(const std::string& recording_id, uint32_t timestamp_ms, size_t sequence) {
    char buffer[160] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%s-%lu-%u",
        recording_id.c_str(),
        static_cast<unsigned long>(timestamp_ms),
        static_cast<unsigned>(sequence)
    );
    return std::string(buffer);
}

}  // namespace

LocalRecordingService& LocalRecordingService::GetInstance() {
    static LocalRecordingService instance;
    return instance;
}

bool LocalRecordingService::Start(AudioService& audio_service) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::kClosed) {
            ESP_LOGW(kTag, "Local recording start ignored because the recorder is busy");
            return false;
        }
        state_ = State::kOpening;
    }

    bool started = false;
    do {
        auto& storage = SdCardStorage::GetInstance();
        auto storage_status = storage.GetStatus();
        if (!storage_status.mounted) {
            ESP_LOGE(
                kTag,
                "Local recording start failed because storage is unavailable, error_code=%lu",
                static_cast<unsigned long>(storage_status.error_code)
            );
            break;
        }

        LocalRecordingMetadata metadata = {};
        metadata.recording_id = BuildRecordingID();
        metadata.file_name = metadata.recording_id + ".opus";
        metadata.file_path = storage.RecordingsDirectory() + "/" + metadata.file_name;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_ = metadata;
            current_highlights_.clear();
            if (!writer_.Open(current_.file_path.c_str(), kSampleRate, kChannels, kFrameSamples)) {
                ESP_LOGE(kTag, "Failed to open local recording writer: %s", current_.file_path.c_str());
                current_ = {};
                break;
            }

            started_at_us_ = esp_timer_get_time();
            auto& time_service = DeviceTimeService::GetInstance();
            current_.rtc_epoch_seq = time_service.RtcEpochSeq();
            current_.started_at_uptime_ms = static_cast<uint64_t>(started_at_us_ / 1000);
            current_.started_at_rtc_ms = esp_rtc_get_time_us() / 1000;
            current_.started_at_unix_ms = time_service.CurrentUnixTimeMs();
            current_.time_synchronized = time_service.IsSynchronized();
            paused_at_us_ = 0;
            accumulated_paused_us_ = 0;
            state_ = State::kRecording;
        }

        audio_service.SetLocalRecordingPacketCallback(
            [this](const uint8_t* data, size_t size) {
                HandleOpusPacket(data, size);
            }
        );
        AudreamPowerManager::GetInstance().SetRecordingActive(true);
        audio_service.EnableLocalRecording(true);
        started = true;
    } while (false);

    if (!started) {
        std::lock_guard<std::mutex> lock(mutex_);
        writer_.Close();
        current_ = {};
        current_highlights_.clear();
        started_at_us_ = 0;
        paused_at_us_ = 0;
        accumulated_paused_us_ = 0;
        state_ = State::kClosed;
        return false;
    }

    const auto metadata = CurrentMetadata();
    ESP_LOGI(kTag, "Started local recording: %s", metadata.file_path.c_str());
    return true;
}

bool LocalRecordingService::Pause(AudioService& audio_service) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::kPaused) {
            return true;
        }
        if (state_ != State::kRecording) {
            ESP_LOGW(kTag, "Local recording pause ignored because recording is not active");
            return false;
        }
        state_ = State::kPausing;
    }

    AudreamPowerManager::GetInstance().SetRecordingCaptureActive(false);
    audio_service.EnableLocalRecording(false);
    if (!audio_service.WaitForLocalRecordingDrain(kStopDrainTimeoutMs)) {
        ESP_LOGW(kTag, "Local recording pause continued before encoder queue fully drained");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::kPausing) {
        return false;
    }
    paused_at_us_ = esp_timer_get_time();
    state_ = State::kPaused;
    ESP_LOGI(
        kTag,
        "Paused local recording: id=%s duration=%lu ms",
        current_.recording_id.c_str(),
        static_cast<unsigned long>(CurrentDurationMsLocked(paused_at_us_))
    );
    return true;
}

bool LocalRecordingService::Resume(AudioService& audio_service) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::kRecording) {
            return true;
        }
        if (state_ != State::kPaused || paused_at_us_ <= 0) {
            ESP_LOGW(kTag, "Local recording resume ignored because recording is not paused");
            return false;
        }
        accumulated_paused_us_ += std::max<int64_t>(0, esp_timer_get_time() - paused_at_us_);
        paused_at_us_ = 0;
        state_ = State::kResuming;
    }

    AudreamPowerManager::GetInstance().SetRecordingCaptureActive(true);
    audio_service.EnableLocalRecording(true);

    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::kResuming) {
        return false;
    }
    state_ = State::kRecording;
    ESP_LOGI(
        kTag,
        "Resumed local recording: id=%s duration=%lu ms",
        current_.recording_id.c_str(),
        static_cast<unsigned long>(CurrentDurationMsLocked(esp_timer_get_time()))
    );
    return true;
}

LocalRecordingMetadata LocalRecordingService::Stop(AudioService& audio_service) {
    bool capture_was_active = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::kRecording && state_ != State::kPaused) {
            return last_;
        }
        capture_was_active = state_ == State::kRecording;
        if (state_ == State::kPaused && paused_at_us_ > 0) {
            accumulated_paused_us_ += std::max<int64_t>(
                0,
                esp_timer_get_time() - paused_at_us_
            );
            paused_at_us_ = 0;
        }
        state_ = State::kStopping;
    }

    if (capture_was_active) {
        AudreamPowerManager::GetInstance().SetRecordingCaptureActive(false);
        audio_service.EnableLocalRecording(false);
        if (!audio_service.WaitForLocalRecordingDrain(kStopDrainTimeoutMs)) {
            ESP_LOGW(kTag, "Local recording stop continued before encoder queue fully drained");
        }
    }
    audio_service.SetLocalRecordingPacketCallback(nullptr);

    std::lock_guard<std::mutex> lock(mutex_);
    writer_.Close();

    current_.duration_ms = CurrentDurationMsLocked(esp_timer_get_time());
    current_.size_bytes = FileSizeBytes(current_.file_path);
    current_.packet_count = writer_.PacketCount();
    current_.valid = current_.size_bytes > 0 && current_.packet_count > 0;
    auto& time_service = DeviceTimeService::GetInstance();
    if (!current_.time_synchronized && time_service.IsSynchronized()) {
        const uint64_t stopped_at_rtc_ms = esp_rtc_get_time_us() / 1000;
        const uint64_t stopped_at_unix_ms = time_service.CurrentUnixTimeMs();
        if (current_.started_at_rtc_ms > 0 &&
            stopped_at_rtc_ms >= current_.started_at_rtc_ms &&
            stopped_at_unix_ms >= stopped_at_rtc_ms - current_.started_at_rtc_ms) {
            current_.started_at_unix_ms =
                stopped_at_unix_ms - (stopped_at_rtc_ms - current_.started_at_rtc_ms);
            current_.time_synchronized = true;
        }
    }
    WriteMetadataFile(current_);
    SdCardStorage::GetInstance().SetRecordingMtime(
        current_.recording_id,
        current_.started_at_unix_ms
    );
    last_ = current_;
    last_highlights_ = current_highlights_;
    state_ = State::kClosed;
    AudreamPowerManager::GetInstance().SetRecordingActive(false);
    AudreamPowerManager::GetInstance().NotifyUserActivity("recording_stopped", 120);
    ESP_LOGI(
        kTag,
        "Stopped local recording: id=%s duration=%lu ms size=%lu bytes",
        last_.recording_id.c_str(),
        static_cast<unsigned long>(last_.duration_ms),
        static_cast<unsigned long>(last_.size_bytes)
    );
    current_ = {};
    current_highlights_.clear();
    started_at_us_ = 0;
    paused_at_us_ = 0;
    accumulated_paused_us_ = 0;
    return last_;
}

bool LocalRecordingService::AddMarkerHighlight(const std::string& source) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::kRecording || started_at_us_ <= 0 || current_.recording_id.empty()) {
        return false;
    }
    const uint32_t timestamp_ms = CurrentDurationMsLocked(esp_timer_get_time());
    LocalRecordingHighlight highlight;
    highlight.timestamp_ms = timestamp_ms;
    highlight.kind = "marker";
    highlight.source = source;
    highlight.id = BuildHighlightID(current_.recording_id, timestamp_ms, current_highlights_.size() + 1);
    current_highlights_.push_back(highlight);
    ESP_LOGI(
        kTag,
        "Added local recording highlight: id=%s timestamp=%lu ms",
        highlight.id.c_str(),
        static_cast<unsigned long>(timestamp_ms)
    );
    return true;
}

void LocalRecordingService::HandleOpusPacket(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (
        state_ != State::kRecording &&
        state_ != State::kPausing &&
        state_ != State::kResuming &&
        state_ != State::kStopping
    ) {
        return;
    }
    if (!writer_.WritePacket(data, size)) {
        ESP_LOGE(kTag, "Failed to write local recording packet");
    }
}

bool LocalRecordingService::IsRecording() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kOpening ||
        state_ == State::kRecording ||
        state_ == State::kPausing ||
        state_ == State::kPaused ||
        state_ == State::kResuming ||
        state_ == State::kStopping;
}

bool LocalRecordingService::IsPaused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kPaused;
}

uint32_t LocalRecordingService::CurrentDurationMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return CurrentDurationMsLocked(esp_timer_get_time());
}

LocalRecordingMetadata LocalRecordingService::CurrentMetadata() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

LocalRecordingMetadata LocalRecordingService::LastMetadata() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_;
}

uint32_t LocalRecordingService::CurrentDurationMsLocked(int64_t now_us) const {
    if (started_at_us_ <= 0 || now_us <= started_at_us_) {
        return 0;
    }
    int64_t paused_us = accumulated_paused_us_;
    if (paused_at_us_ > 0 && now_us > paused_at_us_) {
        paused_us += now_us - paused_at_us_;
    }
    const int64_t active_us = std::max<int64_t>(
        0,
        now_us - started_at_us_ - paused_us
    );
    return static_cast<uint32_t>(active_us / 1000);
}

bool LocalRecordingService::WriteMetadataFile(const LocalRecordingMetadata& metadata) const {
    if (metadata.file_path.empty()) {
        return false;
    }
    const std::string metadata_path = metadata.file_path + ".json";
    FILE* file = fopen(metadata_path.c_str(), "w");
    if (file == nullptr) {
        ESP_LOGE(kTag, "Failed to open recording metadata file: %s", metadata_path.c_str());
        return false;
    }
    std::vector<LocalRecordingHighlight> highlights;
    if (metadata.recording_id == current_.recording_id) {
        highlights = current_highlights_;
    } else if (metadata.recording_id == last_.recording_id) {
        highlights = last_highlights_;
    }

    std::ostringstream stream;
    stream
        << "{"
        << "\"schema_version\":1,"
        << "\"recording_id\":\"" << EscapeJsonString(metadata.recording_id) << "\","
        << "\"file_name\":\"" << EscapeJsonString(metadata.file_name) << "\","
        << "\"rtc_epoch_seq\":" << metadata.rtc_epoch_seq << ","
        << "\"duration_ms\":" << metadata.duration_ms << ","
        << "\"size_bytes\":" << metadata.size_bytes << ","
        << "\"started_at_unix_ms\":" << metadata.started_at_unix_ms << ","
        << "\"started_at_uptime_ms\":" << metadata.started_at_uptime_ms << ","
        << "\"started_at_rtc_ms\":" << metadata.started_at_rtc_ms << ","
        << "\"time_synchronized\":" << (metadata.time_synchronized ? "true" : "false") << ","
        << "\"codec\":\"opus\","
        << "\"container\":\"ogg\","
        << "\"sample_rate\":16000,"
        << "\"channels\":1,"
        << "\"sync_state\":\"pending\","
        << "\"highlights\":[";
    for (size_t index = 0; index < highlights.size(); ++index) {
        const auto& highlight = highlights[index];
        if (index > 0) {
            stream << ",";
        }
        stream
            << "{"
            << "\"id\":\"" << EscapeJsonString(highlight.id) << "\","
            << "\"kind\":\"" << EscapeJsonString(highlight.kind) << "\","
            << "\"source\":\"" << EscapeJsonString(highlight.source) << "\","
            << "\"timestamp_ms\":" << highlight.timestamp_ms
            << "}";
    }
    stream << "]}\n";

    const std::string content = stream.str();
    std::fwrite(content.data(), 1, content.size(), file);
    fclose(file);
    return true;
}
