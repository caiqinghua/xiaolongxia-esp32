#pragma once
#include <atomic>

namespace HostPlaybackCodec {
enum class Fault { kNone, kOpen, kDecode, kConsumption, kFormat, kSize, kReset };
extern std::atomic<int> decodes;
extern std::atomic<Fault> fault;
extern std::atomic<bool> fail_close;
extern std::atomic<int> handles;
}
