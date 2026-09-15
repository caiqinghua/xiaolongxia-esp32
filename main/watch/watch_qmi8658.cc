#include "watch_qmi8658.h"

#include <sensor/imu/qmi8658/SensorQMI8658_Reg.hpp>
#include <limits>
#include <memory>
#include <utility>

namespace {
// Some high-level SensorLib methods ignore a nested operation's return value.
// Latch every transport failure until the entire operation has been checked.
class CheckedComm final : public SensorCommBase {
public:
    CheckedComm(std::unique_ptr<SensorCommBase> inner, bool& failed)
        : inner_(std::move(inner)), failed_(failed) {}
    bool init() override { const bool ok = inner_->init(); failed_ |= !ok; return ok; }
    void deinit() override { inner_->deinit(); }
    int readRegister(uint8_t reg, uint8_t* data, size_t size) override {
        const int result = Check(inner_->readRegister(reg, data, size));
        if (result == 0 && pending_command_ && reg == QMI8658Regs::REG_STATUS_INT && size &&
            (data[0] & QMI8658Regs::MASK_INT_CTRL9_DONE)) command_done_ = true;
        return result;
    }
    int writeRegister(uint8_t reg, uint8_t* data, size_t size) override {
        if (reg == QMI8658Regs::REG_CTRL9 && size == 1) {
            if (data[0] == QMI8658Regs::CTRL_CMD_ACK) {
                // Upstream writeCommand can ACK after its first poll times out.
                // A cleared ACK bit alone is not proof the command completed.
                failed_ |= pending_command_ && !command_done_;
                pending_command_ = false;
            } else {
                pending_command_ = true;
                command_done_ = false;
            }
        }
        return Check(inner_->writeRegister(reg, data, size));
    }
    int readBuffer(uint8_t* data, size_t size) override {
        return Check(inner_->readBuffer(data, size));
    }
    int writeBuffer(uint8_t* data, size_t size) override {
        return Check(inner_->writeBuffer(data, size));
    }
    int writeThenRead(const uint8_t* out, size_t out_size, uint8_t* in, size_t in_size) override {
        return Check(inner_->writeThenRead(out, out_size, in, in_size));
    }
    void setParams(const CommParamsBase& params) override { inner_->setParams(params); }

private:
    int Check(int result) { failed_ |= result != 0; return result; }
    std::unique_ptr<SensorCommBase> inner_;
    bool& failed_;
    bool pending_command_ = false;
    bool command_done_ = false;
};
}  // namespace

void WatchQmi8658::afterCommReady() {
    comm = std::make_unique<CheckedComm>(std::move(comm), io_failed_);
}

bool WatchQmi8658::FinishBegin(bool succeeded) {
    ready_ = succeeded && !io_failed_;
    // Always attempt to leave both sensors and the pedometer powered down,
    // including when initialization was only partially successful.
    const bool stopped = Stop();
    ready_ = ready_ && stopped;
    return ready_;
}

#if defined(ESP_PLATFORM)
bool WatchQmi8658::Begin(i2c_master_bus_handle_t bus, uint8_t address) {
    if (ready_ || running_) return false;
    clearCommState();
    io_failed_ = false;
    return FinishBegin(SensorQMI8658::begin(bus, address));
}
#endif

bool WatchQmi8658::Begin(SensorCommCustom::CustomCallback io,
    SensorCommCustomHal::CustomHalCallback hal, uint8_t address) {
    if (!io || !hal || ready_ || running_) return false;
    clearCommState();
    io_failed_ = false;
    return FinishBegin(SensorQMI8658::begin(COMM_I2C, io, hal, address));
}

bool WatchQmi8658::ReadStepCount(uint32_t& count) {
    uint8_t bytes[3]{};
    if (readRegBuff(QMI8658Regs::REG_STEP_CNT_L, bytes, sizeof(bytes)) != 0) return false;
    count = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16);
    return true;
}

bool WatchQmi8658::Start() {
    if (!ready_ || running_ || generation_ == std::numeric_limits<uint64_t>::max()) return false;
    if (!Stop()) return false;
    io_failed_ = false;
    // Match SensorLib's pedometer runtime rate. Do not use its permissive
    // shake-test thresholds, and do not claim this 6-axis profile is low power.
    const bool configured = configAccel(AccelFullScaleRange::FS_4G, 112.0f) &&
        configGyro(GyroFullScaleRange::FS_1000_DPS, 112.0f) &&
        configPedometerDefault(112.0f) && enableAccel() && enableGyro() &&
        enablePedometer(IntPin::DISABLE) && resetStepCount();
    uint32_t counter = 0;
    const int enabled = readReg(QMI8658Regs::REG_CTRL7);
    const int pedometer = readReg(QMI8658Regs::REG_CTRL8);
    const bool verified = configured && enabled >= 0 && (enabled & 3) == 3 &&
        pedometer >= 0 && (pedometer & 0x10) && ReadStepCount(counter) && counter == 0 && !io_failed_;
    if (!verified) {
        Stop();
        return false;
    }
    ++generation_;
    running_ = true;
    return true;
}

bool WatchQmi8658::Stop() {
    io_failed_ = false;
    const bool pedometer = disablePedometer();
    const bool gyro = disableGyro();
    const bool accel = disableAccel();
    const int enabled = readReg(QMI8658Regs::REG_CTRL7);
    const int control = readReg(QMI8658Regs::REG_CTRL8);
    running_ = false;
    // Failure is visible to the power owner; it must not release I2C pins or
    // declare sleep preparation complete based only on Running() == false.
    return pedometer && gyro && accel && enabled >= 0 && !(enabled & 3) &&
        control >= 0 && !(control & 0x10) && !io_failed_;
}

bool WatchQmi8658::Read(WatchImuReading& reading) {
    if (!running_) return false;
    io_failed_ = false;
    WatchImuReading next;
    int16_t x = 0, y = 0, z = 0;
    if (!ReadStepCount(next.steps) || !readAccelRaw(x, y, z) || io_failed_) return false;
    next.generation = generation_;
    // FS_4G is fixed above; raw acceleration is a signed 16-bit sample.
    constexpr float scale = 4.0f / 32768.0f;
    next.x_g = x * scale;
    next.y_g = y * scale;
    next.z_g = z * scale;
    reading = next;
    return true;
}
