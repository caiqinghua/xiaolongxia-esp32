#include "watch/watch_voice_cloud.h"
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <cJSON.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

using namespace watch_voice;
static const Endpoints kEndpoints{"https://api.example", "wss://voice.example/v1/device"};
static const std::string kDevice = "11111111-2222-3333-4444-555555555555";
static const std::string kCredential = "adv_" + std::string(43, 'a');
static const std::string kSettings = R"({"enabled":true,"activation_mode":"push_to_talk","language":"zh-CN","voice":"default","save_transcript":false,"power_mode":"watch_standby","revision":2,"applied_revision":0,"apply_error":null,"wake_word_enabled":false})";
static std::string Boot(const std::string& settings = kSettings) {
    return "{\"settings\":" + settings + R"(,"protocol_version":1,"access_token":"aaa.bbb.ccc","expires_in":3600,"websocket_url":"wss://voice.example/v1/device"})";
}
static std::string Replace(std::string body, const std::string& from, const std::string& to) {
    const auto pos = body.find(from);
    assert(pos != std::string::npos);
    body.replace(pos, from.size(), to);
    return body;
}

struct esp_http_client {};
static esp_http_client handle;
static bool live = false, init_ok = true, header_ok = true, open_ok = true, complete = true, cancelled = false;
static int code = 200, read_limit = 17, opens = 0, cleanups = 0, read_calls = 0, declared_write = 0;
static int64_t now_ms = 1000, advertised = 0, elapsed_per_read = 0;
static std::string response, url, written;
static size_t offset = 0;
static esp_http_client_method_t method;
static std::map<std::string, std::string> headers;

int64_t esp_timer_get_time() { return now_ms * 1000; }
esp_err_t esp_crt_bundle_attach(void*) { return ESP_OK; }
void esp_log_level_set(const char* tag, int level) { assert(std::string(tag) == "HTTP_CLIENT" && level == ESP_LOG_WARN); }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
    assert(!live && config->disable_auto_redirect && config->transport_type == HTTP_TRANSPORT_OVER_SSL);
    assert(config->crt_bundle_attach == esp_crt_bundle_attach && config->buffer_size == 1024);
    if (!init_ok) return nullptr;
    live = true;
    url = config->url;
    method = config->method;
    return &handle;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) {
    assert(live && client == &handle);
    live = false;
    ++cleanups;
    return ESP_OK;
}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t, int timeout) {
    assert(timeout > 0 && timeout <= 5000);
    return ESP_OK;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char* key, const char* value) {
    headers[key] = value;
    return header_ok ? ESP_OK : ESP_FAIL;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t, int length) {
    ++opens;
    declared_write = length;
    return open_ok ? ESP_OK : ESP_FAIL;
}
int esp_http_client_write(esp_http_client_handle_t, const char* data, int length) {
    const int count = std::min(3, length);
    written.append(data, count);
    return count;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t) { return advertised; }
int esp_http_client_get_status_code(esp_http_client_handle_t) { return code; }
int esp_http_client_read(esp_http_client_handle_t, char* buffer, int capacity) {
    ++read_calls;
    now_ms += elapsed_per_read;
    if (read_limit < 0) return -1;
    const size_t count = std::min({response.size() - offset, static_cast<size_t>(capacity), static_cast<size_t>(read_limit)});
    std::memcpy(buffer, response.data() + offset, count);
    offset += count;
    return static_cast<int>(count);
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t) { return complete && offset == response.size(); }

static void Reset(std::string body = Boot()) {
    assert(!live);
    init_ok = header_ok = open_ok = complete = true;
    cancelled = false;
    code = 200;
    read_limit = 17;
    opens = cleanups = read_calls = declared_write = 0;
    now_ms = 1000;
    advertised = elapsed_per_read = 0;
    response = std::move(body);
    url.clear(); written.clear(); headers.clear(); offset = 0;
}
static Result Fetch(Bootstrap& result, bool clock = true) {
    return Client(kEndpoints).FetchBootstrap(kDevice, kCredential, 1, clock, [] { return cancelled; }, result);
}

