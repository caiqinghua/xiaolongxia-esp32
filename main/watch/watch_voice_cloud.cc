#include "watch_voice_cloud.h"

#include <cJSON.h>
#include <http_parser.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace watch_voice {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
constexpr uint64_t kMaximumRevision = 9007199254740991ULL;

bool AlphaNumeric(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
}

bool SafeUrl(std::string_view url, std::string_view scheme, bool origin) {
    if (url.empty() || url.size() > 512 || std::any_of(url.begin(), url.end(), [](unsigned char ch) {
            return ch <= 32 || ch >= 127 || ch == '\\';
        })) return false;
    http_parser_url parsed{};
    if (http_parser_parse_url(url.data(), url.size(), 0, &parsed) != 0) return false;
    const auto field = [&](http_parser_url_fields key) {
        return url.substr(parsed.field_data[key].off, parsed.field_data[key].len);
    };
    if (!(parsed.field_set & (1 << UF_SCHEMA)) || field(UF_SCHEMA) != scheme ||
        !(parsed.field_set & (1 << UF_HOST)) || field(UF_HOST).empty() ||
        (parsed.field_set & ((1 << UF_USERINFO) | (1 << UF_QUERY) | (1 << UF_FRAGMENT))) ||
        ((parsed.field_set & (1 << UF_PORT)) && parsed.port == 0)) return false;
    return origin ? !(parsed.field_set & (1 << UF_PATH)) : field(UF_PATH) == "/v1/device";
}

bool UniqueObjects(const cJSON* value, unsigned depth = 0) {
    if (depth > 4 || cJSON_IsArray(value)) return false;
    unsigned count = 0;
    for (auto* child = value->child; child; child = child->next) {
        if (++count > 32 || !child->string || !UniqueObjects(child, depth + 1)) return false;
        for (auto* prior = value->child; prior != child; prior = prior->next) {
            if (std::strcmp(prior->string, child->string) == 0) return false;
        }
    }
    return true;
}

Json Parse(std::string_view body) {
    if (body.empty() || body.size() > kMaximumResponseBytes || body.find('\0') != std::string_view::npos ||
        body.find("\\u0000") != std::string_view::npos) return Json(nullptr, cJSON_Delete);
    // Bound parser recursion and tree allocation before cJSON sees the body.
    unsigned depth = 0, fields = 0;
    bool quoted = false, escaped = false;
    for (const char ch : body) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '[' || ch == ']') return Json(nullptr, cJSON_Delete);
        else if (ch == '{') { if (++depth > 4) return Json(nullptr, cJSON_Delete); }
        else if (ch == '}') { if (depth == 0) return Json(nullptr, cJSON_Delete); --depth; }
        else if (ch == ':' && ++fields > 64) return Json(nullptr, cJSON_Delete);
    }
    if (depth != 0 || quoted) return Json(nullptr, cJSON_Delete);
    const std::string terminated(body);
    Json value(cJSON_ParseWithLengthOpts(terminated.c_str(), terminated.size() + 1, nullptr, true), cJSON_Delete);
    if (!cJSON_IsObject(value.get()) || !UniqueObjects(value.get())) value.reset();
    return value;
}

bool Number(const cJSON* parent, const char* key, uint64_t low, uint64_t high, uint64_t& result) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < static_cast<double>(low) || value->valuedouble > static_cast<double>(high) ||
        std::floor(value->valuedouble) != value->valuedouble) return false;
    result = static_cast<uint64_t>(value->valuedouble);
    return true;
}

bool Boolean(const cJSON* parent, const char* key, bool& result) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsBool(value)) return false;
    result = cJSON_IsTrue(value);
    return true;
}

bool Text(const cJSON* parent, const char* key, size_t maximum, std::string& result) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsString(value) || !value->valuestring) return false;
    result = value->valuestring;
    return !result.empty() && result.size() <= maximum;
}

bool ApplyError(std::string_view error) {
    return error.empty() || error == "unsupported_mode" || error == "unsupported_voice" ||
        error == "application_failed" || error == "local_disabled";
}

