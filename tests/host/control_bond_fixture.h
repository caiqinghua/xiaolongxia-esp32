#pragma once
#include "esp_gap_ble_api.h"
#include <functional>
#include <vector>

namespace bond_fixture {
extern std::vector<esp_ble_bond_dev_t> bonds;
extern std::function<void()> on_list;
extern std::function<void()> on_commit;
extern bool fail_commit;
extern unsigned handles;
void Reset(const uint8_t* peer);
}
