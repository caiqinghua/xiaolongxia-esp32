#pragma once
#include "watch/watch_firmware_update.h"
class Ota {
public:
    static firmware_release::CheckResult CheckSignedRelease(const watch_update::Controller::Cancelled&);
    static firmware_release::InstallResult InstallSignedEnvelope(const std::string&,
        const watch_update::Controller::Progress&, const watch_update::Controller::Cancelled&,
        const watch_update::Controller::Cancelled&);
};
