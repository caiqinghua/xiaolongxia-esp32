#pragma once

#include "asset_partition.h"
#include "ota_boot_health.h"
#include "asset_update_client.h"
#include <atomic>

namespace asset_storage {
struct BootConfig {
    asset_release::Compatibility compatibility;
    std::string public_key_hex;
};
bool IsValidConfig(const BootConfig& config);
class UpdateSession;

// A mapping and its update journal belong to one boot. Never replace borrowed data.
class Mapping {
public:
    Mapping() = default;
    ~Mapping();
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    bool Open(const esp_partition_t* partition, const BootConfig* config = nullptr);
    const void* Data() const { return valid_ ? data_ : nullptr; }
    size_t Size() const { return valid_ ? size_ : 0; }
    const std::string& Version() const { return version_; }
    const Selection& Selected() const { return selected_; }
    bool UpdatesReady() const { return valid_ && store_ && can_stage_ && !update_owned_ && !Faulted(); }
    bool TrialPending() const { return selected_.trial; }
    bool Faulted() const { return failed_ || store_faulted_; }
    PartitionStatus StorageStatus() const { return storage_status_; }
    // The SDK retains model pointers for the rest of this boot.
    void Pin() { if (valid_) pinned_ = true; }
    // Application owner only. Poll requires a storage lease before healthy=true.
    bool BeginTrialHealth(bool models_ready, int64_t now_ms, uint32_t wake_feeds);
    bool PollTrialHealth(int64_t now_ms, bool healthy, uint32_t wake_feeds);
    std::unique_ptr<UpdateSession> BeginUpdate(asset_update::Client::CreateHttp create_http,
                                             std::function<int64_t()> now_ms);
private:
    friend class UpdateSession;
    void ReturnUpdate();
    void RefreshStoreState();
    void Close();
    std::unique_ptr<Partition> backend_;
    std::unique_ptr<Store> store_;
    const void* data_ = nullptr;
    size_t size_ = 0;
    esp_partition_mmap_handle_t handle_ = 0;
    Selection selected_;
    std::string version_;
    PartitionStatus storage_status_ = PartitionStatus::kUnsupported;
    bool attempted_ = false, mapped_ = false, valid_ = false, pinned_ = false, failed_ = false;
    OtaBootHealth health_;
    uint32_t last_wake_feeds_ = 0;
    bool health_started_ = false, health_active_ = false;
    bool update_owned_ = false, can_stage_ = false, store_faulted_ = false;
};

// Acquired by the Application task before worker startup and destroyed only
// after join. The Mapping must outlive the session and all borrowed model data.
class UpdateSession {
public:
    ~UpdateSession();
    UpdateSession(const UpdateSession&) = delete;
    UpdateSession& operator=(const UpdateSession&) = delete;
    asset_update::CheckResult Check(const std::string& url, const asset_update::Client::Cancelled& cancelled);
    StageResult Install(const std::string& envelope, const asset_update::Client::Progress& progress,
                        const asset_update::Client::Cancelled& cancelled,
                        const asset_update::Client::Cancelled& before_publish);
private:
    friend class Mapping;
    UpdateSession(Mapping& mapping, asset_update::Client::CreateHttp create_http, std::function<int64_t()> now_ms);
    Mapping& mapping_;
    asset_update::Client client_;
    std::atomic<bool> active_{false};
};
}  // namespace asset_storage
