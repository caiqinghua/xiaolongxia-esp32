#include "device_state_machine.h"

#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void TestOfflineStartup() {
    DeviceStateMachine state;
    std::vector<std::pair<DeviceState, DeviceState>> events;
    const auto listener = state.AddStateChangeListener([&](DeviceState from, DeviceState to) {
        events.emplace_back(from, to);
    });
    Check(state.TransitionTo(kDeviceStateStarting), "initialize device before offline startup");
    Check(state.CanTransitionTo(kDeviceStateIdle), "offline readiness permitted");
    Check(state.TransitionTo(kDeviceStateIdle), "watch ready without network or activation");
    Check(state.GetState() == kDeviceStateIdle, "watch does not remain starting");
    Check(events.size() == 2 && events.back() == std::make_pair(kDeviceStateStarting, kDeviceStateIdle),
          "offline readiness delivered to application listener");
    Check(state.TransitionTo(kDeviceStateIdle) && events.size() == 2, "idle no-op does not repeat notifications");
    state.RemoveStateChangeListener(listener);
    Check(state.TransitionTo(kDeviceStateConnecting) && events.size() == 2, "assistant can start after offline ready");
}

void TestLegacyActivation() {
    DeviceStateMachine saved_wifi;
    Check(saved_wifi.TransitionTo(kDeviceStateStarting), "legacy startup");
    Check(saved_wifi.TransitionTo(kDeviceStateActivating), "legacy saved WiFi activation");
    Check(saved_wifi.TransitionTo(kDeviceStateIdle), "legacy activation completes");
    DeviceStateMachine provisioning;
    Check(provisioning.TransitionTo(kDeviceStateStarting), "legacy provisioning startup");
    Check(provisioning.TransitionTo(kDeviceStateWifiConfiguring), "legacy configuration retained");
    Check(!provisioning.TransitionTo(kDeviceStateIdle), "configuration does not bypass legacy activation");
    Check(provisioning.TransitionTo(kDeviceStateActivating), "legacy provisioned device activates");
    Check(provisioning.TransitionTo(kDeviceStateUpgrading), "legacy automatic upgrade path retained");
    Check(provisioning.TransitionTo(kDeviceStateActivating), "legacy upgrade returns to activation");
    Check(provisioning.TransitionTo(kDeviceStateIdle), "legacy provisioned device ready");
}

void TestInvalidStartupTransitions() {
    DeviceStateMachine state;
    unsigned int callbacks = 0;
    state.AddStateChangeListener([&](DeviceState, DeviceState) { ++callbacks; });
    Check(!state.TransitionTo(kDeviceStateIdle), "unknown device cannot skip initialization");
    Check(!state.TransitionTo(kDeviceStateListening), "unknown device cannot acquire microphone");
    Check(callbacks == 0 && state.GetState() == kDeviceStateUnknown, "rejected transitions have no side effects");
    Check(state.TransitionTo(kDeviceStateStarting), "normal initialization");
    Check(!state.TransitionTo(kDeviceStateListening), "starting cannot skip readiness for listening");
    Check(!state.TransitionTo(kDeviceStateSpeaking), "starting cannot skip readiness for playback");
    Check(callbacks == 1 && state.GetState() == kDeviceStateStarting, "invalid startup transitions stay rejected");
}
}  // namespace

int main() {
    TestOfflineStartup();
    TestLegacyActivation();
    TestInvalidStartupTransitions();
    std::cout << "3 device startup state machine tests passed\n";
}
