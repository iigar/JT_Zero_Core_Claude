#pragma once
/**
 * JT-Zero Sensor Interfaces
 * Abstract sensor interfaces with simulated implementations
 */

#include "jt_zero/common.h"

namespace jtzero {

// ─── Abstract Sensor Interface ───────────────────────────

class SensorBase {
public:
    virtual ~SensorBase() = default;
    virtual bool initialize() = 0;
    virtual bool update() = 0;
    virtual bool is_healthy() const = 0;
    virtual const char* name() const = 0;
    virtual uint32_t update_rate_hz() const = 0;
};

// ─── IMU Sensor ──────────────────────────────────────────

class IMUSensor : public SensorBase {
public:
    bool initialize() override;
    bool update() override;
    bool is_healthy() const override;
    const char* name() const override { return "IMU"; }
    uint32_t update_rate_hz() const override { return 200; }
    
    const IMUData& data() const { return data_; }
    
    // Simulation control
    void set_simulated(bool sim) { simulated_ = sim; }
    void inject_data(const IMUData& d) { data_ = d; }
    
private:
    IMUData data_;
    bool initialized_{false};
    bool simulated_{true};
    uint64_t update_count_{0};
};

// ─── Barometer ───────────────────────────────────────────

class BarometerSensor : public SensorBase {
public:
    bool initialize() override;
    bool update() override;
    bool is_healthy() const override;
    const char* name() const override { return "BARO"; }
    uint32_t update_rate_hz() const override { return 50; }
    
    const BarometerData& data() const { return data_; }
    void set_simulated(bool sim) { simulated_ = sim; }
    
private:
    BarometerData data_;
    bool initialized_{false};
    bool simulated_{true};
    float base_pressure_{1013.25f};
    float target_alt_{0.0f};
    uint64_t update_count_{0};
};

// ─── GPS ─────────────────────────────────────────────────

class GPSSensor : public SensorBase {
public:
    bool initialize() override;
    bool update() override;
    bool is_healthy() const override;
    const char* name() const override { return "GPS"; }
    uint32_t update_rate_hz() const override { return 10; }
    
    const GPSData& data() const { return data_; }
    void set_simulated(bool sim) { simulated_ = sim; }
    
private:
    GPSData data_;
    bool initialized_{false};
    bool simulated_{true};
    uint64_t update_count_{0};
};

// ─── Rangefinder ─────────────────────────────────────────

class RangefinderSensor : public SensorBase {
public:
    bool initialize() override;
    bool update() override;
    bool is_healthy() const override;
    const char* name() const override { return "RANGE"; }
    uint32_t update_rate_hz() const override { return 50; }
    
    const RangefinderData& data() const { return data_; }
    void set_simulated(bool sim) { simulated_ = sim; }
    
private:
    RangefinderData data_;
    bool initialized_{false};
    bool simulated_{true};
    uint64_t update_count_{0};
};

// ─── Optical Flow ────────────────────────────────────────

class OpticalFlowSensor : public SensorBase {
public:
    bool initialize() override;
    bool update() override;
    bool is_healthy() const override;
    const char* name() const override { return "FLOW"; }
    uint32_t update_rate_hz() const override { return 50; }
    
    const OpticalFlowData& data() const { return data_; }
    void set_simulated(bool sim) { simulated_ = sim; }
    
private:
    OpticalFlowData data_;
    bool initialized_{false};
    bool simulated_{true};
    uint64_t update_count_{0};
};

} // namespace jtzero
