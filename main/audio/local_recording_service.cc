#include "audio/local_recording_service.h"

#include <cstdio>
#include <sys/stat.h>

#include "audio/audio_service.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "storage/sd_card_storage.h"

namespace {

constexpr char kTag[] = "LocalRecording";
constexpr uint32_t kSampleRate = 16000;
constexpr uint8_t kChannels = 1;
constexpr uint32_t kFrameSamples = 960;

std::string BuildRecordingID() {
    char buffer[48] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "rec_%lld_%08lx",
        static_cast<long long>(esp_timer_get_time()),
        static_cast<unsigned long>(esp_random())
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
        return false;
    }
    auto& storage = SdCardStorage::GetInstance();
    if (!storage.Mount() || !storage.EnsureRecordingsDirectory()) {
        return false;
    }

    current_ = {};
    current_.recording_id = BuildRecordingID();
    current_.file_name = current_.recording_id + ".opus";
    current_.file_path = storage.RecordingsDirectory() + "/" + current_.file_name;
    if (!writer_.Open(current_.file_path.c_str(), kSampleRate, kChannels, kFrameSamples)) {
        current_ = {};
        return false;
    }

    started_at_us_ = esp_timer_get_time();
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
        "\"codec\":\"opus\","
        "\"container\":\"ogg\","
        "\"sample_rate\":16000,"
        "\"channels\":1,"
        "\"sync_state\":\"pending\""
        "}\n",
        metadata.recording_id.c_str(),
        metadata.file_name.c_str(),
        static_cast<unsigned long>(metadata.duration_ms),
        static_cast<unsigned long>(metadata.size_bytes)
    );
    fclose(file);
    return true;
}
