#include "asset_update_client.h"
#include "http.h"
#include <algorithm>

namespace asset_update {
namespace {
struct CloseHttp {
    Http& http;
    ~CloseHttp() { http.Close(); }
};
using Status = firmware_release::CheckStatus;
using Stage = asset_storage::StageResult;
}

Client::Client(asset_storage::Store& store, CreateHttp create_http, std::function<int64_t()> now_ms)
    : store_(store), create_http_(std::move(create_http)), now_ms_(std::move(now_ms)) {}

CheckResult Client::Check(const std::string& manifest_url, const Cancelled& cancelled) {
    const auto fail = [](Status status) { return CheckResult{status, {}, {}}; };
    if (!cancelled || !create_http_ || !store_.CanStage() || !firmware_release::IsHttpsUrl(manifest_url)) return {};
    if (cancelled()) return fail(Status::kCancelled);
    auto http = create_http_();
    if (!http) return fail(Status::kTransportError);
    CloseHttp close{*http};
    http->SetTimeout(15000);
    if (!http->Open("GET", manifest_url)) return fail(cancelled() ? Status::kCancelled : Status::kTransportError);
    if (cancelled()) return fail(Status::kCancelled);
    const auto status = http->GetStatusCode();
    if (cancelled()) return fail(Status::kCancelled);
    if (status != 200 && status != 204) return fail(Status::kTransportError);
    const auto length = http->GetBodyLength();
    if (status == 204 && length == 0) {
        char byte;
        if (http->Read(&byte, 1) != 0) return fail(Status::kTransportError);
        return fail(cancelled() ? Status::kCancelled : Status::kNoUpdate);
    }
    if (status != 200) return fail(Status::kTransportError);
    if (!length || length > firmware_release::kMaximumManifestBytes) return fail(Status::kInvalidRelease);
    std::string body;
    body.reserve(length);
    char chunk[512];
    while (true) {
        if (cancelled()) return fail(Status::kCancelled);
        const auto received = http->Read(chunk, sizeof(chunk));
        if (cancelled()) return fail(Status::kCancelled);
        if (received < 0) return fail(Status::kTransportError);
        if (received > static_cast<int>(sizeof(chunk)) || static_cast<size_t>(received) > length - body.size())
            return fail(Status::kInvalidRelease);
        if (!received) break;
        body.append(chunk, received);
    }
    if (body.size() != length) return fail(Status::kInvalidRelease);
    auto verified = store_.VerifyCandidate(body);
    if (cancelled()) return fail(Status::kCancelled);
    if (!verified) return fail(Status::kInvalidRelease);
    return {Status::kAvailable, std::move(verified), std::move(body)};
}

Stage Client::Install(const std::string& envelope, const Progress& progress,
                      const Cancelled& cancelled, const Cancelled& before_publish) {
    if (!cancelled || !before_publish || !create_http_ || !now_ms_) return Stage::kRejected;
    if (cancelled()) return Stage::kCancelled;
    const auto release = store_.VerifyCandidate(envelope);
    if (!release) return Stage::kRejected;
    auto http = create_http_();
    if (!http) return Stage::kIoError;
    CloseHttp close{*http};
    http->SetTimeout(15000);
    if (!http->Open("GET", release->Url())) return cancelled() ? Stage::kCancelled : Stage::kIoError;
    if (cancelled()) return Stage::kCancelled;
    const auto status = http->GetStatusCode();
    if (cancelled()) return Stage::kCancelled;
    if (status != 200) return Stage::kIoError;
    if (http->GetBodyLength() != release->Size()) return Stage::kRejected;
    size_t received_bytes = 0, recent_bytes = 0;
    auto last_ms = now_ms_();
    const auto read = [&](void* bytes, size_t capacity) {
        if (cancelled()) return -1;
        const auto received = http->Read(static_cast<char*>(bytes), capacity);
        if (cancelled()) return -1;
        if (received < 0 || static_cast<size_t>(received) > capacity ||
            static_cast<size_t>(received) > release->Size() - received_bytes) return -1;
        received_bytes += received; recent_bytes += received;
        const auto now = now_ms_();
        if (last_ms < 0 || now < last_ms) { last_ms = now; recent_bytes = 0; }
        if (now >= 0 && last_ms >= 0 && (now - last_ms >= 1000 || received == 0)) {
            const size_t speed = now > last_ms ? static_cast<uint64_t>(recent_bytes) * 1000 / (now - last_ms) : 0;
            if (progress) progress(static_cast<int>(std::min<size_t>(99, received_bytes * 100 / release->Size())), speed);
            last_ms = now; recent_bytes = 0;
        }
        return received;
    };
    const auto result = store_.Stage(envelope, release->Size(), read, cancelled, before_publish);
    // Once publication starts, cancellation cannot undo a staged/uncertain journal.
    if (result != Stage::kStaged && result != Stage::kPersistenceUncertain && cancelled()) return Stage::kCancelled;
    return result;
}
}  // namespace asset_update
