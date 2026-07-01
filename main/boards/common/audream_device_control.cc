#include "audream_device_control.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "audio/audio_codec.h"
#include "audio/local_recording_service.h"
#include "application.h"
#include "board.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "settings.h"
#include "storage/sd_card_storage.h"
#include "storage/temporary_ap_transfer_service.h"
#include "system/audream_power_manager.h"
#include "system/device_time_service.h"

namespace {

constexpr char kTag[] = "AudreamControl";
constexpr char kSettingsNamespace[] = "audream_device";
constexpr char kAutoPowerOffKey[] = "auto_off";
constexpr char kMicrophoneGainKey[] = "mic_gain";
constexpr char kCallGainKey[] = "call_gain";
constexpr char kRecordingModeKey[] = "recording_mode";

constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kRequestHeaderLength = 4;
constexpr uint8_t kResponseOpcodeMask = 0x80;
constexpr size_t kMaxReadChunkBytes = 96;
constexpr size_t kMaxListRecordingsPayloadBytes = 220;

constexpr int32_t kDefaultAutoPowerOffSeconds = -1;
constexpr uint8_t kDefaultMicrophoneGainDb = 24;
constexpr uint8_t kDefaultCallGain = 1;
constexpr uint8_t kDefaultBatteryLevelPercent = 80;
constexpr uint8_t kDefaultRecordingMode = 0;

constexpr uint16_t kCapabilityAutoPowerOff = 1 << 0;
constexpr uint16_t kCapabilityMicrophoneGain = 1 << 1;
constexpr uint16_t kCapabilityCallGain = 1 << 2;
constexpr uint16_t kCapabilityPowerOff = 1 << 3;
constexpr uint16_t kCapabilityResetSettings = 1 << 4;
constexpr uint16_t kCapabilityEraseAndRemove = 1 << 5;
constexpr uint16_t kCapabilityBatteryLevel = 1 << 6;
constexpr uint16_t kCapabilityRecordingMode = 1 << 7;
constexpr uint16_t kCapabilityFirmwareVersion = 1 << 8;
constexpr uint16_t kCapabilityLocalRecording = 1 << 9;
constexpr uint16_t kCapabilityRecordingStorage = 1 << 10;
constexpr uint16_t kCapabilityRecordingFileTransfer = 1 << 11;
constexpr uint16_t kCapabilityTemporaryAPTransfer = 1 << 12;
constexpr uint16_t kCapabilityTimeSync = 1 << 13;
constexpr uint16_t kCapabilities =
    kCapabilityAutoPowerOff |
    kCapabilityMicrophoneGain |
    kCapabilityCallGain |
    kCapabilityPowerOff |
    kCapabilityResetSettings |
    kCapabilityEraseAndRemove |
    kCapabilityBatteryLevel |
    kCapabilityRecordingMode |
    kCapabilityFirmwareVersion |
    kCapabilityLocalRecording |
    kCapabilityRecordingStorage |
    kCapabilityRecordingFileTransfer |
    kCapabilityTemporaryAPTransfer |
    kCapabilityTimeSync;
constexpr uint16_t kSimulatedCapabilities =
    kCapabilityAutoPowerOff |
    kCapabilityCallGain |
    kCapabilityPowerOff |
    kCapabilityEraseAndRemove |
    kCapabilityRecordingMode;

enum class Opcode : uint8_t {
    kGetDeviceState = 0x01,
    kSetAutoPowerOff = 0x02,
    kPowerOffNow = 0x03,
    kSetMicrophoneGain = 0x04,
    kSetCallGain = 0x05,
    kResetSettings = 0x06,
    kEraseAndRemove = 0x07,
    kSetRecordingMode = 0x08,
    kSyncTime = 0x09,
    kForegroundWake = 0x0A,
    kGetStorageStatus = 0x20,
    kStartLocalRecording = 0x21,
    kStopLocalRecording = 0x22,
    kListLocalRecordings = 0x23,
    kReadLocalRecordingChunk = 0x24,
    kStartTemporaryAPTransfer = 0x25,
    kCompleteTemporaryAPTransfer = 0x26,
    kRefreshStorageStatus = 0x27,
    kDeleteLocalRecording = 0x28,
};

enum class Status : uint8_t {
    kSuccess = 0,
    kInvalidRequest = 1,
    kUnsupportedVersion = 2,
    kUnsupportedOpcode = 3,
    kInvalidValue = 4,
    kStorageUnavailable = 5,
    kBusy = 6,
    kNotRecording = 7,
    kIoError = 8,
    kNotFound = 9,
};

bool IsMaintenanceOpcode(Opcode opcode) {
    switch (opcode) {
        case Opcode::kGetDeviceState:
        case Opcode::kSyncTime:
        case Opcode::kGetStorageStatus:
        case Opcode::kListLocalRecordings:
            return true;
        default:
            return false;
    }
}

class ScopedBleFileTransfer {
public:
    ScopedBleFileTransfer() {
        AudreamPowerManager::GetInstance().SetBleFileTransferActive(true);
    }

