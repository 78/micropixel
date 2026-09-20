// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <cassert>
#include <cstdio>

#include "platform/sensors/polled_inertial_sensor_peripheral.hpp"
#include "platform/sensors/polled_vector_sensor_peripheral.hpp"

namespace {
using micropixel::device::SensorValues;
using micropixel::platform::buses::I2cExecutor;
using micropixel::platform::sensors::PolledVectorSensorPeripheral;

class Driver final : public micropixel::platform::drivers::VectorSensor {
   public:
    explicit Driver(I2cExecutor& executor) : executor_(executor) {}
    esp_err_t Initialize(i2c_master_bus_handle_t) override {
        assert(executor_.on_worker);
        ++initializations;
        return ESP_OK;
    }
    esp_err_t Configure(uint32_t interval) override {
        assert(executor_.on_worker);
        configured_interval = interval;
        return configure_status;
    }
    esp_err_t Suspend() override {
        assert(executor_.on_worker);
        ++suspends;
        return ESP_OK;
    }
    esp_err_t Read(float (&values)[3]) override {
        assert(executor_.on_worker);
        ++reads;
        if (timer_during_read != nullptr) {
            const size_t queued = executor_.jobs.size();
            FireSensorTimer(timer_during_read);
            assert(executor_.jobs.size() == queued);
        }
        values[0] = 1.0F;
        values[1] = -2.0F;
        values[2] = 3.0F;
        return read_status;
    }
    bool available() const override { return present; }
    uint32_t min_interval_us() const override { return 10000; }
    bool present{true};
    const char* timer_during_read{};
    uint32_t reads{}, suspends{}, initializations{}, configured_interval{};
    esp_err_t configure_status{ESP_OK}, read_status{ESP_OK};

