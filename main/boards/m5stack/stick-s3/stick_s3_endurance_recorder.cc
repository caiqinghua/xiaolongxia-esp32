#include "stick_s3_endurance_recorder.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <utility>

#include "audio/audio_service.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_rtc_time.h"
#include "esp_timer.h"
#include "storage/sd_card_storage.h"
#include "system/audream_power_manager.h"
#include "system/device_time_service.h"

namespace {

constexpr char kTag[] = "StickS3Recorder";
constexpr uint32_t kSampleRate = 16000;
constexpr uint8_t kChannels = 1;
constexpr uint32_t kFrameSamples = 960;
constexpr uint64_t kSegmentSizeBytes = 512 * 1024;
constexpr uint64_t kAllocationReserveBytes = 64 * 1024;
constexpr uint32_t kDrainTimeoutMs = 3000;
constexpr char kLastSessionFileName[] = "last-session.json";

uint32_t FileSizeBytes(const std::string& path) {
    struct stat file_stat = {};
    if (stat(path.c_str(), &file_stat) != 0) {
        return 0;
    }
    return static_cast<uint32_t>(file_stat.st_size);
}

bool FileExists(const std::string& path) {
    struct stat file_stat = {};
    return stat(path.c_str(), &file_stat) == 0 && S_ISREG(file_stat.st_mode);
}

std::string ReadTextFile(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return {};
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return {};
    }
    const long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return {};
    }
    std::string content(static_cast<size_t>(length), '\0');
    const size_t read = fread(content.data(), 1, content.size(), file);
    fclose(file);
    if (read != content.size()) {
        return {};
    }
    return content;
}

bool WriteTextAtomically(const std::string& path, const std::string& content) {
    const std::string temporary_path = path + ".partial";
    FILE* file = fopen(temporary_path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const bool written = fwrite(content.data(), 1, content.size(), file) == content.size();
    const bool flushed = written && fflush(file) == 0;
    const bool synced = flushed && fsync(fileno(file)) == 0;
    const bool closed = fclose(file) == 0;
    if (!written || !flushed || !synced || !closed) {
        unlink(temporary_path.c_str());
        return false;
    }
    if (rename(temporary_path.c_str(), path.c_str()) == 0) {
        return true;
    }

    const int rename_error = errno;
    if (!FileExists(path)) {
        ESP_LOGE(
            kTag,
            "Failed to install metadata file: path=%s error=%s",
            path.c_str(),
            std::strerror(rename_error)
        );
        unlink(temporary_path.c_str());
        return false;
    }
    if (unlink(path.c_str()) != 0) {
        ESP_LOGE(
            kTag,
            "Failed to replace existing metadata file: path=%s error=%s",
            path.c_str(),
            std::strerror(errno)
        );
        unlink(temporary_path.c_str());
        return false;
    }
    if (rename(temporary_path.c_str(), path.c_str()) != 0) {
        ESP_LOGE(
            kTag,
            "Failed to install replacement metadata file: path=%s error=%s",
            path.c_str(),
            std::strerror(errno)
        );
        unlink(temporary_path.c_str());
        return false;
    }
    return true;
}

std::string BuildSessionID() {
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    const uint32_t random = esp_random();
    const uint64_t uptime = static_cast<uint64_t>(esp_timer_get_time());
    char output[64] = {};
    std::snprintf(
        output,
        sizeof(output),
        "%02x%02x%02x%02x%02x%02x-%08lx-%08lx",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        static_cast<unsigned long>(uptime & 0xffffffffULL),
        static_cast<unsigned long>(random)
    );
    return std::string(output);
}

std::string BuildSegmentID(const std::string& session_id, uint32_t index) {
    char output[96] = {};
    std::snprintf(
        output,
        sizeof(output),
        "%s-s%04lu",
        session_id.c_str(),
        static_cast<unsigned long>(index)
    );
    return std::string(output);
}

}  // namespace

StickS3EnduranceRecorder& StickS3EnduranceRecorder::GetInstance() {
    static StickS3EnduranceRecorder instance;
    return instance;
}

