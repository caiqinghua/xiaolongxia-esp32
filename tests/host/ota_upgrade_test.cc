#include "ota.h"
#include "firmware_release.h"
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace {
struct Fixture {
    esp_partition_t partition{32768};
    std::vector<char> body, written;
    size_t declared = 0, offset = 0, fragment = 1000;
    int reads = 0, fail_read = -1, fail_write = -1, writes = 0;
    int begins = 0, ends = 0, aborts = 0, selections = 0, closes = 0, allocations = 0;
    int status = 200;
    bool no_partition = false, no_network = false, no_http = false, no_memory = false;
    bool open = true, begin_ok = true, end_ok = true, select_ok = true, live_handle = false;
    bool impossible_read = false;
    int64_t time = 0;
} f;

void Reset(size_t size = 9000) {
    assert(!f.live_handle && f.allocations == 0);
    f = Fixture{};
    f.body.resize(size);
    for (size_t i = 0; i < size; ++i) f.body[i] = static_cast<char>(i * 37 + 5);
    f.declared = size;
}

class Response : public Http {
public:
    void SetTimeout(int) override {}
    void SetHeader(const std::string&, const std::string&) override {}
    void SetContent(std::string&&) override {}
    void SetKeepAlive(bool) override {}
    bool Open(const std::string& method, const std::string& url) override {
        assert(method == "GET" && url == "https://updates.example/firmware.bin");
        return f.open;
    }
    void Close() override { ++f.closes; }
    int Read(char* data, size_t capacity) override {
        assert(capacity && capacity <= 4096);
        if (f.reads++ == f.fail_read) return -1;
        if (f.impossible_read) return static_cast<int>(capacity + 1);
        const auto count = std::min({capacity, f.fragment, f.body.size() - f.offset});
        if (count) std::memcpy(data, f.body.data() + f.offset, count);
        f.offset += count;
        return static_cast<int>(count);
    }
    int Write(const char*, size_t) override { assert(false); return -1; }
    int GetStatusCode() override { return f.status; }
    std::string GetResponseHeader(const std::string&) const override { return {}; }
    size_t GetBodyLength() override { return f.declared; }
    std::string ReadAll() override { assert(false); return {}; }
    int GetLastError() override { return 0; }
};
bool Run() {
    int previous = -1;
    const bool result = Ota::Upgrade("https://updates.example/firmware.bin", [&](int progress, size_t) {
        assert(progress >= previous && progress <= 100);
        if (!f.selections) assert(progress < 100);
        if (progress == 100) assert(f.selections == 1 && f.select_ok && f.ends == 1 && f.end_ok);
        previous = progress;
    });
    assert(!f.live_handle && f.allocations == 0);
    assert(f.closes == (f.no_partition || f.no_network || f.no_http ? 0 : 1));
    assert((previous == 100) == result);
    return result;
}
}

std::unique_ptr<Http> OtaTestNetwork::CreateHttp(int) {
    return f.no_http ? nullptr : std::make_unique<Response>();
}
OtaTestNetwork* Board::GetNetwork() {
    static OtaTestNetwork network;
    return f.no_network ? nullptr : &network;
}
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) {
    return f.no_partition ? nullptr : &f.partition;
}
esp_err_t esp_ota_begin(const esp_partition_t* partition, size_t size, esp_ota_handle_t* handle) {
    assert(partition == &f.partition && size == OTA_WITH_SEQUENTIAL_WRITES && !f.live_handle);
    ++f.begins;
    *handle = 42;
    f.live_handle = f.begin_ok;
    return f.begin_ok ? ESP_OK : ESP_FAIL;
}
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void* bytes, size_t size) {
    assert(f.live_handle && handle == 42 && size && size <= 4096);
    if (f.writes++ == f.fail_write) return ESP_FAIL;
    const char* data = static_cast<const char*>(bytes);
    f.written.insert(f.written.end(), data, data + size);
    assert(f.written.size() <= f.declared && f.written.size() <= f.partition.size);
    return ESP_OK;
}
esp_err_t esp_ota_abort(esp_ota_handle_t handle) {
    assert(f.live_handle && handle == 42);
    f.live_handle = false;
    ++f.aborts;
    return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t handle) {
    assert(f.live_handle && handle == 42 && f.written == f.body && f.written.size() == f.declared);
    f.live_handle = false;
    ++f.ends;
    return f.end_ok ? ESP_OK : ESP_FAIL;
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* partition) {
    assert(partition == &f.partition && !f.live_handle && f.ends == 1 && f.end_ok);
    ++f.selections;
    return f.select_ok ? ESP_OK : ESP_FAIL;
}
void* heap_caps_malloc(size_t size, unsigned) {
    assert(size == 4096);
    if (f.no_memory) return nullptr;
    ++f.allocations;
    return std::malloc(size);
}
void heap_caps_free(void* memory) { assert(memory && f.allocations == 1); --f.allocations; std::free(memory); }
int64_t esp_timer_get_time() { return f.time += 150000; }

int main() {
    assert(!Ota::UpgradeSigned("https://updates.example/release.json", {}));
    assert(!Ota::UpgradeSignedEnvelope("{}", {}));
    assert(Ota::CheckSignedRelease().status == firmware_release::CheckStatus::kNotConfigured);
    constexpr size_t minimum = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    for (size_t size : {minimum, size_t{4095}, size_t{4096}, size_t{4097}, size_t{32768}}) {
        for (size_t fragment : {size_t{1}, size_t{7}, size_t{1000}, size_t{4096}, size_t{7000}}) {
            Reset(size); f.fragment = fragment;
            assert(Run() && f.begins == 1 && f.ends == 1 && !f.aborts && f.written == f.body);
        }
    }
    for (int failure = 0; failure < 12; ++failure) {
        Reset();
        if (failure == 0) f.no_partition = true;
        if (failure == 1) f.no_network = true;
        if (failure == 2) f.no_http = true;
        if (failure == 3) f.open = false;
        if (failure == 4) f.status = 302;
        if (failure == 5) f.status = 404;
        if (failure == 6) f.declared = 0;
        if (failure == 7) f.declared = minimum - 1;
        if (failure == 8) f.declared = f.partition.size + 1;
        if (failure == 9) f.declared = std::numeric_limits<size_t>::max();
        if (failure == 10) f.no_memory = true;
        if (failure == 11) f.impossible_read = true;
        assert(!Run() && !f.begins && !f.ends && !f.selections);
    }
    for (size_t actual : {size_t{0}, minimum - 1, size_t{4096}, size_t{8999}, size_t{9001}, size_t{12000}}) {
        Reset(actual); f.declared = 9000;
        assert(!Run() && !f.ends && !f.selections);
        assert(f.aborts == f.begins);
    }
    for (int read = 0; read < 13; ++read) {
        Reset(); f.fail_read = read;
        const bool result = Run();
        if (!result) assert(!f.ends && !f.selections && f.aborts == f.begins);
    }
    for (int write = 0; write < 3; ++write) {
        Reset(); f.fail_write = write;
        assert(!Run() && f.aborts == 1 && !f.ends && !f.selections);
    }
    Reset(); f.begin_ok = false;
    assert(!Run() && f.begins == 1 && !f.aborts && !f.ends);
    Reset(); f.end_ok = false;
    assert(!Run() && f.ends == 1 && !f.aborts && !f.selections);
    Reset(); f.select_ok = false;
    assert(!Run() && f.ends == 1 && f.selections == 1 && !f.aborts);
    std::cout << "PASS: original OTA entrypoint, fragmented streaming, exact lengths, handle cleanup and boot-selection failures\n";
}