static void TestStrictModels() {
    Bootstrap result;
    assert(DecodeBootstrap(Boot(), kEndpoints, 1, 1000, 2000, result));
    assert(result.settings.revision == 2 && result.settings.SupportsManualRuntime());
    assert(result.Usable(3595999) && !result.Usable(3596000));
    for (const auto& invalid : {
        Replace(Boot(), "\"revision\":2", "\"revision\":2,\"revision\":3"),
        Replace(Boot(), "\"revision\":2", "\"revision\":2.5"),
        Replace(Boot(), "\"revision\":2", "\"revision\":9007199254740992"),
        Replace(Boot(), "\"enabled\":true", "\"enabled\":1"),
        Replace(Boot(), "\"applied_revision\":0", "\"applied_revision\":3"),
        Replace(Boot(), "\"apply_error\":null", "\"apply_error\":\"arbitrary\""),
        Replace(Boot(), "aaa.bbb.ccc", "aaa..ccc"),
        Replace(Boot(), "aaa.bbb.ccc", "aaa.bbb.ccc\\r\\nInjected: true"),
        Replace(Boot(), "aaa.bbb.ccc", "aaa.bbb.ccc\\u0000suffix"),
        Replace(Boot(), "wss://voice.example/v1/device", "wss://attacker.example/v1/device"),
        Replace(Boot(), "\"protocol_version\":1", "\"protocol_version\":2"),
        Replace(Boot(), "\"expires_in\":3600", "\"expires_in\":7200"),
        Replace(Boot(), "\"save_transcript\":false", "\"save_transcript\":null"),
        Replace(Boot(), "\"wake_word_enabled\":false", "\"wake_word_enabled\":true"),
        Replace(Boot(), "\"language\":\"zh-CN\"", "\"language\":\"en--US\""),
        Boot() + " trailing", std::string(kMaximumResponseBytes + 1, ' '),
        std::string(1000, '[') + "0" + std::string(1000, ']'),
        std::string(R"({"a":{"b":{"c":{"d":{"e":1}}}}})")
    }) {
        result.access_token = "previous";
        assert(!DecodeBootstrap(invalid, kEndpoints, 1, 1000, 2000, result));
        assert(result.access_token.empty() && !result.settings.enabled);
    }
    assert(!DecodeBootstrap(Boot(), kEndpoints, 1, 2000, 1000, result));
    assert(!DecodeBootstrap(Boot(), kEndpoints, 1, 0, 3600000, result));
    const auto disabled_settings = Replace(kSettings, "\"enabled\":true", "\"enabled\":false");
    const auto disabled = "{\"settings\":" + disabled_settings + R"(,"protocol_version":1,"access_token":null,"expires_in":0,"websocket_url":null})";
    assert(DecodeBootstrap(disabled, kEndpoints, 1, 1000, 2000, result));
    assert(!result.settings.enabled && !result.Usable(2000));
    assert(!DecodeBootstrap(Replace(disabled, "\"access_token\":null", "\"access_token\":\"aaa.bbb.ccc\""), kEndpoints, 1, 1000, 2000, result));
    for (const auto* bad : {"http://api.example", "https://api.example/", "https://user@api.example", "https://api.example?x=1", "https://api.example#fragment", "https://api.example:0", "https://api.example\\evil"}) {
        assert(!(Endpoints{bad, kEndpoints.websocket_url}).Valid());
    }
    assert(ValidDeviceId(kDevice) && !ValidDeviceId("../voice"));
    assert(ValidCredential(kCredential) && !ValidCredential(kCredential + "\n"));
    assert(EncodeAcknowledgement(2) == R"({"revision":2})");
    assert(EncodeAcknowledgement(2, "unsupported_voice") == R"({"revision":2,"error":"unsupported_voice"})");
    assert(EncodeAcknowledgement(0).empty() && EncodeAcknowledgement(2, "server body").empty());
}

