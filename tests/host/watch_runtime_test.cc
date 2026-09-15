#include "board_runtime_profile.h"
#include "blufi_session_state.h"
#include "wifi_credential_fields.h"
#include "recording_button_intent.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void TestLegacyProfiles() {
    Check(LegacyBoardRuntimeProfile(false) == BoardRuntimeProfile::kAssistant, "assistant mapping");
    Check(LegacyBoardRuntimeProfile(true) == BoardRuntimeProfile::kRecordingOnly, "recorder mapping");
    Check(!KeepsAudreamBleControl(BoardRuntimeProfile::kAssistant), "legacy BLE policy");
    Check(KeepsAudreamBleControl(BoardRuntimeProfile::kRecordingOnly), "recorder BLE policy");
    Check(KeepsAudreamBleControl(BoardRuntimeProfile::kWatchAssistantRecorder), "watch BLE policy");
    Check(WaveshareWatchRuntimeProfile(false) == BoardRuntimeProfile::kRecordingOnly,
          "disabled watch preserves production recorder");
    Check(WaveshareWatchRuntimeProfile(true) == BoardRuntimeProfile::kWatchAssistantRecorder,
          "experimental watch selects combined runtime");
    Check(!UsesOfflineWatchStartup(BoardRuntimeProfile::kAssistant), "legacy assistant activation retained");
    Check(!UsesOfflineWatchStartup(BoardRuntimeProfile::kRecordingOnly), "legacy recorder startup retained");
    Check(UsesOfflineWatchStartup(BoardRuntimeProfile::kWatchAssistantRecorder), "watch starts offline");
    Check(UsesDefaultWakeWordDetection(BoardRuntimeProfile::kAssistant), "legacy wake word retained");
    Check(!UsesDefaultWakeWordDetection(BoardRuntimeProfile::kRecordingOnly), "recorder has no background wake word");
    Check(!UsesDefaultWakeWordDetection(BoardRuntimeProfile::kWatchAssistantRecorder), "watch standby leaves microphone off");
    Check(UsesIdleDeepSleep(BoardRuntimeProfile::kAssistant), "legacy assistant power policy retained");
    Check(UsesIdleDeepSleep(BoardRuntimeProfile::kRecordingOnly), "recorder idle deep sleep retained");
    Check(!UsesIdleDeepSleep(BoardRuntimeProfile::kWatchAssistantRecorder), "watch standby retains continuous services");
}

void TestShortRecordingPress() {
    RecordingButtonIntent intent(1500000);
    Check(intent.ConfirmHold() == 0, "no hold without a press");
    intent.Press(100);
    Check(intent.ConfirmElapsed(1500099) == 0, "hold threshold not reached");
    Check(intent.Release(1500099) == 0, "short release does not start recording");
    Check(!intent.LastPressWasHold(), "short click remains available");
    Check(intent.ConfirmElapsed(9000000) == 0, "released press cannot mature later");
}

void TestReleasedRecordingHold() {
    RecordingButtonIntent intent(1500000);
    intent.Press(100);
    const auto token = intent.Release(1500100);
    Check(token != 0, "release at threshold confirms intent even before SDK hold callback");
    Check(intent.LastPressWasHold(), "hold cannot also add a marker");
    Check(intent.ConfirmHold() == 0 && intent.Release(9000000) == 0, "late callbacks cannot duplicate hold");
    Check(intent.Consume(token), "application may consume after physical release");
    Check(!intent.Consume(token), "hold consumed only once");
}

void TestBootRecordingHold() {
    RecordingButtonIntent intent(1500000);
    intent.Press(0);
    intent.Press(500000);
    const auto token = intent.ConfirmElapsed(1500000);
    Check(token != 0, "SDK press callback does not restart seeded boot hold");
    Check(intent.ConfirmHold() == 0, "SDK long press cannot repeat boot hold");
    Check(intent.Release(2000000) == 0, "boot release preserves one action");
    Check(intent.Consume(token), "boot intent survives delayed application startup");
    intent.Press(3000000);
    intent.CancelPress();
    Check(intent.ConfirmElapsed(9000000) == 0, "unknown release time does not invent boot intent");
}

