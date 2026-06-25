#include "audio/local_recording_service.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/stat.h>

#include "audio/audio_service.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "storage/sd_card_storage.h"
#include "system/device_time_service.h"

namespace {

constexpr char kTag[] = "LocalRecording";
constexpr uint32_t kSampleRate = 16000;
constexpr uint8_t kChannels = 1;
constexpr uint32_t kFrameSamples = 960;

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

}  // namespace

LocalRecordingService& LocalRecordingService::GetInstance() {
    static LocalRecordingService instance;
    return instance;
}

bool LocalRecordingService::Start(AudioService& audio_service) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recording_) {
        ESP_LOGW(kTag, "Local recording start ignored because recording is already active");
        return false;
    }
    auto& storage = SdCardStorage::GetInstance();
    auto storage_status = storage.GetStatus();
    if (!storage_status.mounted) {
        ESP_LOGE(
            kTag,
            "Local recording start failed because storage is unavailable, error_code=%lu",
            static_cast<unsigned long>(storage_status.error_code)
        );
        return false;
    }

    current_ = {};
    current_.recording_id = BuildRecordingID();
    current_.file_name = current_.recording_id + ".opus";
    current_.file_path = storage.RecordingsDirectory() + "/" + current_.file_name;
    if (!writer_.Open(current_.file_path.c_str(), kSampleRate, kChannels, kFrameSamples)) {
        ESP_LOGE(kTag, "Failed to open local recording writer: %s", current_.file_path.c_str());
        current_ = {};
        return false;
    }

    started_at_us_ = esp_timer_get_time();
    current_.started_at_unix_ms = DeviceTimeService::GetInstance().CurrentUnixTimeMs();
    recording_ = true;
    audio_service.SetLocalRecordingPacketCallback(
        [this](const uint8_t* data, size_t size) {
            HandleOpusPacket(data, size);
        }
    );
    audio_service.EnableLocalRecording(true);
    ESP_LOGI(kTag, "Started local recording: %s", current_.file_path.c_str());
    return true;
}

LocalRecordingMetadata LocalRecordingService::Stop(AudioService& audio_service) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!recording_) {
        return last_;
    }

    audio_service.EnableLocalRecording(false);
    audio_service.SetLocalRecordingPacketCallback(nullptr);
    writer_.Close();

    current_.duration_ms = static_cast<uint32_t>(
        (esp_timer_get_time() - started_at_us_) / 1000
    );
    current_.size_bytes = FileSizeBytes(current_.file_path);
    current_.packet_count = writer_.PacketCount();
    current_.valid = current_.size_bytes > 0 && current_.packet_count > 0;
    WriteMetadataFile(current_);
    last_ = current_;
    recording_ = false;
    ESP_LOGI(
        kTag,
        "Stopped local recording: id=%s duration=%lu ms size=%lu bytes",
        last_.recording_id.c_str(),
        static_cast<unsigned long>(last_.duration_ms),
        static_cast<unsigned long>(last_.size_bytes)
    );
    current_ = {};
    return last_;
}

void LocalRecordingService::HandleOpusPacket(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!recording_) {
        return;
    }
    if (!writer_.WritePacket(data, size)) {
        ESP_LOGE(kTag, "Failed to write local recording packet");
    }
}

bool LocalRecordingService::IsRecording() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recording_;
}

LocalRecordingMetadata LocalRecordingService::CurrentMetadata() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

LocalRecordingMetadata LocalRecordingService::LastMetadata() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_;
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
    std::fprintf(
        file,
        "{"
        "\"recording_id\":\"%s\","
        "\"file_name\":\"%s\","
        "\"duration_ms\":%lu,"
        "\"size_bytes\":%lu,"
        "\"started_at_unix_ms\":%s,"
        "\"codec\":\"opus\","
        "\"container\":\"ogg\","
        "\"sample_rate\":16000,"
        "\"channels\":1,"
        "\"sync_state\":\"pending\""
        "}\n",
        metadata.recording_id.c_str(),
        metadata.file_name.c_str(),
        static_cast<unsigned long>(metadata.duration_ms),
        static_cast<unsigned long>(metadata.size_bytes),
        std::to_string(metadata.started_at_unix_ms).c_str()
    );
    fclose(file);
    return true;
}
