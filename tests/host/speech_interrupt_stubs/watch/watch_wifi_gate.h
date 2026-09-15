#pragma once
struct SpeechTestWifiGate {
    bool allowed = true;
    bool StationAllowed() const { return allowed; }
};
inline SpeechTestWifiGate& GetWatchWifiGate() { static SpeechTestWifiGate gate; return gate; }