   private:
    I2cExecutor& executor_;
};

void TestChannelsAndLifecycle() {
    I2cExecutor executor;
    Driver accel(executor), gyro(executor), mag2(executor), mag3(executor);
    // IDs are board-local: channel 2 may represent a magnetometer on Claw4.
    std::array<PolledVectorSensorPeripheral::Channel, 4> channels{{
        {1, MICROPIXEL_SENSOR_ACCELERATION, accel, "accel"},
        {7, MICROPIXEL_SENSOR_ANGULAR_VELOCITY, gyro, "gyro"},
        {2, MICROPIXEL_SENSOR_MAGNETIC_FIELD, mag2, "mag2"},
        {4, MICROPIXEL_SENSOR_MAGNETIC_FIELD, mag3, "mag3"},
    }};
    {
        PolledVectorSensorPeripheral peripheral(channels, "test");
        assert(peripheral.Initialize(executor) == ESP_OK);
        assert(peripheral.Initialize(executor) == ESP_ERR_INVALID_STATE);
        {
            PolledVectorSensorPeripheral duplicate_owner(channels, "test");
            assert(duplicate_owner.Initialize(executor) == ESP_ERR_INVALID_ARG);
        }
        micropixel_sensor_info_t info{};
        assert(peripheral.GetInfo(2, info) == MICROPIXEL_STATUS_OK);
        assert(info.kind == MICROPIXEL_SENSOR_MAGNETIC_FIELD && info.value_count == 3);
        assert(info.min_interval_us == 10000 && info.max_interval_us == 60000000);
        assert(info.placement == MICROPIXEL_SENSOR_PLACEMENT_BUILT_IN);
        assert(peripheral.GetInfo(99, info) == MICROPIXEL_STATUS_NOT_FOUND);
        SensorValues values{};
        assert(peripheral.Start(99, 20000) == MICROPIXEL_STATUS_NOT_FOUND);
        assert(peripheral.Read(1, values) == MICROPIXEL_STATUS_WOULD_BLOCK);
        for (auto id : {1U, 7U, 2U, 4U}) assert(peripheral.Start(id, 20000) == MICROPIXEL_STATUS_OK);
        FireSensorTimer("accel");
        FireSensorTimer("accel");
        FireSensorTimer("mag2");
        assert(executor.jobs.size() == 2 && accel.reads == 0 && mag2.reads == 0);
        assert(peripheral.Read(1, values) == MICROPIXEL_STATUS_WOULD_BLOCK);
        accel.timer_during_read = "accel";
        executor.Drain();
        assert(accel.reads == 1 && mag2.reads == 1);
        assert(peripheral.Read(1, values) == MICROPIXEL_STATUS_OK);
        assert(values.values[0] == 1 && values.values[1] == -2 && values.values[2] == 3);
        assert(values.timestamp_us == static_cast<uint64_t>(sensor_time_us));
        assert(peripheral.Read(7, values) == MICROPIXEL_STATUS_WOULD_BLOCK);
        executor.reject_post = true;
        FireSensorTimer("gyro");
        executor.reject_post = false;
        FireSensorTimer("gyro");
        executor.Drain();
        assert(gyro.reads == 1);  // A rejected Post must not leave pending set.
        gyro.read_status = ESP_FAIL;
        FireSensorTimer("gyro");
        executor.Drain();
        assert(peripheral.Read(7, values) == MICROPIXEL_STATUS_INTERNAL);
        FireSensorTimer("accel");
        executor.busy_low_invokes = 1;
        peripheral.Stop(1);
        assert(executor.busy_low_invokes == 0);
        assert(executor.jobs.empty() && accel.reads == 1 && accel.suspends == 1);
        assert(peripheral.Read(1, values) == MICROPIXEL_STATUS_WOULD_BLOCK);
        assert(values.timestamp_us == 0 && values.values[0] == 0);
        FireSensorTimer("mag2");
        assert(peripheral.Start(2, 40000) == MICROPIXEL_STATUS_OK);
        assert(executor.jobs.empty() && mag2.reads == 1 && mag2.configured_interval == 40000);
        assert(peripheral.Read(2, values) == MICROPIXEL_STATUS_WOULD_BLOCK);
        FireSensorTimer("mag3");  // Destruction must drain this before freeing timers/storage.
    }
    assert(executor.jobs.empty() && mag3.reads == 0);
    for (const auto& timer : sensor_timers) assert(!timer.allocated);
}

void TestFailures() {
    I2cExecutor executor;
    Driver driver(executor), absent(executor);
    absent.present = false;
    std::array<PolledVectorSensorPeripheral::Channel, 2> channels{{
        {1, MICROPIXEL_SENSOR_ACCELERATION, driver, "accel"},
        {2, MICROPIXEL_SENSOR_MAGNETIC_FIELD, absent, "absent"},
    }};
    PolledVectorSensorPeripheral peripheral(channels, "test");
    assert(peripheral.Initialize(executor) == ESP_OK);
    assert(peripheral.Start(2, 20000) == MICROPIXEL_STATUS_NOT_FOUND);
    driver.configure_status = ESP_FAIL;
    assert(peripheral.Start(1, 20000) == MICROPIXEL_STATUS_INTERNAL);
    driver.configure_status = ESP_OK;
    executor.reject_invoke = true;
    assert(peripheral.Start(1, 20000) == MICROPIXEL_STATUS_INTERNAL);
    executor.reject_invoke = false;
    fail_timer_start = true;
    assert(peripheral.Start(1, 20000) == MICROPIXEL_STATUS_INTERNAL);
    assert(driver.suspends == 1);
    fail_timer_start = false;
    assert(peripheral.Start(1, 20000) == MICROPIXEL_STATUS_OK);
    FireSensorTimer("accel");
    executor.Drain();
    SensorValues values{};
    assert(peripheral.Read(1, values) == MICROPIXEL_STATUS_OK);
}

void TestTimerFailureAndInvalidChannels() {
    I2cExecutor executor;
    Driver driver(executor);
    std::array<PolledVectorSensorPeripheral::Channel, 1> channel{{
        {1, MICROPIXEL_SENSOR_ACCELERATION, driver, "accel"},
    }};
    {
        PolledVectorSensorPeripheral peripheral(channel, "test");
        fail_timer_create = true;
        assert(peripheral.Initialize(executor) == ESP_OK);
        fail_timer_create = false;
        assert(peripheral.Start(1, 20000) == MICROPIXEL_STATUS_NOT_FOUND);
    }
    std::array<PolledVectorSensorPeripheral::Channel, 2> duplicates{{
        {1, MICROPIXEL_SENSOR_ACCELERATION, driver, "a"},
        {1, MICROPIXEL_SENSOR_MAGNETIC_FIELD, driver, "b"},
    }};
    PolledVectorSensorPeripheral invalid(duplicates, "test");
    assert(invalid.Initialize(executor) == ESP_ERR_INVALID_ARG);
}

void TestInertialAdapter() {
    I2cExecutor executor;
    Driver accel(executor), gyro(executor);
    micropixel::platform::sensors::PolledInertialSensorPeripheral peripheral(accel, gyro,
                                                                             {"test", "IMU", "accel", "gyro"});
    peripheral.Initialize(reinterpret_cast<void*>(1), executor);
    assert(accel.initializations == 1 && gyro.initializations == 0);
    assert(peripheral.Start(2, 20000) == MICROPIXEL_STATUS_OK);
    FireSensorTimer("gyro");
    executor.Drain();
    assert(gyro.reads == 1);
}
}  // namespace

int main() {
    TestChannelsAndLifecycle();
    TestFailures();
    TestTimerFailureAndInvalidChannels();
    TestInertialAdapter();
    std::puts("Polled vector sensor tests passed.");
}
