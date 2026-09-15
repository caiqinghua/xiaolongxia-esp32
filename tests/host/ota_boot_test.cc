#include "ota.h"
#include "ota_boot_health.h"
#include <esp_ota_ops.h>
#include <cassert>
#include <iostream>

namespace {
struct Fixture {
    esp_partition_t partition{32768};
    bool missing = false;
    esp_ota_img_states_t state = ESP_OTA_IMG_PENDING_VERIFY;
    int queries = 0, confirms = 0;
    int fail_query = 0;
    bool write_ok = true, read_back_valid = true;
} f;

void Check(bool result, int confirms, int queries) {
    assert(Ota::MarkCurrentVersionValid() == result);
    assert(f.confirms == confirms && f.queries == queries);
}
}

const char* esp_err_to_name(esp_err_t) { return "test error"; }
const esp_partition_t* esp_ota_get_running_partition() { return f.missing ? nullptr : &f.partition; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t* partition, esp_ota_img_states_t* state) {
    assert(partition == &f.partition);
    if (++f.queries == f.fail_query) return ESP_FAIL;
    *state = f.state;
    return ESP_OK;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback() {
    ++f.confirms;
    assert(f.state == ESP_OTA_IMG_PENDING_VERIFY);
    if (!f.write_ok) return ESP_FAIL;
    if (f.read_back_valid) f.state = ESP_OTA_IMG_VALID;
    return ESP_OK;
}

int main() {
    f = {}; f.missing = true; Check(false, 0, 0);
    f = {}; f.partition.type = 1; Check(false, 0, 0);
    f = {}; f.partition.subtype = ESP_PARTITION_SUBTYPE_APP_FACTORY; Check(true, 0, 0);
    f = {}; f.fail_query = 1; Check(false, 0, 1);
    for (auto state : {ESP_OTA_IMG_NEW, ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED, ESP_OTA_IMG_UNDEFINED}) {
        f = {}; f.state = state; Check(false, 0, 1);
    }
    f = {}; f.state = ESP_OTA_IMG_VALID; Check(true, 0, 1);
    f = {}; f.write_ok = false; Check(false, 1, 1);
    f = {}; f.read_back_valid = false; Check(false, 1, 2);
    f = {}; f.fail_query = 2; Check(false, 1, 2);
    f = {}; Check(true, 1, 2);
    assert(Ota::MarkCurrentVersionValid());
    assert(f.confirms == 1);

    OtaBootHealth disabled;
    assert(!disabled.Observe(60000, true));
    for (bool initialized : {false, true}) {
        OtaBootHealth health;
        health.Begin(initialized, 0);
        for (int tick = 1; tick < 30; ++tick) assert(!health.Observe(tick * 1000, true));
        assert(health.Observe(30000, true) == initialized);
        assert(!health.Observe(31000, true));
        if (initialized) {
            f = {};
            health.Complete(Ota::MarkCurrentVersionValid());
            assert(health.Confirmed() && f.confirms == 1);
        } else {
            health.Begin(true, 31000);
            assert(!health.Observe(61000, true));
        }
    }
    for (bool error : {false, true}) {
        OtaBootHealth health;
        health.Begin(true, 1000);
        for (int tick = 2; tick <= 20; ++tick) assert(!health.Observe(tick * 1000, true));
        const int reset = error ? 21000 : 24000;
        assert(!health.Observe(reset, !error));
        for (int tick = 1; tick < 30; ++tick) assert(!health.Observe(reset + tick * 1000, true));
        assert(health.Observe(reset + 30000, true));
        health.Complete(false);
        assert(!health.Confirmed() && !health.Observe(reset + 31000, true));
    }
    for (int64_t invalid : {-1, 999}) {
        OtaBootHealth health;
        health.Begin(true, 1000);
        assert(!health.Observe(invalid, true));
        for (int tick = 1; tick <= 60; ++tick) assert(!health.Observe(1000 + tick * 1000, true));
    }
    OtaBootHealth same_time;
    same_time.Begin(true, 0);
    for (int i = 0; i < 100; ++i) assert(!same_time.Observe(0, true));
    assert(!same_time.Observe(30000, true));
    for (int tick = 1; tick < 30; ++tick) assert(!same_time.Observe(30000 + tick * 1000, true));
    assert(same_time.Observe(60000, true));
    same_time.Complete(true);
    same_time.Complete(false);
    assert(same_time.Confirmed());

    std::cout << "PASS: boot confirmation, native state failures and offline startup observation\n";
}
