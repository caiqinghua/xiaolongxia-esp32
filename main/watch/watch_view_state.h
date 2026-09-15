#pragma once

#include "watch_time_service.h"
#include "watch_assistant_state.h"
#include "watch_recording_state.h"
#include "watch_recording_library.h"
#include "watch_push_to_talk.h"
#include "watch_level_settings.h"
#include "watch_power_mode.h"
#include "watch_activity.h"
#include "watch_wrist_control.h"
#include "watch_alert_sound.h"
#include "watch_notification_state.h"
#include "watch_pairing_view_state.h"
#include "watch_firmware_view_state.h"
#include "audio/recording_playback_task.h"

enum class WatchPage { kFace, kApps, kTimer, kStopwatch, kAlarms, kAlarmEditor, kSettings, kAssistant, kRecorder,
    kRecordings, kRecordingDetail, kControls, kActivity, kActivityHistory, kActivityResetConfirm, kRecordingDeleteConfirm,
    kPlayback, kNotifications, kNotificationDetail, kWristSettings, kPowerSettings, kDevicePairing,
    kFirmwareUpdate, kFirmwareConfirm };
enum class WatchViewAction { kTimeCommand, kSetAlarm, kSetPreferences,
    kAssistantTalk, kAssistantSend, kAssistantEnd, kAssistantClear, kRecordingCommand,
    kLoadRecordings, kCancelRecordings, kAssistantPress, kAssistantRelease, kAssistantCancelPress, kSetLevel, kSetVoiceEnabled,
    kSetWifiEnabled, kRetryActivityStorage, kResetCorruptActivityStorage, kLoadOlderRecordings, kLoadNewerRecordings,
    kDeleteRecording, kPlayRecording, kPausePlayback, kResumePlayback, kStopPlayback, kSetPlaybackVolume, kSeekPlayback,
    kSetNotificationsEnabled, kConfirmNotificationPeer, kOpenNotification, kClearNotifications, kWristCommand, kSetPowerMode,
    kPrepareAssistantNetwork, kOpenDevicePairing, kCancelDevicePairing, kConfirmDevicePairingPeer, kApproveDevicePairingAccount,
    kOpenBindingRemoval, kConfirmBindingRemoval, kOpenControlRemoval, kConfirmControlRemoval,
    kOpenSavedControlPeers, kSelectSavedControlPeer, kSelectAllSavedControlPeers, kConfirmAllControlRemoval,
    kCheckFirmware, kCancelFirmware, kInstallFirmware, kRebootFirmware,
    kSelectUpdateTarget, kValidateModels, kCancelModelValidation, kRollbackModels };

struct WatchViewRequest {
    uint64_t id = 0;
    WatchViewAction action = WatchViewAction::kTimeCommand;
    WatchTimeCommand command = WatchTimeCommand::kStartTimer;
    uint64_t duration_ms = 0;
    size_t alarm_index = 0;
    WatchAlarm alarm;
    std::string timezone;
    bool hour24 = true;
    WatchRecordingCommand recording_command = WatchRecordingCommand::kStart;
    uint64_t press_token = 0;
    WatchLevel level = WatchLevel::kBrightness;
    int level_value = 0;
    bool enabled = false;
    uint64_t settings_revision = 0;
    uint64_t binding_generation = 0;
    std::string power_mode;
    WatchWristAction wrist_action = WatchWristAction::kCancel;
    uint64_t repair_token = 0;
    uint64_t library_revision = 0;
    std::string recording_id;
    uint64_t cleanup_request = 0;
    bool metadata_only = false;
    uint64_t playback_generation = 0;
    uint64_t playback_target_sample = 0;
    uint64_t notification_confirmation = 0;
    uint64_t pairing_attempt = 0;
    uint64_t pairing_open_request = 0;
    bool pairing_control_only = false;
    uint64_t pairing_confirmation = 0;
    uint64_t pairing_request = 0;
    uint64_t pairing_peer_generation = 0;
    uint64_t firmware_request = 0;
    uint64_t firmware_attempt = 0;
    uint32_t firmware_sequence = 0;
    watch_update::Target update_target = watch_update::Target::kFirmware;
    WatchNotificationSource notification_source = WatchNotificationSource::kNone;
    uint64_t notification_session = 0;
    uint64_t notification_revision = 0;
    uint32_t notification_uid = 0;
};

struct WatchViewState {
    WatchClockReading clock;
    WatchTimeSnapshot time;
    WatchAlertSoundSnapshot alert_sound;
    int battery_percent = -1;
    bool charging = false;
    bool wifi_connected = false;
    bool ble_connected = false;
    bool recording = false;
    uint64_t recording_ms = 0;
    WatchAssistantSnapshot assistant;
    WatchRecordingSnapshot recorder;
    WatchRecordingLibrarySnapshot library;
    WatchRecordingDeleteSnapshot deletion;
    bool can_delete_recording = false;
    bool can_play_recording = false;
    RecordingPlaybackSnapshot playback;
    WatchControlSnapshot controls;
    WatchPowerModeSnapshot power_mode;
    WatchMotionSnapshot motion;
    WatchWristSnapshot wrist;
    WatchNotificationSnapshot notifications;
    WatchPairingSnapshot pairing;
    WatchFirmwareSnapshot firmware;
};
