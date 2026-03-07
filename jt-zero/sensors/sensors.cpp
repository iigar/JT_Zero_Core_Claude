/**
 * JT-Zero Sensor Implementations
 * Simulated sensors for development and testing
 * On real hardware, replace with actual I2C/SPI drivers
 */

#include "jt_zero/sensors.h"
#include <cmath>
#include <cstdlib>

namespace jtzero {

// ─── Noise helper ────────────────────────────────────────
static float noise(float amplitude) {
    return amplitude * (static_cast<float>(rand()) / RAND_MAX - 0.5f) * 2.0f;
}

// ─── IMU ─────────────────────────────────────────────────

bool IMUSensor::initialize() {
    data_ = {};
    data_.acc_z = -9.81f;  // Gravity
    initialized_ = true;
    return true;
}

bool IMUSensor::update() {
    if (!initialized_) return false;
    
    if (simulated_) {
        update_count_++;
        const double t = static_cast<double>(update_count_) / 200.0;  // 200 Hz
        
        // Simulate gentle oscillation + noise
        data_.timestamp_us = now_us();
        data_.gyro_x = 0.01f * std::sin(t * 0.5f) + noise(0.002f);
        data_.gyro_y = 0.008f * std::cos(t * 0.7f) + noise(0.002f);
        data_.gyro_z = 0.005f * std::sin(t * 0.3f) + noise(0.001f);
        data_.acc_x  = 0.1f * std::sin(t * 0.2f) + noise(0.05f);
        data_.acc_y  = 0.08f * std::cos(t * 0.15f) + noise(0.05f);
        data_.acc_z  = -9.81f + 0.05f * std::sin(t * 0.1f) + noise(0.02f);
        data_.valid   = true;
    }
    
    return data_.valid;
}

bool IMUSensor::is_healthy() const {
    return initialized_ && data_.valid;
}

// ─── Barometer ───────────────────────────────────────────

bool BarometerSensor::initialize() {
    data_ = {};
    data_.pressure = base_pressure_;
    data_.temperature = 22.0f;
    initialized_ = true;
    return true;
}

bool BarometerSensor::update() {
    if (!initialized_) return false;
    
    if (simulated_) {
        update_count_++;
        const double t = static_cast<double>(update_count_) / 50.0;
        
        // Simulate altitude changes
        target_alt_ = 5.0f + 2.0f * std::sin(t * 0.05f);
        
        data_.timestamp_us = now_us();
        data_.altitude = target_alt_ + noise(0.1f);
        data_.pressure = base_pressure_ - (data_.altitude * 0.12f) + noise(0.01f);
        data_.temperature = 22.0f + noise(0.5f) - data_.altitude * 0.0065f;
        data_.valid = true;
    }
    
    return data_.valid;
}

bool BarometerSensor::is_healthy() const {
    return initialized_ && data_.valid;
}

// ─── GPS ─────────────────────────────────────────────────

bool GPSSensor::initialize() {
    data_ = {};
    data_.lat = 50.4501;   // Kyiv
    data_.lon = 30.5234;
    data_.satellites = 12;
    data_.fix_type = 3;
    initialized_ = true;
    return true;
}

bool GPSSensor::update() {
    if (!initialized_) return false;
    
    if (simulated_) {
        update_count_++;
        const double t = static_cast<double>(update_count_) / 10.0;
        
        // Simulate slow drift / flight path
        data_.timestamp_us = now_us();
        data_.lat = 50.4501 + 0.0001 * std::sin(t * 0.02);
        data_.lon = 30.5234 + 0.0001 * std::cos(t * 0.015);
        data_.alt = 150.0f + 5.0f * std::sin(t * 0.05f) + noise(0.3f);
        data_.speed = 2.0f + noise(0.5f);
        data_.satellites = 12 + static_cast<uint8_t>(noise(2.0f));
        data_.fix_type = 3;
        data_.valid = true;
    }
    
    return data_.valid;
}

bool GPSSensor::is_healthy() const {
    return initialized_ && data_.valid && data_.fix_type >= 2;
}

// ─── Rangefinder ─────────────────────────────────────────

bool RangefinderSensor::initialize() {
    data_ = {};
    initialized_ = true;
    return true;
}

bool RangefinderSensor::update() {
    if (!initialized_) return false;
    
    if (simulated_) {
        update_count_++;
        const double t = static_cast<double>(update_count_) / 50.0;
        
        data_.timestamp_us = now_us();
        data_.distance = 3.0f + 1.5f * std::sin(t * 0.1f) + noise(0.05f);
        data_.signal_quality = 0.95f + noise(0.03f);
        if (data_.distance < 0.1f) data_.distance = 0.1f;
        if (data_.signal_quality > 1.0f) data_.signal_quality = 1.0f;
        if (data_.signal_quality < 0.0f) data_.signal_quality = 0.0f;
        data_.valid = true;
    }
    
    return data_.valid;
}

bool RangefinderSensor::is_healthy() const {
    return initialized_ && data_.valid && data_.signal_quality > 0.3f;
}

// ─── Optical Flow ────────────────────────────────────────

bool OpticalFlowSensor::initialize() {
    data_ = {};
    initialized_ = true;
    return true;
}

bool OpticalFlowSensor::update() {
    if (!initialized_) return false;
    
    if (simulated_) {
        update_count_++;
        const double t = static_cast<double>(update_count_) / 50.0;
        
        data_.timestamp_us = now_us();
        data_.flow_x = 0.05f * std::sin(t * 0.3f) + noise(0.01f);
        data_.flow_y = 0.03f * std::cos(t * 0.2f) + noise(0.01f);
        data_.quality = static_cast<uint8_t>(200 + noise(30.0f));
        data_.ground_distance = 3.0f + noise(0.1f);
        data_.valid = true;
    }
    
    return data_.valid;
}

bool OpticalFlowSensor::is_healthy() const {
    return initialized_ && data_.valid && data_.quality > 50;
}

} // namespace jtzero