static void TestHttpAndCleanup() {
    Bootstrap result;
    Reset();
    assert(Fetch(result) == Result::kOk && !live && cleanups == 1);
    assert(method == HTTP_METHOD_POST && url == kEndpoints.api_origin + "/v1/devices/" + kDevice + "/voice/bootstrap");
    assert(headers["Authorization"] == "Device " + kCredential);
    assert(written == R"({"protocol_version":1})" && written.size() == static_cast<size_t>(declared_write));
    for (const auto& pair : {std::pair<int, Result>{401, Result::kUnauthorized}, {403, Result::kUnauthorized},
                            {409, Result::kConflict}, {302, Result::kServer}, {500, Result::kServer}}) {
        Reset(); code = pair.first;
        assert(Fetch(result) == pair.second && !live && cleanups == 1 && read_calls == 0 && result.access_token.empty());
    }
    Reset();
    assert(Fetch(result, false) == Result::kClockRequired && opens == 0 && !live);
    Reset(); cancelled = true;
    assert(Fetch(result) == Result::kCancelled && opens == 0 && !live);
    Reset(); init_ok = false;
    assert(Fetch(result) == Result::kTransport && !live && cleanups == 0);
    Reset(); header_ok = false;
    assert(Fetch(result) == Result::kTransport && !live && cleanups == 1 && opens == 0);
    Reset(); open_ok = false;
    assert(Fetch(result) == Result::kTransport && !live && cleanups == 1);
    Reset(); read_limit = -1;
    assert(Fetch(result) == Result::kTransport && !live && cleanups == 1);
    Reset(); complete = false;
    assert(Fetch(result) == Result::kTransport && !live && result.access_token.empty());
    Reset(); advertised = kMaximumResponseBytes + 1;
    assert(Fetch(result) == Result::kInvalidResponse && read_calls == 0 && !live);
    Reset(std::string(kMaximumResponseBytes + 1, 'x')); read_limit = 1024;
    assert(Fetch(result) == Result::kInvalidResponse && offset == kMaximumResponseBytes + 1 && !live);
    Reset(); elapsed_per_read = 5000;
    assert(Fetch(result) == Result::kTimeout && !live && result.access_token.empty());
    Reset();
    unsigned polls = 0;
    assert(Client(kEndpoints).FetchBootstrap(kDevice, kCredential, 1, true, [&] { return ++polls > 8; }, result) == Result::kCancelled);
    assert(!live && result.access_token.empty());
}

static void TestSettingsAndAck() {
    Settings result;
    Reset(kSettings);
    Client client(kEndpoints);
    assert(client.FetchSettings(kDevice, kCredential, true, {}, result) == Result::kOk);
    assert(method == HTTP_METHOD_GET && written.empty() && url.find("/voice-settings") != std::string::npos);
    Reset(Replace(kSettings, "\"applied_revision\":0", "\"applied_revision\":2"));
    assert(client.Acknowledge(kDevice, kCredential, 2, {}, true, {}, result) == Result::kOk);
    assert(written == R"({"revision":2})" && result.applied_revision == 2);
    Reset(kSettings);
    assert(client.Acknowledge(kDevice, kCredential, 2, {}, true, {}, result) == Result::kInvalidResponse);
    assert(result.revision == 0);
    Reset(Replace(kSettings, "\"apply_error\":null", "\"apply_error\":\"application_failed\""));
    assert(client.Acknowledge(kDevice, kCredential, 2, "application_failed", true, {}, result) == Result::kOk);
    assert(result.applied_revision == 0 && result.apply_error == "application_failed");
    Reset(Replace(kSettings, "\"apply_error\":null", "\"apply_error\":\"local_disabled\""));
    assert(client.Acknowledge(kDevice, kCredential, 2, "local_disabled", true, {}, result) == Result::kOk);
    assert(result.applied_revision == 0 && result.apply_error == "local_disabled");
    assert(written == R"({"revision":2,"error":"local_disabled"})");
}

