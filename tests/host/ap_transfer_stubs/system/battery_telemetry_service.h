#pragma once
enum class BatteryTelemetryEvent { kTransferStopped };
class BatteryTelemetryService {
public:
    static BatteryTelemetryService& GetInstance();
    void RecordEvent(BatteryTelemetryEvent);
};
