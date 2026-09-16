#include "asset_mapping.h"
#include "audio/assistant_model_bundle.h"
#include "signed_release_json.h"
#include <esp_log.h>
#include <spi_flash_mmap.h>
#include <cstring>
#include <new>
#include <cstdlib>

namespace asset_storage {
namespace {
constexpr char kTag[] = "AssetMapping";
}

bool IsValidConfig(const BootConfig& config) {
    using namespace signed_release_json;
    std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> key{};
    const auto& compatibility = config.compatibility;
    return sodium_init() >= 0 && Model(compatibility.model) && Token(compatibility.sr_abi, 63) &&
        Token(compatibility.wake_model, 31) && compatibility.firmware_sequence &&
        compatibility.firmware_sequence <= 2147483647 &&
        config.factory_firmware_min <= config.factory_firmware_max &&
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
    if (!partition || partition->size < 12) {
        ESP_LOGE(kTag, "Assets partition is missing or too small");
        failed_ = true;
        return false;
    }
    std::array<unsigned char, 12> header{};
    const esp_err_t read_result = esp_partition_read(partition, 0, header.data(), header.size());
    if (read_result != ESP_OK) {
        ESP_LOGE(kTag, "Failed to read assets header: %s", esp_err_to_name(read_result));
        failed_ = true;
        return false;
    }
    const auto factory_length = audream::ReadBundleU32(header.data() + 8);
    if (factory_length > partition->size - header.size()) {
        ESP_LOGE(kTag, "Assets factory length is invalid: length=%u partition=%u",
                 static_cast<unsigned>(factory_length), static_cast<unsigned>(partition->size));
        failed_ = true;
        return false;
    }
    selected_.size = factory_length + header.size();
    if (config && IsValidConfig(*config)) {
        factory_firmware_min_ = config->factory_firmware_min;
        factory_firmware_max_ = config->factory_firmware_max;
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
    const int free_pages = spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA);
    const esp_err_t map_result = esp_partition_mmap(
        partition, selected_.offset, size_, ESP_PARTITION_MMAP_DATA, &data_, &handle_);
    mapped_ = map_result == ESP_OK;
    if (!mapped_) {
        ESP_LOGE(kTag, "Failed to map assets: %s offset=%u bytes=%u free_pages=%d",
                 esp_err_to_name(map_result), static_cast<unsigned>(selected_.offset),
                 static_cast<unsigned>(size_), free_pages);
        failed_ = true;
        Close();
        return false;
    }
    if (!audream::ValidateMappedAssetBundle(data_, size_)) {
        const auto* bytes = static_cast<const unsigned char*>(data_);
        const size_t files = size_ >= 4 ? audream::ReadBundleU32(bytes) : 0;
        const size_t length = size_ >= 12 ? audream::ReadBundleU32(bytes + 8) : 0;
        ESP_LOGE(kTag,
                 "Mapped assets bundle structure is invalid: offset=%u bytes=%u files=%u length=%u",
                 static_cast<unsigned>(selected_.offset), static_cast<unsigned>(size_),
                 static_cast<unsigned>(files), static_cast<unsigned>(length));
        if (size_ >= 12 && files && files <= length / 44 && length <= size_ - 12) {
            const size_t payload_size = length - files * 44;
            for (size_t i = 0; i < files; ++i) {
                const auto* entry = bytes + 12 + i * 44;
                const size_t asset_size = audream::ReadBundleU32(entry + 32);
                const size_t asset_offset = audream::ReadBundleU32(entry + 36);
                const bool name_valid = entry[0] && std::memchr(entry, 0, 32);
                const bool range_valid = asset_offset <= payload_size && payload_size - asset_offset >= 2 &&
                    asset_size <= payload_size - asset_offset - 2;
                const auto* payload = range_valid ? bytes + 12 + files * 44 + asset_offset : nullptr;
                const bool magic_valid = payload && payload[0] == 'Z' && payload[1] == 'Z';
                if (!name_valid || !range_valid || !magic_valid) {
                    ESP_LOGE(kTag,
                             "Invalid asset entry: index=%u name_valid=%d size=%u asset_offset=%u "
                             "payload_size=%u magic=%02x%02x",
                             static_cast<unsigned>(i), name_valid, static_cast<unsigned>(asset_size),
                             static_cast<unsigned>(asset_offset), static_cast<unsigned>(payload_size),
                             payload ? payload[0] : 0, payload ? payload[1] : 0);
                    break;
                }
            }
        }
        failed_ = true;
        Close();
        return false;
    }
    if (selected_.slot && (!store_ || !store_->SelectedRelease() ||
        !asset_release::ValidateBundle(*store_->SelectedRelease(), data_, size_))) {
        ESP_LOGE(kTag, "Selected asset release validation failed: slot=%u",
                 static_cast<unsigned>(selected_.slot));
        failed_ = true;
        Close();
        return false;
    }
    const auto* bytes = static_cast<const unsigned char*>(data_);
    const auto length = audream::ReadBundleU32(bytes + 8);
    if (length != size_ - 12) {
        ESP_LOGE(kTag, "Mapped assets length mismatch: header=%u mapped=%u",
                 static_cast<unsigned>(length), static_cast<unsigned>(size_));
        failed_ = true;
        Close();
        return false;
    }
    uint32_t checksum = 0;
    for (size_t i = 12; i < size_; ++i) checksum = (checksum + bytes[i]) & 0xffff;
    const auto stored_checksum = audream::ReadBundleU32(bytes + 4);
    if (checksum != stored_checksum) {
        ESP_LOGE(kTag, "Mapped assets checksum mismatch: calculated=0x%04x stored=0x%04x",
                 static_cast<unsigned>(checksum), static_cast<unsigned>(stored_checksum));
        failed_ = true;
        Close();
        return false;
    }
    valid_ = true;
    if (selected_.slot) version_ = store_->SelectedRelease()->Version();
    return true;
}

bool Mapping::SupportsFirmware(uint32_t sequence) const {
    if (!valid_ || failed_ || store_faulted_ || selected_.trial) return false;
    if (!selected_.slot) {
        return sequence >= factory_firmware_min_ && sequence <= factory_firmware_max_;
    }
    const auto* release = store_ ? store_->SelectedRelease() : nullptr;
    return release && sequence >= release->MinimumFirmware() && sequence <= release->MaximumFirmware();
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