static void TestPowerModeMutation() {
    Client client(kEndpoints);
    Settings result;
    for (const auto* mode : {"watch_standby", "battery_saver", "always_available"}) {
        auto settings = Replace(kSettings, "\"revision\":2", "\"revision\":3");
        settings = Replace(settings, "watch_standby", mode);
        if (std::string(mode) == "always_available") {
            settings = Replace(settings, "push_to_talk", "wake_word");
            settings = Replace(settings, "\"wake_word_enabled\":false", "\"wake_word_enabled\":true");
        }
        Reset(settings);
        assert(client.UpdatePowerMode(kDevice, kCredential, 2, mode, true, {}, result) == Result::kOk);
        assert(method == HTTP_METHOD_POST && url == kEndpoints.api_origin + "/v1/devices/" + kDevice + "/voice-settings/power-mode");
        assert(written == "{\"revision\":2,\"power_mode\":\"" + std::string(mode) + "\"}");
        assert(result.revision == 3 && result.applied_revision == 0 && !live && cleanups == 1);
    }
    for (const auto revision : {0ULL, 9007199254740991ULL, UINT64_MAX}) {
        Reset();
        assert(client.UpdatePowerMode(kDevice, kCredential, revision, "watch_standby", true, {}, result) == Result::kInvalidRequest);
        assert(opens == 0 && result.revision == 0);
    }
    Reset();
    assert(client.UpdatePowerMode(kDevice, kCredential, 2, "unknown", true, {}, result) == Result::kInvalidRequest);
    assert(opens == 0);
    for (const auto& bad : {Replace(kSettings, "\"revision\":2", "\"revision\":3"),
                            Replace(kSettings, "watch_standby", "battery_saver"),
                            Replace(kSettings, "\"applied_revision\":0", "\"applied_revision\":2"),
                            Replace(kSettings, "\"apply_error\":null", "\"apply_error\":\"local_disabled\"")}) {
        Reset(bad);
        assert(client.UpdatePowerMode(kDevice, kCredential, 1, "watch_standby", true, {}, result) == Result::kInvalidResponse);
        assert(result.revision == 0 && !live);
    }
    Reset(Replace(kSettings, "watch_standby", "always_available"));
    assert(client.UpdatePowerMode(kDevice, kCredential, 1, "always_available", true, {}, result) == Result::kInvalidResponse);
    assert(result.revision == 0);
    Reset(kSettings); code = 409;
    assert(client.UpdatePowerMode(kDevice, kCredential, 1, "watch_standby", true, {}, result) == Result::kConflict);
    assert(result.revision == 0 && opens == 1 && !live);
    Reset(kSettings);
    assert(client.UpdatePowerMode(kDevice, kCredential, 1, "watch_standby", true,
        [] { return offset == response.size(); }, result) == Result::kCancelled);
    assert(result.revision == 0 && !live);
}

static void TestPairingCredentialVerification() {
    const std::string user = "aaaaaaaa-2222-3333-4444-555555555555";
    const std::string challenge = "bbbbbbbb-2222-3333-4444-555555555555";
    const std::string body = "{\"device_id\":\"" + kDevice + "\",\"user_id\":\"" + user +
        "\",\"challenge_id\":\"" + challenge + "\",\"credential_version\":1,\"status\":\"pending\"}";
    Client client(kEndpoints);
    PairingReceipt result;
    Reset(body);
    assert(client.VerifyPairing(kDevice, kCredential, user, challenge, true, {}, result) == Result::kOk);
    assert(result.credential_version == 1 && !result.active && !live);
    assert(url == kEndpoints.api_origin + "/v1/devices/" + kDevice + "/pairing/verify" && method == HTTP_METHOD_POST);
    assert(written == EncodePairingVerification(user, challenge));
    assert(headers["Authorization"] == "Device " + kCredential);
    for (const auto& invalid : {Replace(body, kDevice, user), Replace(body, user, kDevice),
             Replace(body, challenge, user), Replace(body, "pending", "unbound"),
             Replace(body, "\"credential_version\":1", "\"credential_version\":0"),
             Replace(body, "\"credential_version\":1", "\"credential_version\":true"),
             Replace(body, "\"credential_version\":1", "\"credential_version\":1,\"credential_version\":2"),
             Replace(body, "\"status\":\"pending\"", "\"status\":\"pending\",\"credential\":\"secret\"")}) {
        Reset(invalid);
        assert(client.VerifyPairing(kDevice, kCredential, user, challenge, true, {}, result) == Result::kInvalidResponse);
        assert(result.credential_version == 0 && !result.active);
    }
    Reset(Replace(body, "pending", "active"));
    assert(client.VerifyPairing(kDevice, kCredential, user, challenge, true, {}, result) == Result::kOk && result.active);
    Reset(body);
    assert(client.VerifyPairing(kDevice, kCredential, "bad", challenge, true, {}, result) == Result::kInvalidRequest && opens == 0);
    assert(client.VerifyPairing(kDevice, kCredential, user, challenge, false, {}, result) == Result::kClockRequired && opens == 0);
    assert(client.VerifyPairing(kDevice, kCredential, user, challenge, true, [] { return true; }, result) == Result::kCancelled);
    assert(opens == 0 && result.credential_version == 0);
    Reset(body); code = 403;
    assert(client.VerifyPairing(kDevice, kCredential, user, challenge, true, {}, result) == Result::kUnauthorized && !live);
}

