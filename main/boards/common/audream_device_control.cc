#include "audream_device_control.h"

#include <algorithm>
#include <cstdio>

#include "audio/audio_codec.h"
#include "board.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "settings.h"

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
constexpr uint16_t kCapabilities =
    kCapabilityAutoPowerOff |
    kCapabilityMicrophoneGain |
    kCapabilityCallGain |
    kCapabilityPowerOff |
    kCapabilityResetSettings |
    kCapabilityEraseAndRemove |
    kCapabilityBatteryLevel |
    kCapabilityRecordingMode |
    kCapabilityFirmwareVersion;
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
};

enum class Status : uint8_t {
    kSuccess = 0,
    kInvalidRequest = 1,
    kUnsupportedVersion = 2,
    kUnsupportedOpcode = 3,
    kInvalidValue = 4,
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
