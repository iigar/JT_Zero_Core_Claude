#pragma once
/**
 * JT-Zero Memory Engine
 * Ring-buffer based telemetry history
 * Fixed memory, no dynamic allocation
 */

#include "jt_zero/common.h"
#include <mutex>

namespace jtzero {

// Telemetry snapshot stored in memory
struct TelemetryRecord {
    uint64_t timestamp_us{0};
    IMUData          imu;
    BarometerData    baro;
    GPSData          gps;
    RangefinderData  range;
    OpticalFlowData  flow;
    FlightMode       mode{FlightMode::IDLE};
    float roll{0}, pitch{0}, yaw{0};
    float battery_voltage{0};
    float cpu_usage{0};
};

// Event log entry
struct EventRecord {
    uint64_t  timestamp_us{0};
    EventType type{EventType::NONE};
    uint8_t   priority{0};
    char      message[64]{};
};

class MemoryEngine {
public:
    static constexpr size_t TELEMETRY_HISTORY = 2048;  // ~100s at 20Hz
    static constexpr size_t EVENT_HISTORY = 512;

    MemoryEngine();
    
    // Record telemetry snapshot
    void record_telemetry(const SystemState& state);
    
    // Record event
    void record_event(const Event& event);
    
    // Get recent telemetry (returns count of records copied)
    size_t get_recent_telemetry(TelemetryRecord* out, size_t max_count) const;
    
    // Get recent events (returns count of records copied)
    size_t get_recent_events(EventRecord* out, size_t max_count) const;
    
    // Get latest telemetry
    TelemetryRecord get_latest_telemetry() const;
    
    // Statistics
    uint64_t total_telemetry_records() const;
    uint64_t total_event_records() const;
    size_t   memory_usage_bytes() const;
    
    // Clear history
    void clear();
    
private:
    // Telemetry ring buffer
    std::array<TelemetryRecord, TELEMETRY_HISTORY> telemetry_;
    std::atomic<size_t> telem_write_idx_{0};
    std::atomic<uint64_t> telem_total_{0};
    
    // Event ring buffer
    std::array<EventRecord, EVENT_HISTORY> events_;
    std::atomic<size_t> event_write_idx_{0};
    std::atomic<uint64_t> event_total_{0};
};

} // namespace jtzero