bool StickS3EnduranceRecorder::Start(AudioService& audio_service) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recording_) {
        return true;
    }
    if (!SdCardStorage::GetInstance().GetStatus().mounted) {
        ESP_LOGE(kTag, "Cannot start endurance recording because storage is unavailable");
        return false;
    }

    session_id_ = BuildSessionID();
    next_segment_index_ = 0;
    overwritten_segment_count_ = 0;
    total_encoded_duration_ms_ = 0;
    segments_.clear();
    started_at_us_ = esp_timer_get_time();
    started_at_uptime_ms_ = static_cast<uint64_t>(started_at_us_ / 1000);
    started_at_rtc_ms_ = esp_rtc_get_time_us() / 1000;
    auto& time_service = DeviceTimeService::GetInstance();
    started_at_unix_ms_ = time_service.CurrentUnixTimeMs();
    rtc_epoch_seq_ = time_service.RtcEpochSeq();
    time_synchronized_ = time_service.IsSynchronized();
    write_failed_ = false;
    stopping_ = false;

    if (!OpenNextSegmentLocked(0)) {
        session_id_.clear();
        return false;
    }

    audio_service.SetLocalRecordingPacketCallback(
        [this](const uint8_t* data, size_t size) {
            HandleOpusPacket(data, size);
        }
    );
    recording_ = true;
    AudreamPowerManager::GetInstance().SetRecordingActive(true);
    audio_service.EnableLocalRecording(true);
    ESP_LOGI(
        kTag,
        "Endurance recording started: session=%s segment_bytes=%llu sample_rate=%lu",
        session_id_.c_str(),
        static_cast<unsigned long long>(kSegmentSizeBytes),
        static_cast<unsigned long>(kSampleRate)
    );
    return true;
}

bool StickS3EnduranceRecorder::Stop(AudioService& audio_service, const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!recording_ || stopping_) {
            return false;
        }
        stopping_ = true;
    }

    AudreamPowerManager::GetInstance().SetRecordingCaptureActive(false);
    audio_service.EnableLocalRecording(false);
    if (!audio_service.WaitForLocalRecordingDrain(kDrainTimeoutMs)) {
        ESP_LOGW(kTag, "Endurance recording stopped before the encoder queue fully drained");
    }
    audio_service.SetLocalRecordingPacketCallback(nullptr);

    std::lock_guard<std::mutex> lock(mutex_);
    recording_ = false;
    stopping_ = false;
    const uint64_t elapsed_ms = DurationMsLocked();
    FinalizeCurrentSegmentLocked();
    const bool metadata_written = WriteSessionMetadataLocked(elapsed_ms, reason);
    AudreamPowerManager::GetInstance().SetRecordingActive(false);
    ESP_LOGI(
        kTag,
        "Endurance recording stopped: session=%s elapsed_ms=%llu encoded_ms=%llu segments=%u overwritten=%u reason=%s metadata=%d",
        session_id_.c_str(),
        static_cast<unsigned long long>(elapsed_ms),
        static_cast<unsigned long long>(total_encoded_duration_ms_),
        static_cast<unsigned>(segments_.size()),
        static_cast<unsigned>(overwritten_segment_count_),
        reason.c_str(),
        metadata_written ? 1 : 0
    );
    return metadata_written;
}

bool StickS3EnduranceRecorder::IsRecording() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recording_;
}

uint64_t StickS3EnduranceRecorder::DurationMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return DurationMsLocked();
}

void StickS3EnduranceRecorder::LogLastSessionDuration() const {
    const std::string path = SdCardStorage::GetInstance().RecordingsDirectory() +
        "/" + kLastSessionFileName;
    const std::string content = ReadTextFile(path);
    if (content.empty()) {
        ESP_LOGI(kTag, "No previous endurance recording metadata was found");
        return;
    }

    cJSON* root = cJSON_ParseWithLength(content.data(), content.size());
    if (root == nullptr) {
        ESP_LOGW(kTag, "Previous endurance recording metadata is invalid");
        return;
    }
    const cJSON* elapsed = cJSON_GetObjectItemCaseSensitive(root, "elapsed_duration_ms");
    const cJSON* session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    const cJSON* reason = cJSON_GetObjectItemCaseSensitive(root, "stop_reason");
    if (!cJSON_IsNumber(elapsed) || elapsed->valuedouble < 0) {
        ESP_LOGW(kTag, "Previous endurance recording duration is missing");
        cJSON_Delete(root);
        return;
    }

    const uint64_t elapsed_ms = static_cast<uint64_t>(elapsed->valuedouble);
    const uint64_t total_seconds = elapsed_ms / 1000;
    const uint64_t hours = total_seconds / 3600;
    const uint64_t minutes = (total_seconds / 60) % 60;
    const uint64_t seconds = total_seconds % 60;
    ESP_LOGI(
        kTag,
        "Previous endurance recording: session=%s duration=%02llu:%02llu:%02llu elapsed_ms=%llu reason=%s",
        cJSON_IsString(session) ? session->valuestring : "unknown",
        static_cast<unsigned long long>(hours),
        static_cast<unsigned long long>(minutes),
        static_cast<unsigned long long>(seconds),
        static_cast<unsigned long long>(elapsed_ms),
        cJSON_IsString(reason) ? reason->valuestring : "unknown"
    );
    cJSON_Delete(root);
}