    ~ScopedBleFileTransfer() {
        AudreamPowerManager::GetInstance().SetBleFileTransferActive(false);
    }
};

uint16_t ReadUInt16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
        (static_cast<uint16_t>(data[1]) << 8);
}

int32_t ReadInt32(const uint8_t* data) {
    return static_cast<int32_t>(
        static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24)
    );
}

void AppendUInt16(std::vector<uint8_t>& output, uint16_t value) {
    output.push_back(static_cast<uint8_t>(value & 0xFF));
    output.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void AppendInt32(std::vector<uint8_t>& output, int32_t value) {
    const uint32_t raw_value = static_cast<uint32_t>(value);
    output.push_back(static_cast<uint8_t>(raw_value & 0xFF));
    output.push_back(static_cast<uint8_t>((raw_value >> 8) & 0xFF));
    output.push_back(static_cast<uint8_t>((raw_value >> 16) & 0xFF));
    output.push_back(static_cast<uint8_t>((raw_value >> 24) & 0xFF));
}

void AppendUInt32(std::vector<uint8_t>& output, uint32_t value) {
    output.push_back(static_cast<uint8_t>(value & 0xFF));
    output.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    output.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    output.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void AppendUInt64(std::vector<uint8_t>& output, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }
}

void AppendString(std::vector<uint8_t>& output, const std::string& value) {
    const size_t clamped_size = std::min<size_t>(value.size(), 255);
    output.push_back(static_cast<uint8_t>(clamped_size));
    output.insert(output.end(), value.begin(), value.begin() + clamped_size);
}

std::vector<uint8_t> BuildStorageStatusPayload(const StorageStatus& status) {
    std::vector<uint8_t> payload;
    payload.reserve(33);
    payload.push_back(status.mounted ? 1 : 0);
    AppendUInt64(payload, status.total_bytes);
    AppendUInt64(payload, status.free_bytes);
    AppendUInt64(payload, status.recording_bytes);
    AppendUInt32(payload, status.estimated_remaining_minutes);
    AppendUInt32(payload, static_cast<uint32_t>(status.error_code));
    return payload;
}

void LogStorageStatus(const char* operation, const StorageStatus& status) {
    ESP_LOGI(
        kTag,
        "%s storage status mounted=%d error_code=%lu free_mb=%lu remaining_minutes=%lu",
        operation,
        status.mounted ? 1 : 0,
        static_cast<unsigned long>(status.error_code),
        static_cast<unsigned long>(status.free_bytes / (1024 * 1024)),
        static_cast<unsigned long>(status.estimated_remaining_minutes)
    );
}

bool ReadString(const uint8_t* data, size_t length, size_t& cursor, std::string& value) {
    if (cursor >= length) {
        return false;
    }
    const size_t string_length = data[cursor];
    cursor++;
    if (cursor + string_length > length) {
        return false;
    }
    value.assign(
        reinterpret_cast<const char*>(&data[cursor]),
        string_length
    );
    cursor += string_length;
    return true;
}

uint32_t ReadUInt32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t ReadUInt64(const uint8_t* data) {
    uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<uint64_t>(data[shift / 8]) << shift;
    }
    return value;
}

bool IsValidAutoPowerOff(int32_t seconds) {
    return seconds == -1 || seconds == 900 || seconds == 1800 ||
        seconds == 3600 || seconds == 18000;
}

}  // namespace

AudreamDeviceControl& AudreamDeviceControl::GetInstance() {
    static AudreamDeviceControl instance;
    return instance;
}

