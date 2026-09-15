#include "watch_activity.h"

#include <limits>

namespace {
void Write32(WatchActivityRecord& record, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) record[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}

uint32_t Read32(const uint8_t* data, size_t offset) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) value |= uint32_t(data[offset + i]) << (8 * i);
    return value;
}

bool Valid(const WatchActivityCheckpoint& checkpoint) {
    if (checkpoint.count > checkpoint.days.size()) return false;
    for (size_t i = 0; i < checkpoint.count; ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (checkpoint.days[i].day == checkpoint.days[j].day) return false;
        }
    }
    return true;
}
}  // namespace

std::optional<WatchActivityRecord> EncodeWatchActivityCheckpoint(const WatchActivityCheckpoint& checkpoint) {
    if (!Valid(checkpoint)) return std::nullopt;
    WatchActivityRecord record{};
    record[0] = 1;
    record[1] = static_cast<uint8_t>(checkpoint.count);
    record[2] = checkpoint.incomplete ? 1 : 0;
    Write32(record, 4, checkpoint.undated_steps);
    for (size_t i = 0; i < checkpoint.count; ++i) {
        Write32(record, 8 + i * 8, static_cast<uint32_t>(checkpoint.days[i].day));
        Write32(record, 12 + i * 8, checkpoint.days[i].steps);
    }
    return record;
}

bool DecodeWatchActivityCheckpoint(const uint8_t* data, size_t size, WatchActivityCheckpoint& checkpoint) {
    if (!data || size != WatchActivityRecord{}.size() || data[0] != 1 ||
        data[1] > WatchActivityCheckpoint::kDays || data[2] > 1 || data[3] != 0) return false;
    WatchActivityCheckpoint decoded;
    decoded.count = data[1];
    decoded.incomplete = data[2] != 0;
    decoded.undated_steps = Read32(data, 4);
    for (size_t i = 0; i < decoded.count; ++i) {
        const uint32_t value = Read32(data, 8 + i * 8);
        const int64_t signed_day = value <= uint32_t(std::numeric_limits<int32_t>::max())
            ? int64_t(value) : int64_t(value) - (int64_t(1) << 32);
        decoded.days[i] = {static_cast<int32_t>(signed_day), Read32(data, 12 + i * 8)};
    }
    for (size_t i = 8 + decoded.count * 8; i < size; ++i) {
        if (data[i]) return false;
    }
    if (!Valid(decoded)) return false;
    checkpoint = decoded;
    return true;
}