void TestRecordingHoldGenerations() {
    RecordingButtonIntent intent(1500000);
    intent.Press(0);
    const auto first = intent.ConfirmHold();
    intent.Release(2000000);
    intent.Press(3000000);
    const auto second = intent.ConfirmHold();
    Check(second > first, "new physical press issues a new action");
    Check(!intent.Consume(0) && !intent.Consume(second + 1), "invalid action rejected");
    Check(intent.Consume(first) && intent.Consume(second), "ordered holds both delivered");
    Check(!intent.Consume(first), "stale action cannot toggle recording again");
    intent.Release(5000000);
    intent.Press(6000000);
    const auto third = intent.ConfirmHold();
    intent.Release(8000000);
    intent.Press(9000000);
    const auto fourth = intent.ConfirmHold();
    Check(intent.Consume(fourth) && !intent.Consume(third), "late older delivery cannot reverse newer intent");
}

void TestConcurrentRecordingHold() {
    RecordingButtonIntent intent(1500000);
    intent.Press(0);
    std::array<uint64_t, 3> tokens{};
    std::thread timer([&]() { tokens[0] = intent.ConfirmElapsed(1500000); });
    std::thread button([&]() { tokens[1] = intent.ConfirmHold(); });
    std::thread release([&]() { tokens[2] = intent.Release(1500000); });
    timer.join();
    button.join();
    release.join();
    unsigned int issued = 0;
    for (auto token : tokens) {
        if (token != 0) {
            ++issued;
            Check(intent.Consume(token), "concurrent hold action deliverable");
        }
    }
    Check(issued == 1, "timer and SDK callbacks produce exactly one action");
}

void TestRecordingHoldInvalidClock() {
    RecordingButtonIntent disabled(0);
    disabled.Press(0);
    Check(disabled.ConfirmHold() == 0 && disabled.Release(5000000) == 0, "invalid threshold disabled");
    RecordingButtonIntent intent(1500000);
    intent.Press(-1);
    Check(intent.ConfirmHold() == 0, "invalid press clock rejected");
    intent.Press(2000000);
    Check(intent.Release(1000000) == 0, "backward clock cannot confirm a hold");
    Check(intent.ConfirmElapsed(9000000) == 0, "invalid release still ends physical press");
}

void TestInactiveAndControlOnly() {
    BlufiSessionState session;
    Check(!session.IsActive() && !session.AllowsProvisioning(), "initial inactive state");
    Check(session.BeginConnection() == 0, "inactive connection rejected");
    Check(!session.CompleteConnection(0, true), "empty completion rejected");
    session.Activate(BlufiRole::kControlOnly);
    Check(session.IsActive() && session.KeepsControl(), "control activated");
    Check(!session.AllowsProvisioning(), "control does not enable WiFi");
    Check(session.BeginConnection() == 0, "control-only connection rejected");
}

void TestCombinedCompletion() {
    BlufiSessionState session;
    session.Activate(BlufiRole::kControlOnly);
    session.Activate(BlufiRole::kProvisioningAndControl);
    Check(session.KeepsControl() && session.AllowsProvisioning(), "combined role enabled");
    const auto ticket = session.BeginConnection();
    Check(ticket != 0 && session.IsCurrent(ticket), "connection ticket issued");
    Check(session.BeginConnection() == 0, "duplicate connection rejected");
    Check(session.CompleteConnection(ticket, true), "combined completion accepted");
    Check(session.IsActive() && session.KeepsControl(), "BLE remains active after success");
    Check(!session.AllowsProvisioning(), "provisioning closes after success");
    Check(session.Role() == BlufiRole::kControlOnly, "combined role demoted to control");
    Check(!session.CompleteConnection(ticket, true), "duplicate completion ignored");
}