AudreamDeviceControl::DeviceState AudreamDeviceControl::LoadState() const {
    Settings settings(kSettingsNamespace);
    int battery_level = kDefaultBatteryLevelPercent;
    bool charging = false;
    bool discharging = false;
    const bool has_battery_level = Board::GetInstance().GetBatteryLevel(
        battery_level,
        charging,
        discharging
    );
    battery_level = std::clamp(battery_level, 0, 100);

    unsigned int firmware_major = 0;
    unsigned int firmware_minor = 0;
    unsigned int firmware_patch = 0;
    const esp_app_desc_t* app_description = esp_app_get_description();
    std::sscanf(
        app_description->version,
        "%u.%u.%u",
        &firmware_major,
        &firmware_minor,
        &firmware_patch
    );
    return {
        .auto_power_off_seconds = settings.GetInt(
            kAutoPowerOffKey,
            kDefaultAutoPowerOffSeconds
        ),
        .microphone_gain_db = static_cast<uint8_t>(std::clamp(
            settings.GetInt(kMicrophoneGainKey, kDefaultMicrophoneGainDb),
            0L,
            30L
        )),
        .call_gain = static_cast<uint8_t>(std::clamp(
            settings.GetInt(kCallGainKey, kDefaultCallGain),
            0L,
            2L
        )),
        .battery_level_percent = static_cast<uint8_t>(battery_level),
        .charging = charging,
        .battery_level_simulated = !has_battery_level,
        .recording_mode = static_cast<uint8_t>(std::clamp(
            settings.GetInt(kRecordingModeKey, kDefaultRecordingMode),
            0L,
            1L
        )),
        .firmware_major = static_cast<uint8_t>(std::min(firmware_major, 255U)),
        .firmware_minor = static_cast<uint8_t>(std::min(firmware_minor, 255U)),
        .firmware_patch = static_cast<uint8_t>(std::min(firmware_patch, 255U)),
    };
}

void AudreamDeviceControl::ResetSettings() const {
    Settings settings(kSettingsNamespace, true);
    settings.EraseAll();
}

void AudreamDeviceControl::ApplyMicrophoneGain(uint8_t gain_db) const {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        codec->SetInputGain(gain_db);
    }
}

std::vector<uint8_t> AudreamDeviceControl::BuildResponse(
    uint8_t opcode,
    uint16_t request_id,
    uint8_t status,
    const DeviceState& state
) const {
    std::vector<uint8_t> response;
    response.reserve(21);
    response.push_back(kProtocolVersion);
    response.push_back(opcode | kResponseOpcodeMask);
    AppendUInt16(response, request_id);
    response.push_back(status);
    AppendUInt16(response, kCapabilities);
    AppendUInt16(
        response,
        kSimulatedCapabilities |
            (state.battery_level_simulated ? kCapabilityBatteryLevel : 0)
    );
    AppendInt32(response, state.auto_power_off_seconds);
    response.push_back(state.microphone_gain_db);
    response.push_back(state.call_gain);
    response.push_back(state.battery_level_percent);
    response.push_back(state.charging ? 1 : 0);
    response.push_back(state.recording_mode);
    response.push_back(state.firmware_major);
    response.push_back(state.firmware_minor);
    response.push_back(state.firmware_patch);
    return response;
}

std::vector<uint8_t> AudreamDeviceControl::BuildPayloadResponse(
    uint8_t opcode,
    uint16_t request_id,
    uint8_t status,
    const std::vector<uint8_t>& payload
) const {
    std::vector<uint8_t> response;
    response.reserve(kRequestHeaderLength + 1 + payload.size());
    response.push_back(kProtocolVersion);
    response.push_back(opcode | kResponseOpcodeMask);
    AppendUInt16(response, request_id);
    response.push_back(status);
    response.insert(response.end(), payload.begin(), payload.end());
    return response;
}