void StickS3EnduranceRecorder::HandleOpusPacket(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!recording_ || write_failed_) {
        return;
    }
    if (!writer_.WritePacket(data, size)) {
        write_failed_ = true;
        ESP_LOGE(kTag, "Failed to write endurance recording packet");
        return;
    }
    if (writer_.BytesWritten() < kSegmentSizeBytes) {
        return;
    }

    FinalizeCurrentSegmentLocked();
    if (!OpenNextSegmentLocked(DurationMsLocked())) {
        write_failed_ = true;
        ESP_LOGE(kTag, "Failed to open the next circular recording segment");
    }
}

bool StickS3EnduranceRecorder::OpenNextSegmentLocked(uint64_t elapsed_ms) {
    std::vector<std::string> deleted_recording_ids;
    const bool reclaimed = SdCardStorage::GetInstance().ReclaimSpaceForRecording(
        kSegmentSizeBytes + kAllocationReserveBytes,
        current_segment_.recording_id,
        &deleted_recording_ids
    );
    if (!reclaimed) {
        ESP_LOGE(kTag, "Circular recording could not reserve enough Flash space");
        return false;
    }
    for (const auto& deleted_id : deleted_recording_ids) {
        const bool belongs_to_session = std::any_of(
            segments_.begin(),
            segments_.end(),
            [&deleted_id](const Segment& segment) {
                return segment.recording_id == deleted_id;
            }
        );
        if (belongs_to_session) {
            overwritten_segment_count_++;
        }
    }

    Segment segment;
    segment.index = next_segment_index_++;
    segment.recording_id = BuildSegmentID(session_id_, segment.index);
    segment.file_name = segment.recording_id + ".opus";
    segment.file_path = SdCardStorage::GetInstance().RecordingsDirectory() + "/" + segment.file_name;
    segment.started_at_uptime_ms = started_at_uptime_ms_ + elapsed_ms;
    segment.started_at_rtc_ms = started_at_rtc_ms_ + elapsed_ms;
    segment.started_at_unix_ms = started_at_unix_ms_ == 0 ? 0 : started_at_unix_ms_ + elapsed_ms;
    segment.time_synchronized = time_synchronized_;
    if (!writer_.Open(segment.file_path.c_str(), kSampleRate, kChannels, kFrameSamples)) {
        ESP_LOGE(kTag, "Failed to open endurance recording segment: %s", segment.file_path.c_str());
        return false;
    }
    current_segment_ = std::move(segment);
    ESP_LOGI(
        kTag,
        "Circular recording segment opened: id=%s index=%lu",
        current_segment_.recording_id.c_str(),
        static_cast<unsigned long>(current_segment_.index)
    );
    return true;
}

StickS3EnduranceRecorder::Segment StickS3EnduranceRecorder::FinalizeCurrentSegmentLocked() {
    current_segment_.packet_count = writer_.PacketCount();
    current_segment_.duration_ms = static_cast<uint32_t>(
        writer_.GranulePosition() * 1000ULL / kSampleRate
    );
    writer_.Close();
    current_segment_.size_bytes = FileSizeBytes(current_segment_.file_path);
    if (current_segment_.size_bytes > 0 && current_segment_.packet_count > 0) {
        WriteSegmentMetadataLocked(current_segment_);
        SdCardStorage::GetInstance().SetRecordingMtime(
            current_segment_.recording_id,
            current_segment_.started_at_unix_ms
        );
        total_encoded_duration_ms_ += current_segment_.duration_ms;
        segments_.push_back(current_segment_);
        ESP_LOGI(
            kTag,
            "Circular recording segment finalized: id=%s duration_ms=%lu size_bytes=%lu packets=%lu",
            current_segment_.recording_id.c_str(),
            static_cast<unsigned long>(current_segment_.duration_ms),
            static_cast<unsigned long>(current_segment_.size_bytes),
            static_cast<unsigned long>(current_segment_.packet_count)
        );
    } else if (!current_segment_.file_path.empty()) {
        unlink(current_segment_.file_path.c_str());
    }
    Segment finalized = current_segment_;
    current_segment_ = {};
    return finalized;
}