void TestLegacyCompletion() {
    BlufiSessionState session;
    session.Activate(BlufiRole::kWifiProvisioning);
    Check(!session.KeepsControl(), "legacy provisioning has no persistent owner");
    Check(session.CompleteConnection(session.BeginConnection(), true), "legacy completion accepted");
    Check(session.IsProvisioned() && !session.KeepsControl(), "legacy eligible for shutdown");
    const auto shutdown_ticket = session.Snapshot();
    session.Activate(BlufiRole::kControlOnly);
    Check(!session.IsCurrent(shutdown_ticket), "old shutdown cannot affect control role");
}

void TestFailedConnectionRetry() {
    BlufiSessionState session;
    session.Activate(BlufiRole::kProvisioningAndControl);
    const auto first = session.BeginConnection();
    Check(session.CompleteConnection(first, false), "failure accepted");
    Check(session.AllowsProvisioning() && !session.IsConnecting(), "retry remains available");
    const auto retry = session.BeginConnection();
    Check(retry != 0 && retry != first, "retry has new identity");
    Check(!session.CompleteConnection(first, true), "old success cannot finish retry");
    Check(session.IsCurrent(retry), "retry unchanged by stale result");
}

void TestRoleChangeCancelsConnection() {
    BlufiSessionState session;
    session.Activate(BlufiRole::kProvisioningAndControl);
    const auto ticket = session.BeginConnection();
    session.Activate(BlufiRole::kControlOnly);
    Check(!session.IsCurrent(ticket), "role change cancels connection");
    Check(!session.CompleteConnection(ticket, true), "stale completion after role change");
    Check(session.KeepsControl() && !session.AllowsProvisioning(), "control state preserved");
}

void TestIdempotentInitialization() {
    BlufiSessionState session;
    Check(session.Activate(BlufiRole::kProvisioningAndControl), "initial activation changes state");
    const auto ticket = session.BeginConnection();
    Check(!session.Activate(BlufiRole::kProvisioningAndControl), "same-role init is a no-op");
    Check(session.IsCurrent(ticket), "repeated init preserves connection");
    Check(session.CompleteConnection(ticket, true), "connection completes after repeated init");
}

void TestShutdownAndRestart() {
    BlufiSessionState session;
    session.Activate(BlufiRole::kProvisioningAndControl);
    const auto ticket = session.BeginConnection();
    session.Deactivate();
    Check(!session.IsActive(), "shutdown disables roles");
    session.Deactivate();
    session.Activate(BlufiRole::kProvisioningAndControl);
    const auto next = session.BeginConnection();
    Check(next != 0 && next != ticket, "restart creates distinct attempt");
    Check(!session.CompleteConnection(ticket, true), "previous lifecycle result rejected");
    Check(session.IsCurrent(next), "new lifecycle unchanged");
}

void TestPeerChangeInvalidatesRequests() {
    BlufiSessionState session;
    session.Activate(BlufiRole::kProvisioningAndControl);
    const auto scan = session.Snapshot();
    const auto ticket = session.BeginConnection();
    session.InvalidateRequests();
    Check(!session.IsCurrent(scan) && !session.IsCurrent(ticket), "old peer callbacks invalidated");
    Check(session.AllowsProvisioning() && session.KeepsControl(), "peer change preserves roles");
    Check(session.BeginConnection() != 0, "new peer can connect");
}