std::vector<uint8_t> AudreamDeviceControl::HandleRequest(const uint8_t* data, size_t length) {
    if (data == nullptr || length < kRequestHeaderLength) {
        ESP_LOGW(kTag, "Rejected malformed device control request");
        return BuildResponse(
            0,
            0,
            static_cast<uint8_t>(Status::kInvalidRequest),
            LoadState()
        );
    }

    const uint8_t version = data[0];
    const uint8_t opcode_value = data[1];
    const uint16_t request_id = ReadUInt16(&data[2]);
    if (version != kProtocolVersion) {
        ESP_LOGW(kTag, "Unsupported protocol version: %u", version);
        return BuildResponse(
            opcode_value,
            request_id,
            static_cast<uint8_t>(Status::kUnsupportedVersion),
            LoadState()
        );
    }

    const auto opcode = static_cast<Opcode>(opcode_value);
    if (IsMaintenanceOpcode(opcode)) {
        AudreamPowerManager::GetInstance().NotifyMaintenanceActivity("device_control");
    } else {
        AudreamPowerManager::GetInstance().NotifyUserActivity("device_control");
    }
    switch (opcode) {
        case Opcode::kGetDeviceState:
            ApplyMicrophoneGain(LoadState().microphone_gain_db);
            return BuildResponse(
                opcode_value,
                request_id,
                static_cast<uint8_t>(Status::kSuccess),
                LoadState()
            );

        case Opcode::kSetAutoPowerOff: {
            if (length != kRequestHeaderLength + sizeof(int32_t)) {
                return BuildResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kInvalidRequest),
                    LoadState()
                );
            }
            const int32_t seconds = ReadInt32(&data[kRequestHeaderLength]);
            if (!IsValidAutoPowerOff(seconds)) {
                return BuildResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kInvalidValue),
                    LoadState()
                );
            }
            {
                Settings settings(kSettingsNamespace, true);
                settings.SetInt(kAutoPowerOffKey, seconds);
            }
            ESP_LOGI(kTag, "Stored simulated auto power-off setting: %ld seconds", seconds);
            break;
        }

        case Opcode::kPowerOffNow:
            ESP_LOGI(kTag, "Simulated immediate power-off request received");
            break;

        case Opcode::kSetMicrophoneGain: {
            if (length != kRequestHeaderLength + 1 || data[kRequestHeaderLength] > 30) {
                return BuildResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kInvalidValue),
                    LoadState()
                );
            }
            const uint8_t gain_db = data[kRequestHeaderLength];
            {
                Settings settings(kSettingsNamespace, true);
                settings.SetInt(kMicrophoneGainKey, gain_db);
            }
            ApplyMicrophoneGain(gain_db);
            ESP_LOGI(kTag, "Applied microphone gain: %u dB", gain_db);
            break;
        }

        case Opcode::kSetCallGain: {
            if (length != kRequestHeaderLength + 1 || data[kRequestHeaderLength] > 2) {
                return BuildResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kInvalidValue),
                    LoadState()
                );
            }
            const uint8_t call_gain = data[kRequestHeaderLength];
            {
                Settings settings(kSettingsNamespace, true);
                settings.SetInt(kCallGainKey, call_gain);
            }
            ESP_LOGI(kTag, "Stored simulated call recording gain: %u", call_gain);
            break;
        }

        case Opcode::kSetRecordingMode: {
            if (length != kRequestHeaderLength + 1 || data[kRequestHeaderLength] > 1) {
                return BuildResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kInvalidValue),
                    LoadState()
                );
            }
            const uint8_t recording_mode = data[kRequestHeaderLength];
            {
                Settings settings(kSettingsNamespace, true);
                settings.SetInt(kRecordingModeKey, recording_mode);
            }
            ESP_LOGI(kTag, "Stored simulated recording mode: %u", recording_mode);
            break;
        }

        case Opcode::kSyncTime: {
            if (length != kRequestHeaderLength + sizeof(uint64_t)) {
                return BuildResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kInvalidRequest),
                    LoadState()
                );
            }
            const uint64_t unix_time_ms = ReadUInt64(&data[kRequestHeaderLength]);
            DeviceTimeService::GetInstance().SyncUnixTimeMs(unix_time_ms);
            break;
        }

        case Opcode::kForegroundWake:
            ESP_LOGI(kTag, "Foreground wake request received");
            break;

        case Opcode::kGetStorageStatus: {
            auto status = SdCardStorage::GetInstance().GetStatus();
            LogStorageStatus("Cached", status);
            return BuildPayloadResponse(
                opcode_value,
                request_id,
                static_cast<uint8_t>(Status::kSuccess),
                BuildStorageStatusPayload(status)
            );
        }

        case Opcode::kRefreshStorageStatus: {
            auto status = SdCardStorage::GetInstance().ProbeAndMountStatus();
            LogStorageStatus("Refreshed", status);
            return BuildPayloadResponse(
                opcode_value,
                request_id,
                static_cast<uint8_t>(Status::kSuccess),
                BuildStorageStatusPayload(status)
            );
        }

        case Opcode::kStartLocalRecording: {
            auto& recording = LocalRecordingService::GetInstance();
            if (recording.IsRecording()) {
                return BuildPayloadResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kBusy),
                    {}
                );
            }
            if (!recording.Start(Application::GetInstance().GetAudioService())) {
                return BuildPayloadResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kStorageUnavailable),
                    {}
                );
            }
            const auto metadata = recording.CurrentMetadata();
            std::vector<uint8_t> payload;
            AppendString(payload, metadata.recording_id);
            AppendString(payload, metadata.file_name);
            return BuildPayloadResponse(
                opcode_value,
                request_id,
                static_cast<uint8_t>(Status::kSuccess),
                payload
            );
        }

        case Opcode::kStopLocalRecording: {
            auto& recording = LocalRecordingService::GetInstance();
            if (!recording.IsRecording()) {
                return BuildPayloadResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kNotRecording),
                    {}
                );
            }
            const auto metadata = recording.Stop(Application::GetInstance().GetAudioService());
            std::vector<uint8_t> payload;
            AppendUInt32(payload, metadata.duration_ms);
            AppendUInt32(payload, metadata.size_bytes);
            AppendUInt32(payload, metadata.packet_count);
            AppendUInt64(payload, metadata.started_at_unix_ms);
            AppendString(payload, metadata.recording_id);
            AppendString(payload, metadata.file_name);
            return BuildPayloadResponse(
                opcode_value,
                request_id,
                metadata.valid
                    ? static_cast<uint8_t>(Status::kSuccess)
                    : static_cast<uint8_t>(Status::kIoError),
                payload
            );
        }

        case Opcode::kListLocalRecordings: {
            if (length != kRequestHeaderLength + 3) {
                return BuildPayloadResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kInvalidRequest),
                    {}
                );
            }
            const uint8_t max_count = std::clamp<uint8_t>(data[kRequestHeaderLength], 1, 16);
            const uint16_t offset = ReadUInt16(&data[kRequestHeaderLength + 1]);
            size_t total_count = 0;
            auto recordings = SdCardStorage::GetInstance().ListRecordings(
                offset,
                max_count,
                &total_count
            );
            std::vector<uint8_t> payload;
            payload.push_back(0);
            uint8_t appended_count = 0;
            std::string first_appended_id;
            for (const auto& recording : recordings) {
                const size_t entry_size =
                    1 + std::min<size_t>(recording.recording_id.size(), 255) +
                    1 + std::min<size_t>(recording.file_name.size(), 255) +
                    sizeof(uint32_t) +
                    sizeof(uint32_t) +
                    sizeof(uint64_t);
                if (payload.size() + entry_size > kMaxListRecordingsPayloadBytes) {
                    break;
                }
                if (first_appended_id.empty()) {
                    first_appended_id = recording.recording_id;
                }
                AppendString(payload, recording.recording_id);
                AppendString(payload, recording.file_name);
                AppendUInt32(payload, recording.duration_ms);
                AppendUInt32(payload, recording.size_bytes);
                AppendUInt64(payload, recording.started_at_unix_ms);
                appended_count++;
            }
            payload[0] = appended_count;
            const bool has_more = offset + appended_count < total_count;
            ESP_LOGI(
                kTag,
                "Listed local recordings: offset=%u total=%u returned=%u has_more=%d payload_size=%u first=%s",
                static_cast<unsigned>(offset),
                static_cast<unsigned>(total_count),
                static_cast<unsigned>(appended_count),
                has_more ? 1 : 0,
                static_cast<unsigned>(payload.size()),
                first_appended_id.c_str()
            );
            return BuildPayloadResponse(
                opcode_value,
                request_id,
                static_cast<uint8_t>(Status::kSuccess),
                payload
            );
        }

        case Opcode::kReadLocalRecordingChunk: {
            ScopedBleFileTransfer transfer_blocker;
            size_t cursor = kRequestHeaderLength;
            std::string recording_id;
            if (!ReadString(data, length, cursor, recording_id) ||
                cursor + sizeof(uint32_t) + sizeof(uint16_t) != length) {
                return BuildPayloadResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kInvalidRequest),
                    {}
                );
            }
            const uint32_t offset = ReadUInt32(&data[cursor]);
            cursor += sizeof(uint32_t);
            const uint16_t requested_length = ReadUInt16(&data[cursor]);
            const size_t chunk_length = std::min<size_t>(requested_length, kMaxReadChunkBytes);
            std::vector<uint8_t> chunk;
            uint32_t total_size = 0;
            if (!SdCardStorage::GetInstance().ReadRecordingChunk(
                    recording_id,
                    offset,
                    chunk_length,
                    chunk,
                    total_size
                )) {
                return BuildPayloadResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kNotFound),
                    {}
                );
            }
            std::vector<uint8_t> payload;
            AppendUInt32(payload, offset);
            AppendUInt32(payload, total_size);
            AppendUInt16(payload, static_cast<uint16_t>(chunk.size()));
            payload.push_back(offset + chunk.size() >= total_size ? 1 : 0);
            payload.insert(payload.end(), chunk.begin(), chunk.end());
            return BuildPayloadResponse(
                opcode_value,
                request_id,
                static_cast<uint8_t>(Status::kSuccess),
                payload
            );
        }

        case Opcode::kStartTemporaryAPTransfer: {
            if (length != kRequestHeaderLength) {
                return BuildPayloadResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kInvalidRequest),
                    {}
                );
            }
            auto session = TemporaryAPTransferService::GetInstance().Start();
            if (!session.valid) {
                return BuildPayloadResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kNotFound),
                    {}
                );
            }
            std::vector<uint8_t> payload;
            AppendString(payload, session.ssid);
            AppendString(payload, session.password);
            AppendString(payload, session.url);
            AppendString(payload, session.token);
            AppendUInt32(payload, session.expires_in_seconds);
            return BuildPayloadResponse(
                opcode_value,
                request_id,
                static_cast<uint8_t>(Status::kSuccess),
                payload
            );
        }

        case Opcode::kCompleteTemporaryAPTransfer: {
            size_t cursor = kRequestHeaderLength;
            std::string token;
            if (!ReadString(data, length, cursor, token) ||
                cursor != length) {
                return BuildPayloadResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kInvalidRequest),
                    {}
                );
            }
            if (!TemporaryAPTransferService::GetInstance().Complete(token)) {
                return BuildPayloadResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kNotFound),
                    {}
                );
            }
            return BuildPayloadResponse(
                opcode_value,
                request_id,
                static_cast<uint8_t>(Status::kSuccess),
                {}
            );
        }

        case Opcode::kDeleteLocalRecording: {
            size_t cursor = kRequestHeaderLength;
            std::string recording_id;
            if (!ReadString(data, length, cursor, recording_id) || cursor != length) {
                return BuildPayloadResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kInvalidRequest),
                    {}
                );
            }
            auto& recording_service = LocalRecordingService::GetInstance();
            if (recording_service.IsRecording() &&
                recording_service.CurrentMetadata().recording_id == recording_id) {
                return BuildPayloadResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kBusy),
                    {}
                );
            }
            if (!SdCardStorage::GetInstance().DeleteRecording(recording_id)) {
                return BuildPayloadResponse(
                    opcode_value,
                    request_id,
                    static_cast<uint8_t>(Status::kNotFound),
                    {}
                );
            }
            return BuildPayloadResponse(
                opcode_value,
                request_id,
                static_cast<uint8_t>(Status::kSuccess),
                {}
            );
        }

        case Opcode::kResetSettings:
            ResetSettings();
            ApplyMicrophoneGain(kDefaultMicrophoneGainDb);
            ESP_LOGI(kTag, "Reset Audream device settings to defaults");
            break;

        case Opcode::kEraseAndRemove:
            ResetSettings();
            ApplyMicrophoneGain(kDefaultMicrophoneGainDb);
            ESP_LOGI(kTag, "Simulated erase-and-remove request completed");
            break;

        default:
            ESP_LOGW(kTag, "Unsupported device control opcode: %u", opcode_value);
            return BuildResponse(
                opcode_value,
                request_id,
                static_cast<uint8_t>(Status::kUnsupportedOpcode),
                LoadState()
            );
    }

    return BuildResponse(
        opcode_value,
        request_id,
        static_cast<uint8_t>(Status::kSuccess),
        LoadState()
    );
}