bool ReadSettings(const cJSON* root, Settings& value) {
    if (!cJSON_IsObject(root) || !Boolean(root, "enabled", value.enabled) ||
        !Boolean(root, "save_transcript", value.save_transcript) ||
        !Boolean(root, "wake_word_enabled", value.wake_word_enabled) ||
        !Text(root, "activation_mode", 32, value.activation_mode) || !Text(root, "power_mode", 32, value.power_mode) ||
        !Text(root, "language", 35, value.language) || !Text(root, "voice", 128, value.voice) ||
        !Number(root, "revision", 1, kMaximumRevision, value.revision) ||
        !Number(root, "applied_revision", 0, value.revision, value.applied_revision)) return false;
    if (value.activation_mode != "push_to_talk" && value.activation_mode != "wake_word") return false;
    if (value.power_mode != "watch_standby" && value.power_mode != "battery_saver" && value.power_mode != "always_available") return false;
    if ((value.activation_mode == "wake_word" && value.power_mode != "always_available") ||
        (value.wake_word_enabled && (!value.enabled || value.activation_mode != "wake_word"))) return false;
    if (!std::all_of(value.voice.begin(), value.voice.end(), [](char ch) { return AlphaNumeric(ch) || ch == '_' || ch == '-' || ch == '.'; })) return false;
    size_t start = 0;
    unsigned parts = 0;
    do {
        const auto end = value.language.find('-', start);
        const auto length = (end == std::string::npos ? value.language.size() : end) - start;
        if (++parts > 4 || length < (parts == 1 ? 2U : 1U) || length > 8) return false;
        for (size_t index = start; index < start + length; ++index) {
            const char ch = value.language[index];
            if (!AlphaNumeric(ch) || (parts == 1 && ch >= '0' && ch <= '9')) return false;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    } while (true);
    const auto* error = cJSON_GetObjectItemCaseSensitive(root, "apply_error");
    if (cJSON_IsNull(error)) return true;
    return Text(root, "apply_error", 32, value.apply_error) && ApplyError(value.apply_error);
}

bool Token(std::string_view value) {
    if (value.empty() || value.size() > 8192) return false;
    unsigned segments = 1;
    size_t length = 0;
    for (const char ch : value) {
        if (ch == '.') {
            if (length == 0 || ++segments > 3) return false;
            length = 0;
        } else {
            if (!AlphaNumeric(ch) && ch != '-' && ch != '_') return false;
            ++length;
        }
    }
    return segments == 3 && length > 0;
}
}  // namespace

bool Endpoints::Valid() const {
    return SafeUrl(api_origin, "https", true) && SafeUrl(websocket_url, "wss", false);
}

bool ValidAccessToken(std::string_view value) { return Token(value); }
bool ValidWebSocketUrl(std::string_view value) { return SafeUrl(value, "wss", false); }

bool ValidDeviceId(std::string_view value) {
    if (value.size() != 36) return false;
    for (size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (ch != '-') return false;
        } else if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    }
    return true;
}

bool ValidCredential(std::string_view value) {
    return value.size() >= 47 && value.size() <= 128 && value.substr(0, 4) == "adv_" &&
        std::all_of(value.begin() + 4, value.end(), [](char ch) { return AlphaNumeric(ch) || ch == '-' || ch == '_'; });
}

bool Settings::SupportsManualRuntime() const {
    return activation_mode == "push_to_talk" && !wake_word_enabled && !save_transcript;
}

bool Settings::SupportsWakeRuntime() const {
    return enabled && activation_mode == "wake_word" && wake_word_enabled &&
        power_mode == "always_available" && !save_transcript;
}

bool Bootstrap::Usable(uint64_t now_ms) const {
    return settings.enabled && !access_token.empty() && !websocket_url.empty() &&
        now_ms < expires_at_ms && expires_at_ms - now_ms > 5000;
}

bool DecodeSettings(std::string_view body, Settings& result) {
    result = {};
    auto root = Parse(body);
    Settings decoded;
    if (!root || !ReadSettings(root.get(), decoded)) return false;
    result = std::move(decoded);
    return true;
}

