#include "asset_mapping.h"
#include "audio/assistant_model_bundle.h"
#include "signed_release_json.h"
#include <new>
#include <cstdlib>

namespace asset_storage {
bool IsValidConfig(const BootConfig& config) {
    using namespace signed_release_json;
    std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> key{};
    const auto& compatibility = config.compatibility;
    return sodium_init() >= 0 && Model(compatibility.model) && Token(compatibility.sr_abi, 63) &&
        Token(compatibility.wake_model, 31) && compatibility.firmware_sequence &&
        compatibility.firmware_sequence <= 2147483647 &&
        Hex(config.public_key_hex, key.data(), key.size()) && !sodium_is_zero(key.data(), key.size());
}
Mapping::~Mapping() {
    if (update_owned_) std::abort();
    if (!pinned_) Close();
}
void Mapping::Close() {
    if (mapped_) esp_partition_munmap(handle_);
    mapped_ = false; valid_ = false; data_ = nullptr; size_ = 0;
}

bool Mapping::BeginTrialHealth(bool models_ready, int64_t now_ms, uint32_t wake_feeds) {
    if (update_owned_) return false;
    if (!TrialPending()) return false;
    if (!models_ready || !valid_ || !pinned_ || Faulted() || now_ms < 0) {
        health_started_ = true; health_active_ = false; failed_ = true;
        return false;
    }
    if (!health_started_) {
        if (!asset_release::IsModelOnlyBundle(data_, size_)) return false;
        health_started_ = true; health_active_ = true;
        last_wake_feeds_ = wake_feeds;
        health_.Begin(true, now_ms);
    }
    return health_active_;
}

bool Mapping::PollTrialHealth(int64_t now_ms, bool healthy, uint32_t wake_feeds) {
    if (update_owned_) return health_active_;
    if (!health_active_) return false;
    if (!valid_ || !pinned_ || Faulted() || !TrialPending()) {
        health_active_ = false;
        return false;
    }
    const bool progressing = wake_feeds != last_wake_feeds_;
    last_wake_feeds_ = wake_feeds;
    if (health_.Observe(now_ms, healthy && progressing)) {
        const bool confirmed = store_->ConfirmTrial(selected_.sequence);
        health_.Complete(confirmed);
        health_active_ = false;
        if (confirmed) selected_ = store_->Selected();
        else failed_ = true;
        RefreshStoreState();
    }
    if (health_.Failed()) { health_active_ = false; failed_ = true; }
    return health_active_;
}

bool Mapping::EndTrialHealth(bool failed) {
    if (update_owned_ || !TrialPending()) return false;
    health_active_ = false;
    health_started_ = false;
    health_ = {};
    if (failed) failed_ = true;
    return true;
}

bool Mapping::Open(const esp_partition_t* partition, const BootConfig* config) {
    if (attempted_) return valid_;
    attempted_ = true;
    if (!partition || partition->size < 12) { failed_ = true; return false; }
    std::array<unsigned char, 12> header{};
    if (esp_partition_read(partition, 0, header.data(), header.size()) != ESP_OK) { failed_ = true; return false; }
    const auto factory_length = audream::ReadBundleU32(header.data() + 8);
    if (factory_length > partition->size - header.size()) { failed_ = true; return false; }
    selected_.size = factory_length + header.size();
    if (config && IsValidConfig(*config)) {
        backend_ = Partition::Open(partition, storage_status_);
        if (backend_) {
            store_.reset(new(std::nothrow) Store(backend_->Geometry(), backend_->FactoryDigest(),
                config->compatibility, config->public_key_hex, backend_->MakeHooks()));
            if (!store_) { failed_ = true; return false; }
            selected_ = store_->Boot();
            RefreshStoreState();
        }
    } else if (config) {
        failed_ = true;
    }
    size_ = selected_.size;
    mapped_ = esp_partition_mmap(partition, selected_.offset, size_, ESP_PARTITION_MMAP_DATA, &data_, &handle_) == ESP_OK;
    if (!mapped_ || !audream::ValidateMappedAssetBundle(data_, size_)) {
        failed_ = true; Close(); return false;
    }
    if (selected_.slot && (!store_ || !store_->SelectedRelease() ||
        !asset_release::ValidateBundle(*store_->SelectedRelease(), data_, size_))) {
        failed_ = true; Close(); return false;
    }
    const auto* bytes = static_cast<const unsigned char*>(data_);
    const auto length = audream::ReadBundleU32(bytes + 8);
    if (length != size_ - 12) { failed_ = true; Close(); return false; }
    uint32_t checksum = 0;
    for (size_t i = 12; i < size_; ++i) checksum = (checksum + bytes[i]) & 0xffff;
    if (checksum != audream::ReadBundleU32(bytes + 4)) { failed_ = true; Close(); return false; }
    valid_ = true;
    if (selected_.slot) version_ = store_->SelectedRelease()->Version();
    return true;
}

void Mapping::RefreshStoreState() {
    can_stage_ = store_ && store_->CanStage();
    store_faulted_ = store_ && store_->Faulted();
}

std::unique_ptr<UpdateSession> Mapping::BeginUpdate(asset_update::Client::CreateHttp create_http,
                                                 std::function<int64_t()> now_ms) {
    if (!UpdatesReady() || !create_http || !now_ms) return {};
    std::unique_ptr<UpdateSession> session(new(std::nothrow) UpdateSession(*this, std::move(create_http), std::move(now_ms)));
    if (session) update_owned_ = true;
    return session;
}

void Mapping::ReturnUpdate() {
    if (!update_owned_) std::abort();
    RefreshStoreState();
    update_owned_ = false;
}

UpdateSession::UpdateSession(Mapping& mapping, asset_update::Client::CreateHttp create_http,
                             std::function<int64_t()> now_ms)
    : mapping_(mapping), client_(*mapping.store_, std::move(create_http), std::move(now_ms)) {}

UpdateSession::~UpdateSession() {
    if (active_.load()) std::abort();
    mapping_.ReturnUpdate();
}

asset_update::CheckResult UpdateSession::Check(const std::string& url, const asset_update::Client::Cancelled& cancelled) {
    if (active_.exchange(true)) return {firmware_release::CheckStatus::kTransportError, {}, {}};
    struct Finish { std::atomic<bool>& active; ~Finish() { active.store(false); } } finish{active_};
    return client_.Check(url, cancelled);
}

StageResult UpdateSession::Install(const std::string& envelope, const asset_update::Client::Progress& progress,
                                   const asset_update::Client::Cancelled& cancelled,
                                   const asset_update::Client::Cancelled& before_publish) {
    if (active_.exchange(true)) return StageResult::kRejected;
    struct Finish { std::atomic<bool>& active; ~Finish() { active.store(false); } } finish{active_};
    return client_.Install(envelope, progress, cancelled, before_publish);
}
}  // namespace asset_storage
