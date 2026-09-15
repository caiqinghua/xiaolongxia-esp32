#pragma once
#include <cassert>
#include <cstdint>
#include "system/audream_power_manager.h"
#include "asset_update_client.h"
namespace asset_storage {
class UpdateSession {
public:
    using Cancelled = asset_update::Client::Cancelled;
    using Progress = asset_update::Client::Progress;
    using CheckHook = std::function<asset_update::CheckResult(const std::string&, const Cancelled&)>;
    using InstallHook = std::function<StageResult(const std::string&, const Progress&, const Cancelled&, const Cancelled&)>;
    UpdateSession(CheckHook check, InstallHook install, std::function<void()> release)
        : check_(std::move(check)), install_(std::move(install)), release_(std::move(release)) {}
    ~UpdateSession() { release_(); }
    asset_update::CheckResult Check(const std::string& url, const Cancelled& cancelled) { return check_(url, cancelled); }
    StageResult Install(const std::string& envelope, const Progress& progress, const Cancelled& cancelled,
                        const Cancelled& commit) { return install_(envelope, progress, cancelled, commit); }
private:
    CheckHook check_;
    InstallHook install_;
    std::function<void()> release_;
};
}
struct srmodel_list_t {};
class Assets {
public:
    bool available = true;
    bool models_ready = false;
    unsigned health_begins = 0;
    unsigned health_polls = 0;
    bool last_healthy = false, keep_observing = true;
    bool updates_ready = true, update_owned = false, update_acquire_ok = true;
    unsigned update_acquires = 0, update_returns = 0;
    asset_storage::UpdateSession::CheckHook update_check;
    asset_storage::UpdateSession::InstallHook update_install;
    srmodel_list_t models;
    static Assets& GetInstance() { static Assets assets; return assets; }
    bool LoadAssistantModels(srmodel_list_t*& result) {
        result = available ? &models : nullptr;
        return available;
    }
    bool BeginTrialHealth(bool ready, int64_t now_ms, uint32_t wake_feeds) {
        assert(now_ms >= 0 && wake_feeds == 123);
        ++health_begins; models_ready = ready; return ready;
    }
    bool PollTrialHealth(int64_t now_ms, bool healthy, uint32_t wake_feeds) {
        assert(now_ms >= 0 && wake_feeds == 123);
        assert(AudreamPowerManager::GetInstance().storage_leases == (healthy ? 1 : 0));
        ++health_polls; last_healthy = healthy; return keep_observing;
    }
    bool UpdatesReady() const { return updates_ready && !update_owned; }
    std::string CurrentVersion() const { return {}; }
    bool partition_valid() const { return available; }
    std::unique_ptr<asset_storage::UpdateSession> BeginUpdate() {
        assert(AudreamPowerManager::GetInstance().storage_leases == 1);
        if (!UpdatesReady() || !update_acquire_ok || !update_check || !update_install) return {};
        update_owned = true; ++update_acquires;
        return std::make_unique<asset_storage::UpdateSession>(update_check, update_install, [this] {
            assert(update_owned && AudreamPowerManager::GetInstance().storage_leases == 1);
            update_owned = false; ++update_returns;
        });
    }
};
