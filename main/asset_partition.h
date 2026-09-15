#pragma once

#include "asset_storage.h"
#include <esp_partition.h>
#include <memory>

namespace asset_storage {
enum class PartitionStatus { kReady, kUnsupported, kInvalidFactory, kInsufficientSpace, kIoError };

// Must outlive the Store using its hooks. It owns no long-lived model mappings.
class Partition {
public:
    static std::unique_ptr<Partition> Open(const esp_partition_t* partition, PartitionStatus& status);
    Partition(const Partition&) = delete;
    Partition& operator=(const Partition&) = delete;
    const Layout& Geometry() const { return layout_; }
    const Digest& FactoryDigest() const { return factory_digest_; }
    Hooks MakeHooks();
private:
    Partition(const esp_partition_t* partition, Layout layout, Digest factory_digest);
    bool Range(uint32_t offset, size_t size, bool writing) const;
    bool Validate(uint32_t offset, const asset_release::Verified& release) const;
    static LoadResult LoadJournal(JournalBytes& bytes);
    static bool SaveJournal(const JournalBytes& bytes);

    const esp_partition_t* partition_;
    Layout layout_;
    Digest factory_digest_;
};
}  // namespace asset_storage
