#include "watch_voice_pairing.h"
#include "watch_voice_cloud.h"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace watch_voice {
namespace {
struct Challenge { std::string device, user, id; uint64_t expiry = 0; };
bool Decode(std::string_view payload, Challenge& result) {
    if (payload.empty() || payload.size() > 1024) return false;
    // The server's challenge is a flat ASCII object. Bound recursion before parsing.
    bool quoted = false, escaped = false;
    unsigned depth = 0;
    for (const unsigned char ch : payload) {
        if (ch < 32 || ch > 126) return false;
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{') { if (++depth != 1) return false; }
        else if (ch == '}') { if (!depth) return false; --depth; }
        else if (ch == '[' || ch == ']') return false;
    }
    if (quoted || depth || payload.find("\\u0000") != std::string_view::npos) return false;
    const std::string terminated(payload);
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        cJSON_ParseWithLengthOpts(terminated.c_str(), terminated.size() + 1, nullptr, true), cJSON_Delete);
    if (!cJSON_IsObject(root.get())) return false;
    unsigned fields = 0;
    for (const auto* item = root->child; item; item = item->next) {
        if (!item->string || ++fields > 6) return false;
        for (const auto* prior = root->child; prior != item; prior = prior->next)
            if (std::strcmp(prior->string, item->string) == 0) return false;
    }
    const auto text = [&](const char* key) -> std::string {
        const auto* value = cJSON_GetObjectItemCaseSensitive(root.get(), key);
        return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
    };
    const auto* expiry = cJSON_GetObjectItemCaseSensitive(root.get(), "expires_at");
    const auto nonce = text("nonce");
    result.device = text("device_id"); result.user = text("user_id"); result.id = text("challenge_id");
    if (fields != 6 || text("domain") != "audream-pairing-v1" || !ValidDeviceId(result.device) ||
        !ValidDeviceId(result.user) || !ValidDeviceId(result.id) || nonce.size() != 43 ||
        !std::all_of(nonce.begin(), nonce.end(), [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'; }) ||
        !cJSON_IsNumber(expiry) || !std::isfinite(expiry->valuedouble) ||
        expiry->valuedouble < 1600000000 || expiry->valuedouble > 4102444800 ||
        std::floor(expiry->valuedouble) != expiry->valuedouble) return false;
    result.expiry = static_cast<uint64_t>(expiry->valuedouble);
    return true;
}
}

void PairingSession::Clear() {
    request_ = peer_ = deadline_ms_ = window_ms_ = 0;
    approved_ = false;
    identity_ = {};
    payload_.clear();
}
bool PairingSession::ValidTime(uint64_t now_ms) {
    if (now_ms < last_ms_ || !window_ms_ || now_ms >= window_ms_) { Clear(); return false; }
    last_ms_ = now_ms;
    return true;
}
bool PairingSession::OpenLocalWindow(uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    Clear();
    if (now_ms > std::numeric_limits<uint64_t>::max() - 120000 ||
        LoadDeviceIdentity(identity_) != IdentityStatus::kOk) return false;
    last_ms_ = now_ms;
    window_ms_ = now_ms + 120000;
    return true;
}
void PairingSession::Cancel() { std::lock_guard<std::mutex> lock(mutex_); Clear(); }
PairingOffer PairingSession::Offer(uint64_t peer, std::string_view payload, uint64_t unix_seconds, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ValidTime(now_ms) || !peer || request_ || sequence_ == std::numeric_limits<uint64_t>::max()) return {};
    Challenge challenge;
    if (!Decode(payload, challenge) || challenge.device != identity_.device_id || unix_seconds < 1600000000 ||
        challenge.expiry <= unix_seconds || challenge.expiry - unix_seconds > 120) return {};
    request_ = ++sequence_;
    peer_ = peer;
    payload_ = payload;
    deadline_ms_ = now_ms + std::min(window_ms_ - now_ms, (challenge.expiry - unix_seconds) * 1000);
    return {request_, challenge.device, challenge.user, challenge.id};
}
bool PairingSession::ApproveLocally(uint64_t request, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ValidTime(now_ms) || !request || request != request_ || now_ms >= deadline_ms_) return false;
    approved_ = true;
    return true;
}
bool PairingSession::TakeProof(uint64_t request, uint64_t peer, uint64_t now_ms, std::array<uint8_t, 64>& proof) {
    proof.fill(0);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ValidTime(now_ms) || !approved_ || !request || request != request_ || !peer || peer != peer_ ||
        now_ms >= deadline_ms_) return false;
    const bool result = DevicePairingSigner::Sign(identity_, payload_, proof);
    Clear();
    return result;
}
void PairingSession::Disconnect(uint64_t peer) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (peer && peer == peer_) Clear();
}

}  // namespace watch_voice