bool StickS3EnduranceRecorder::WriteSegmentMetadataLocked(const Segment& segment) const {
    std::ostringstream output;
    output
        << "{"
        << "\"schema_version\":2,"
        << "\"recording_id\":\"" << segment.recording_id << "\","
        << "\"file_name\":\"" << segment.file_name << "\","
        << "\"session_id\":\"" << session_id_ << "\","
        << "\"segment_index\":" << segment.index << ","
        << "\"rtc_epoch_seq\":" << rtc_epoch_seq_ << ","
        << "\"duration_ms\":" << segment.duration_ms << ","
        << "\"size_bytes\":" << segment.size_bytes << ","
        << "\"started_at_unix_ms\":" << segment.started_at_unix_ms << ","
        << "\"started_at_uptime_ms\":" << segment.started_at_uptime_ms << ","
        << "\"started_at_rtc_ms\":" << segment.started_at_rtc_ms << ","
        << "\"time_synchronized\":" << (segment.time_synchronized ? "true" : "false") << ","
        << "\"codec\":\"opus\","
        << "\"container\":\"ogg\","
        << "\"sample_rate\":16000,"
        << "\"channels\":1,"
        << "\"sync_state\":\"pending\","
        << "\"highlights\":[]"
        << "}\n";
    const std::string metadata_path = segment.file_path + ".json";
    if (!WriteTextAtomically(metadata_path, output.str())) {
        ESP_LOGE(kTag, "Failed to write segment metadata: %s", metadata_path.c_str());
        return false;
    }
    return true;
}

bool StickS3EnduranceRecorder::WriteSessionMetadataLocked(
    uint64_t elapsed_ms,
    const std::string& reason
) const {
    uint64_t retained_duration_ms = 0;
    uint64_t retained_bytes = 0;
    uint32_t retained_segment_count = 0;
    for (const auto& segment : segments_) {
        if (!FileExists(segment.file_path)) {
            continue;
        }
        retained_duration_ms += segment.duration_ms;
        retained_bytes += segment.size_bytes;
        retained_segment_count++;
    }

    std::ostringstream output;
    output
        << "{"
        << "\"schema_version\":1,"
        << "\"session_id\":\"" << session_id_ << "\","
        << "\"stop_reason\":\"" << reason << "\","
        << "\"completed\":true,"
        << "\"audio_complete\":" << (write_failed_ ? "false" : "true") << ","
        << "\"elapsed_duration_ms\":" << elapsed_ms << ","
        << "\"encoded_audio_duration_ms\":" << total_encoded_duration_ms_ << ","
        << "\"retained_audio_duration_ms\":" << retained_duration_ms << ","
        << "\"retained_audio_bytes\":" << retained_bytes << ","
        << "\"segment_count\":" << segments_.size() << ","
        << "\"retained_segment_count\":" << retained_segment_count << ","
        << "\"overwritten_segment_count\":" << overwritten_segment_count_ << ","
        << "\"started_at_unix_ms\":" << started_at_unix_ms_ << ","
        << "\"started_at_uptime_ms\":" << started_at_uptime_ms_ << ","
        << "\"started_at_rtc_ms\":" << started_at_rtc_ms_ << ","
        << "\"time_synchronized\":" << (time_synchronized_ ? "true" : "false") << ","
        << "\"sample_rate\":16000,"
        << "\"channels\":1,"
        << "\"segment_size_bytes\":" << kSegmentSizeBytes
        << "}\n";
    const std::string directory = SdCardStorage::GetInstance().RecordingsDirectory();
    const std::string path = directory +
        "/" + session_id_ + ".session.json";
    const std::string content = output.str();
    if (!WriteTextAtomically(path, content)) {
        ESP_LOGE(kTag, "Failed to write endurance session metadata: %s", path.c_str());
        return false;
    }
    const std::string last_session_path = directory + "/" + kLastSessionFileName;
    if (!WriteTextAtomically(last_session_path, content)) {
        ESP_LOGE(kTag, "Failed to update the latest session metadata: %s", last_session_path.c_str());
        return false;
    }
    return true;
}

uint64_t StickS3EnduranceRecorder::DurationMsLocked() const {
    if (started_at_us_ <= 0) {
        return 0;
    }
    return static_cast<uint64_t>(
        std::max<int64_t>(0, esp_timer_get_time() - started_at_us_) / 1000
    );
}