void TestConcurrentConnectionRequests() {
    BlufiSessionState session;
    session.Activate(BlufiRole::kProvisioningAndControl);
    std::array<BlufiSessionState::Ticket, 16> tickets{};
    std::vector<std::thread> threads;
    for (size_t i = 0; i < tickets.size(); ++i) {
        threads.emplace_back([&session, &tickets, i] { tickets[i] = session.BeginConnection(); });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    size_t winners = 0;
    for (const auto ticket : tickets) {
        winners += ticket != 0;
    }
    Check(winners == 1, "exactly one concurrent connection accepted");
}

void TestCredentialFieldBoundaries() {
    uint8_t ssid[32]{};
    uint8_t password[64]{};
    const std::string full_ssid(32, 's');
    const std::string full_password(64, 'a');
    Check(CopyWifiCredentialField(ssid, reinterpret_cast<const uint8_t*>(full_ssid.data()),
                                  full_ssid.size(), false), "32-byte SSID accepted");
    Check(ReadWifiCredentialField(ssid) == full_ssid, "full SSID bounded read");
    Check(CopyWifiCredentialField(password, reinterpret_cast<const uint8_t*>(full_password.data()),
                                  full_password.size(), true), "64-byte key accepted");
    Check(ReadWifiCredentialField(password) == full_password, "full password bounded read");
    const std::string oversized(65, 'x');
    Check(!CopyWifiCredentialField(ssid, reinterpret_cast<const uint8_t*>(oversized.data()), 33, false),
          "oversized SSID rejected");
    Check(ReadWifiCredentialField(ssid).empty(), "invalid SSID clears previous value");
    Check(!CopyWifiCredentialField(password, reinterpret_cast<const uint8_t*>(oversized.data()), 65, true),
          "oversized password rejected");
    Check(ReadWifiCredentialField(password).empty(), "invalid password clears previous value");
}

void TestCredentialFieldInvalidInput() {
    uint8_t field[32]{};
    const uint8_t embedded_null[] = {'a', 0, 'b'};
    Check(!CopyWifiCredentialField(field, embedded_null, sizeof(embedded_null), false), "NUL rejected");
    Check(!CopyWifiCredentialField(field, nullptr, 1, false), "null nonempty input rejected");
    Check(!CopyWifiCredentialField(field, nullptr, static_cast<size_t>(-1), false), "negative wire length rejected");
    Check(!CopyWifiCredentialField(field, nullptr, 0, false), "empty SSID rejected");
    Check(CopyWifiCredentialField(field, nullptr, 0, true), "open network password accepted");
    const uint8_t utf8_ssid[] = {0xe8, 0xbf, 0xbd, 0xe9, 0x9f, 0xb3};
    Check(CopyWifiCredentialField(field, utf8_ssid, sizeof(utf8_ssid), false), "UTF-8 SSID accepted");
    Check(ReadWifiCredentialField(field).size() == sizeof(utf8_ssid), "SSID uses byte length");
}

}  // namespace

int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        {"legacy_profiles", TestLegacyProfiles},
        {"short_recording_press", TestShortRecordingPress},
        {"released_recording_hold", TestReleasedRecordingHold},
        {"boot_recording_hold", TestBootRecordingHold},
        {"recording_hold_generations", TestRecordingHoldGenerations},
        {"concurrent_recording_hold", TestConcurrentRecordingHold},
        {"recording_hold_invalid_clock", TestRecordingHoldInvalidClock},
        {"inactive_and_control_only", TestInactiveAndControlOnly},
        {"combined_completion", TestCombinedCompletion},
        {"legacy_completion", TestLegacyCompletion},
        {"failed_connection_retry", TestFailedConnectionRetry},
        {"role_change_cancels_connection", TestRoleChangeCancelsConnection},
        {"idempotent_initialization", TestIdempotentInitialization},
        {"shutdown_and_restart", TestShutdownAndRestart},
        {"peer_change_invalidates_requests", TestPeerChangeInvalidatesRequests},
        {"concurrent_connection_requests", TestConcurrentConnectionRequests},
        {"credential_field_boundaries", TestCredentialFieldBoundaries},
        {"credential_field_invalid_input", TestCredentialFieldInvalidInput},
    };
    for (const auto& test : tests) {
        test.run();
        std::cout << "PASS: " << test.name << '\n';
    }
}