bool DecodeBootstrap(std::string_view body, const Endpoints& endpoints, int protocol_version,
                     uint64_t request_started_ms, uint64_t now_ms, Bootstrap& result) {
    result = {};
    if (!endpoints.Valid() || protocol_version < 1 || protocol_version > 3 || now_ms < request_started_ms) return false;
    auto root = Parse(body);
    Bootstrap decoded;
    uint64_t version = 0;
    uint64_t expiry = 0;
    if (!root || !ReadSettings(cJSON_GetObjectItemCaseSensitive(root.get(), "settings"), decoded.settings) ||
        !Number(root.get(), "protocol_version", protocol_version, protocol_version, version) ||
        !Number(root.get(), "expires_in", 0, 3600, expiry)) return false;
    decoded.protocol_version = static_cast<int>(version);
    if (!decoded.settings.enabled) {
        if (expiry != 0 || !cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(root.get(), "access_token")) ||
            !cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(root.get(), "websocket_url"))) return false;
    } else {
        if (expiry < 10 || request_started_ms > std::numeric_limits<uint64_t>::max() - expiry * 1000 ||
            !Text(root.get(), "access_token", 8192, decoded.access_token) || !Token(decoded.access_token) ||
            !Text(root.get(), "websocket_url", 512, decoded.websocket_url) || decoded.websocket_url != endpoints.websocket_url) return false;
        decoded.expires_at_ms = request_started_ms + expiry * 1000;
        if (!decoded.Usable(now_ms)) return false;
    }
    result = std::move(decoded);
    return true;
}

std::string EncodePowerModeRequest(uint64_t revision, std::string_view power_mode) {
    // A successful mutation increments the revision, which must remain exact in JSON.
    if (revision == 0 || revision >= kMaximumRevision ||
        (power_mode != "watch_standby" && power_mode != "battery_saver" &&
         power_mode != "always_available")) return {};
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddNumberToObject(root.get(), "revision", static_cast<double>(revision)) ||
        !cJSON_AddStringToObject(root.get(), "power_mode", std::string(power_mode).c_str())) return {};
    char* encoded = cJSON_PrintUnformatted(root.get());
    if (!encoded) return {};
    const std::string result(encoded);
    cJSON_free(encoded);
    return result;
}

std::string EncodeAcknowledgement(uint64_t revision, std::string_view error) {
    if (revision == 0 || revision > kMaximumRevision || !ApplyError(error)) return {};
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddNumberToObject(root.get(), "revision", static_cast<double>(revision)) ||
        (!error.empty() && !cJSON_AddStringToObject(root.get(), "error", std::string(error).c_str()))) return {};
    char* encoded = cJSON_PrintUnformatted(root.get());
    if (!encoded) return {};
    const std::string result(encoded);
    cJSON_free(encoded);
    return result;
}

std::string EncodePairingVerification(std::string_view user_id, std::string_view challenge_id) {
    if (!ValidDeviceId(user_id) || !ValidDeviceId(challenge_id)) return {};
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddStringToObject(root.get(), "user_id", std::string(user_id).c_str()) ||
        !cJSON_AddStringToObject(root.get(), "challenge_id", std::string(challenge_id).c_str())) return {};
    char* encoded = cJSON_PrintUnformatted(root.get());
    if (!encoded) return {};
    const std::string result(encoded);
    cJSON_free(encoded);
    return result;
}

bool DecodePairingReceipt(std::string_view body, std::string_view device_id, std::string_view user_id,
                          std::string_view challenge_id, PairingReceipt& result) {
    result = {};
    if (!ValidDeviceId(device_id) || !ValidDeviceId(user_id) || !ValidDeviceId(challenge_id)) return false;
    auto root = Parse(body);
    std::string device, user, challenge, status;
    PairingReceipt receipt;
    if (!root || cJSON_GetArraySize(root.get()) != 5 ||
        !Text(root.get(), "device_id", 36, device) || device != device_id ||
        !Text(root.get(), "user_id", 36, user) || user != user_id ||
        !Text(root.get(), "challenge_id", 36, challenge) || challenge != challenge_id ||
        !Text(root.get(), "status", 7, status) || (status != "pending" && status != "active") ||
        !Number(root.get(), "credential_version", 1, kMaximumRevision, receipt.credential_version)) return false;
    receipt.active = status == "active";
    result = receipt;
    return true;
}
}  // namespace watch_voice