static void TestApiTrace(const char* path) {
    std::ifstream input(path);
    std::stringstream bytes;
    bytes << input.rdbuf();
    assert(input && !bytes.str().empty());
    auto* root = cJSON_Parse(bytes.str().c_str());
    assert(root);
    const auto* receipt = cJSON_GetObjectItemCaseSensitive(root, "pairing_receipt");
    assert(cJSON_IsObject(receipt));
    char* pairing_body = cJSON_PrintUnformatted(receipt);
    assert(pairing_body);
    Reset(pairing_body); cJSON_free(pairing_body);
    const auto* device_id = cJSON_GetObjectItemCaseSensitive(receipt, "device_id");
    const auto* user_id = cJSON_GetObjectItemCaseSensitive(receipt, "user_id");
    const auto* challenge_id = cJSON_GetObjectItemCaseSensitive(receipt, "challenge_id");
    assert(cJSON_IsString(device_id) && cJSON_IsString(user_id) && cJSON_IsString(challenge_id));
    PairingReceipt verified;
    assert(Client(kEndpoints).VerifyPairing(device_id->valuestring, kCredential, user_id->valuestring,
        challenge_id->valuestring, true, {}, verified) == Result::kOk);
    assert(verified.credential_version == 1 && !verified.active);
    for (const char* key : {"disabled", "enabled"}) {
        char* body = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(root, key));
        assert(body);
        Reset(body);
        cJSON_free(body);
        Bootstrap result;
        assert(Fetch(result) == Result::kOk);
        assert(result.settings.enabled == (std::string(key) == "enabled"));
        assert(result.Usable(1000) == result.settings.enabled);
    }
    for (const char* key : {"settings", "applied", "failed"}) {
        char* body = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(root, key));
        assert(body);
        Settings settings;
        assert(DecodeSettings(body, settings));
        cJSON_free(body);
        if (std::string(key) == "applied") assert(settings.applied_revision == settings.revision);
        if (std::string(key) == "failed") assert(settings.apply_error == "application_failed");
    }
    for (const int version : {2, 3}) {
        char* body = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(root, ("enabled_v" + std::to_string(version)).c_str()));
        assert(body);
        Reset(body);
        cJSON_free(body);
        Bootstrap result;
        assert(Client(kEndpoints).FetchBootstrap(kDevice, kCredential, version, true, {}, result) == Result::kOk);
        assert(result.protocol_version == version && result.Usable(1000));
    }
    auto* power_changes = cJSON_GetObjectItemCaseSensitive(root, "power_changes");
    assert(cJSON_IsArray(power_changes) && cJSON_GetArraySize(power_changes) == 3);
    for (int index = 0; index < cJSON_GetArraySize(power_changes); ++index) {
        auto* change = cJSON_GetArrayItem(power_changes, index);
        auto* request = cJSON_GetObjectItemCaseSensitive(change, "request");
        auto* revision = cJSON_GetObjectItemCaseSensitive(request, "revision");
        auto* mode = cJSON_GetObjectItemCaseSensitive(request, "power_mode");
        char* body = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(change, "response"));
        assert(body && cJSON_IsNumber(revision) && cJSON_IsString(mode));
        Reset(body);
        cJSON_free(body);
        Settings result;
        assert(Client(kEndpoints).UpdatePowerMode(kDevice, kCredential, static_cast<uint64_t>(revision->valuedouble),
            mode->valuestring, true, {}, result) == Result::kOk);
        assert(!result.enabled && result.applied_revision < result.revision);
    }
    cJSON_Delete(root);
}

int main(int argc, char** argv) {
    TestPairingCredentialVerification();
    TestPowerModeMutation();
    Settings wake_settings;
    wake_settings.enabled = true;
    wake_settings.activation_mode = "wake_word";
    wake_settings.power_mode = "always_available";
    wake_settings.wake_word_enabled = true;
    assert(wake_settings.SupportsWakeRuntime() && !wake_settings.SupportsManualRuntime());
    wake_settings.save_transcript = true;
    assert(!wake_settings.SupportsWakeRuntime());
    wake_settings.save_transcript = false;
    wake_settings.wake_word_enabled = false;
    assert(!wake_settings.SupportsWakeRuntime());
    wake_settings.wake_word_enabled = true;
    wake_settings.enabled = false;
    assert(!wake_settings.SupportsWakeRuntime());
    wake_settings.enabled = true;
    wake_settings.power_mode = "battery_saver";
    assert(!wake_settings.SupportsWakeRuntime());
    TestStrictModels();
    TestHttpAndCleanup();
    TestSettingsAndAck();
    if (argc == 2) TestApiTrace(argv[1]);
    assert(!live);
    std::cout << "PASS: voice cloud strict parsing, scoped HTTP, cancellation, limits, acknowledgement" << std::endl;
}
