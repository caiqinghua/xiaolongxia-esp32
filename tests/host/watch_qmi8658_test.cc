#include "watch/watch_qmi8658.h"

#include <sensor/imu/qmi8658/SensorQMI8658_Reg.hpp>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>

namespace {
using namespace QMI8658Regs;
std::array<uint8_t, 256> registers;
size_t operations;
size_t fail_operation;
uint32_t now_ms;
bool suppress_done;
bool ignore_disable;

void ResetFixture() {
    registers.fill(0);
    registers[REG_WHO_AM_I] = QMI8658_WHO_AM_I_VAL;
    operations = fail_operation = now_ms = 0;
    suppress_done = ignore_disable = false;
}

bool Io(uint8_t address, uint8_t reg, uint8_t* data, size_t size, bool with_register, bool write) {
    assert(address == QMI8658_L_SLAVE_ADDRESS);
    assert(with_register && size && reg + size <= registers.size());
    if (++operations == fail_operation) return false;
    if (!write) {
        for (size_t i = 0; i < size; ++i) data[i] = registers[reg + i];
        return true;
    }
    if (ignore_disable && reg == REG_CTRL7 && !(data[0] & 3)) return true;
    for (size_t i = 0; i < size; ++i) registers[reg + i] = data[i];
    if (reg == REG_RESET && data[0] == QMI8658_RESET_VAL) {
        registers[REG_RST_RESULT] = MASK_RST_RESULT;
    }
    if (reg == REG_CTRL9) {
        registers[REG_STATUS_INT] = data[0] == CTRL_CMD_ACK || suppress_done ? 0 : MASK_INT_CTRL9_DONE;
        if (data[0] == CTRL_CMD_RESET_PEDOMETER) {
            registers[REG_STEP_CNT_L] = registers[REG_STEP_CNT_M] = registers[REG_STEP_CNT_H] = 0;
        }
    }
    return true;
}

uint32_t Hal(SensorCommCustomHal::Operation op, void* param, void*) {
    switch (op) {
        case SensorCommCustomHal::OP_MILLIS: return now_ms++;
        case SensorCommCustomHal::OP_DELAY: now_ms += reinterpret_cast<uintptr_t>(param); break;
        case SensorCommCustomHal::OP_DELAYMICROSECONDS: break;
        default: assert(false);  // No board GPIO or wake IRQ may be configured here.
    }
    return 0;
}

void SetRaw(uint8_t reg, int16_t value) {
    registers[reg] = static_cast<uint16_t>(value) & 0xff;
    registers[reg + 1] = static_cast<uint16_t>(value) >> 8;
}

void TestStartReadStop() {
    ResetFixture();
    WatchQmi8658 sensor;
    WatchImuReading reading{99, 99, 99, 99, 99};
    assert(!sensor.Read(reading) && !sensor.Start());
    assert(sensor.Begin(Io, Hal));
    assert(!sensor.Running() && sensor.Generation() == 0);
    assert(!(registers[REG_CTRL7] & 3));
    assert(sensor.Start());
    assert(sensor.Running() && sensor.Generation() == 1);
    assert(!sensor.Start());
    assert(!sensor.Begin(Io, Hal));
    assert(sensor.Read(reading));
    assert(reading.steps == 0 && reading.generation == 1);
    registers[REG_STEP_CNT_L] = 0x56;
    registers[REG_STEP_CNT_M] = 0x34;
    registers[REG_STEP_CNT_H] = 0x12;
    SetRaw(REG_AX_L, 8192);
    SetRaw(REG_AY_L, -4096);
    SetRaw(REG_AZ_L, 2048);
    assert(sensor.Read(reading));
    assert(reading.steps == 0x123456);
    assert(std::abs(reading.x_g - 1.0f) < 0.001f);
    assert(std::abs(reading.y_g + 0.5f) < 0.001f);
    assert(std::abs(reading.z_g - 0.25f) < 0.001f);
    assert(sensor.Stop());
    assert(!sensor.Running() && !sensor.Read(reading));
    assert(!(registers[REG_CTRL7] & 3) && !(registers[REG_CTRL8] & 0x10));
    assert(sensor.Start());
    assert(sensor.Read(reading) && reading.generation == 2 && reading.steps == 0);
    assert(sensor.Stop());
}

void TestReadFailurePreservesOutput() {
    for (size_t offset : {1, 2}) {
        ResetFixture();
        WatchQmi8658 sensor;
        assert(sensor.Begin(Io, Hal) && sensor.Start());
        WatchImuReading reading{42, 123, 1, 2, 3};
        fail_operation = operations + offset;
        assert(!sensor.Read(reading));
        assert(reading.generation == 42 && reading.steps == 123 && reading.z_g == 3);
        assert(sensor.Read(reading));
        assert(reading.steps == 0 && reading.generation == 1);
        assert(sensor.Stop());
    }
}

void TestEveryInitializationFailure() {
    ResetFixture();
    size_t count = 0;
    {
        WatchQmi8658 sensor;
        assert(sensor.Begin(Io, Hal));
        count = operations;
    }
    for (size_t i = 1; i <= count; ++i) {
        ResetFixture();
        fail_operation = i;
        WatchQmi8658 sensor;
        if (sensor.Begin(Io, Hal)) {
            std::fprintf(stderr, "Ignored initialization I/O failure at operation %zu\n", i);
            assert(false);
        }
        assert(!sensor.Start() && !sensor.Running());
    }
}

void TestEveryStartFailure() {
    ResetFixture();
    size_t count = 0;
    {
        WatchQmi8658 sensor;
        assert(sensor.Begin(Io, Hal));
        const size_t before = operations;
        assert(sensor.Start());
        count = operations - before;
        assert(sensor.Stop());
    }
    for (size_t i = 1; i <= count; ++i) {
        ResetFixture();
        WatchQmi8658 sensor;
        assert(sensor.Begin(Io, Hal));
        fail_operation = operations + i;
        if (sensor.Start()) {
            std::fprintf(stderr, "Ignored pedometer I/O failure at operation %zu\n", i);
            assert(false);
        }
        assert(!sensor.Running() && sensor.Generation() == 0);
        assert(sensor.Stop());
    }
}

void TestUnacknowledgedCommandAndPowerDown() {
    ResetFixture();
    WatchQmi8658 sensor;
    assert(sensor.Begin(Io, Hal));
    suppress_done = true;
    assert(!sensor.Start());
    assert(!sensor.Running() && sensor.Generation() == 0);
    suppress_done = false;
    assert(sensor.Start());
    ignore_disable = true;
    assert(!sensor.Stop());
    assert(!sensor.Running());
    assert(registers[REG_CTRL7] & 3);
    ignore_disable = false;
    assert(sensor.Stop());
    assert(!(registers[REG_CTRL7] & 3));
}
}  // namespace

int main() {
    TestStartReadStop();
    TestReadFailurePreservesOutput();
    TestEveryInitializationFailure();
    TestEveryStartFailure();
    TestUnacknowledgedCommandAndPowerDown();
    std::puts("QMI8658 adapter: 5 test groups passed against pinned SensorLib");
}
