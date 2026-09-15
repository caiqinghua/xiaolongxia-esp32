#pragma once

#include "asset_storage.h"
#include "firmware_release.h"
#include <memory>

class Http;

namespace asset_update {
struct CheckResult {
    firmware_release::CheckStatus status = firmware_release::CheckStatus::kNotConfigured;
    std::optional<asset_release::Verified> release;
    std::string envelope;
};

// The existing update worker owns this client and the Store exclusively until join.
// The owner supplies TLS clock/network/storage leases; this client never boots a
// second Store, changes active mappings, starts radios or restarts the device.
class Client {
public:
    using Cancelled = std::function<bool()>;
    using Progress = std::function<void(int, size_t)>;
    using CreateHttp = std::function<std::unique_ptr<Http>()>;
    Client(asset_storage::Store& store, CreateHttp create_http, std::function<int64_t()> now_ms);
    CheckResult Check(const std::string& manifest_url, const Cancelled& cancelled);
    asset_storage::StageResult Install(const std::string& envelope, const Progress& progress,
                                      const Cancelled& cancelled, const Cancelled& before_publish);
private:
    asset_storage::Store& store_;
    CreateHttp create_http_;
    std::function<int64_t()> now_ms_;
};
}  // namespace asset_update
