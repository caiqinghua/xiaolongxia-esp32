#include "asset_partition.h"
#include "audio/assistant_model_bundle.h"
#include <nvs.h>
#include <sodium.h>
#include <cstring>
#include <new>

namespace asset_storage {
namespace {
constexpr char kNamespace[] = "asset_update";
constexpr char kKey[] = "journal_v1";
struct Mapping {
    const void* bytes = nullptr;
    esp_partition_mmap_handle_t handle = 0;
    bool mapped = false;
    ~Mapping() { if (mapped) esp_partition_munmap(handle); }
    bool Open(const esp_partition_t* partition, uint32_t offset, size_t size) {
        mapped = esp_partition_mmap(partition, offset, size, ESP_PARTITION_MMAP_DATA, &bytes, &handle) == ESP_OK;
        return mapped && bytes;
    }
};
struct NvsHandle {
    nvs_handle_t value = 0;
    bool opened = false;
    ~NvsHandle() { if (opened) nvs_close(value); }
    esp_err_t Open(nvs_open_mode_t mode) {
        const auto result = nvs_open(kNamespace, mode, &value);
        opened = result == ESP_OK;
        return result;
    }
};
}  // namespace

Partition::Partition(const esp_partition_t* partition, Layout layout, Digest factory_digest)
    : partition_(partition), layout_(layout), factory_digest_(factory_digest) {}

std::unique_ptr<Partition> Partition::Open(const esp_partition_t* partition, PartitionStatus& status) {
    status = PartitionStatus::kUnsupported;
    if (!partition || partition->type != ESP_PARTITION_TYPE_DATA || std::strcmp(partition->label, "assets") ||
        partition->readonly || partition->erase_size != kSectorBytes ||
        partition->address % kMapAlignment || partition->size < 12 ||
        partition->size > asset_release::kMaximumBundleBytes || esp_partition_get_main_flash_sector_size() != kSectorBytes)
        return nullptr;
    status = PartitionStatus::kIoError;
    std::array<unsigned char, 12> header{};
    if (esp_partition_read(partition, 0, header.data(), header.size()) != ESP_OK) return nullptr;
    const uint32_t length = audream::ReadBundleU32(header.data() + 8);
    status = PartitionStatus::kInvalidFactory;
    if (length > partition->size - header.size()) return nullptr;
    const uint32_t factory_size = length + header.size();
    Mapping mapping;
    status = PartitionStatus::kIoError;
    if (!mapping.Open(partition, 0, factory_size)) return nullptr;
    status = PartitionStatus::kInvalidFactory;
    if (!audream::ValidateMappedAssetBundle(mapping.bytes, factory_size)) return nullptr;
    const auto* bytes = static_cast<const unsigned char*>(mapping.bytes);
    uint32_t checksum = 0;
    for (size_t i = 12; i < factory_size; ++i) checksum = (checksum + bytes[i]) & 0xffff;
    if (checksum != audream::ReadBundleU32(bytes + 4)) return nullptr;
    status = PartitionStatus::kInsufficientSpace;
    auto layout = Layout::Create(partition->size, factory_size);
    if (!layout) return nullptr;
    status = PartitionStatus::kIoError;
    Digest digest{};
    if (sodium_init() < 0 || crypto_hash_sha256(digest.data(), bytes, factory_size) != 0) return nullptr;
    std::unique_ptr<Partition> result(new(std::nothrow) Partition(partition, *layout, digest));
    if (result) status = PartitionStatus::kReady;
    return result;
}

bool Partition::Range(uint32_t offset, size_t size, bool writing) const {
    if (!size || offset > partition_->size || size > partition_->size - offset) return false;
    if (!writing) return true;
    for (unsigned slot = 1; slot <= 2; ++slot) {
        const auto base = layout_.Offset(slot);
        if (offset >= base && offset - base <= layout_.BankBytes() &&
            size <= layout_.BankBytes() - (offset - base)) return true;
    }
    return false;
}

bool Partition::Validate(uint32_t offset, const asset_release::Verified& release) const {
    if ((offset != layout_.Offset(1) && offset != layout_.Offset(2)) || release.Size() > layout_.Capacity() ||
        !Range(offset, release.Size(), false)) return false;
    Mapping mapping;
    return mapping.Open(partition_, offset, release.Size()) &&
        asset_release::ValidateBundle(release, mapping.bytes, release.Size());
}

LoadResult Partition::LoadJournal(JournalBytes& bytes) {
    NvsHandle handle;
    auto result = handle.Open(NVS_READONLY);
    if (result == ESP_ERR_NVS_NOT_FOUND) return LoadResult::kMissing;
    if (result != ESP_OK) return LoadResult::kError;
    size_t size = 0;
    result = nvs_get_blob(handle.value, kKey, nullptr, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND) return LoadResult::kMissing;
    if (result != ESP_OK || size != bytes.size()) return LoadResult::kError;
    result = nvs_get_blob(handle.value, kKey, bytes.data(), &size);
    return result == ESP_OK && size == bytes.size() ? LoadResult::kLoaded : LoadResult::kError;
}

bool Partition::SaveJournal(const JournalBytes& bytes) {
    if (!DecodeJournal(bytes)) return false;
    {
        NvsHandle handle;
        if (handle.Open(NVS_READWRITE) != ESP_OK || nvs_set_blob(handle.value, kKey, bytes.data(), bytes.size()) != ESP_OK ||
            nvs_commit(handle.value) != ESP_OK) return false;
    }
    JournalBytes readback{};
    return LoadJournal(readback) == LoadResult::kLoaded && readback == bytes;
}

Hooks Partition::MakeHooks() {
    return {
        [this](uint32_t offset, void* bytes, size_t size) {
            return bytes && Range(offset, size, false) && esp_partition_read(partition_, offset, bytes, size) == ESP_OK;
        },
        [this](uint32_t offset, const void* bytes, size_t size) {
            return bytes && Range(offset, size, true) && esp_partition_write(partition_, offset, bytes, size) == ESP_OK;
        },
        [this](uint32_t offset, size_t size) {
            return offset % kSectorBytes == 0 && size % kSectorBytes == 0 && Range(offset, size, true) &&
                esp_partition_erase_range(partition_, offset, size) == ESP_OK;
        },
        LoadJournal, SaveJournal,
        [this](uint32_t offset, const asset_release::Verified& release) { return Validate(offset, release); }
    };
}
}  // namespace asset_storage
