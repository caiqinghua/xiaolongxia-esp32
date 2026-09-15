#pragma once
#include <cstdint>

namespace firmware_release {
struct ImageIdentity {
    char magic[8];
    uint32_t sequence;
    char model[96];
};
static_assert(sizeof(ImageIdentity) == 108);
const ImageIdentity* CurrentIdentity();
}  // namespace firmware_release
