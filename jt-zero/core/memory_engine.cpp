/**
 * JT-Zero Memory Engine Implementation
 * Fixed-size ring buffer telemetry and event storage
 */

#include "jt_zero/memory_engine.h"
#include <cstring>
#include <algorithm>

namespace jtzero {

MemoryEngine::MemoryEngine() {
    telemetry_.fill({});
    events_.fill({});
}

void MemoryEngine::record_telemetry(const SystemState& state) {
    const size_t idx = telem_write_idx_.load(std::memory_order_relaxed) % TELEMETRY_HISTORY;
    
    auto& rec = telemetry_[idx];
    rec.timestamp_us = now_us();
    rec.imu   = state.imu;
    rec.baro  = state.baro;
    rec.gps   = state.gps;
    rec.range = state.range;
    rec.flow  = state.flow;
    rec.mode  = state.flight_mode;
    rec.roll  = state.roll;
    rec.pitch = state.pitch;
    rec.yaw   = state.yaw;
    rec.battery_voltage = state.battery_voltage;
    rec.cpu_usage = state.cpu_usage;
    
    telem_write_idx_.fetch_add(1, std::memory_order_release);
    telem_total_.fetch_add(1, std::memory_order_relaxed);
}

void MemoryEngine::record_event(const Event& event) {
    const size_t idx = event_write_idx_.load(std::memory_order_relaxed) % EVENT_HISTORY;
    
    auto& rec = events_[idx];
    rec.timestamp_us = event.timestamp_us;
    rec.type = event.type;
    rec.priority = event.priority;
    std::strncpy(rec.message, event.message, sizeof(rec.message) - 1);
    rec.message[sizeof(rec.message) - 1] = '\0';
    
    event_write_idx_.fetch_add(1, std::memory_order_release);
    event_total_.fetch_add(1, std::memory_order_relaxed);
}

size_t MemoryEngine::get_recent_telemetry(TelemetryRecord* out, size_t max_count) const {
    const size_t write_idx = telem_write_idx_.load(std::memory_order_acquire);
    const size_t total = telem_total_.load(std::memory_order_acquire);
    const size_t available = std::min(total, TELEMETRY_HISTORY);
    const size_t count = std::min(max_count, available);
    
    for (size_t i = 0; i < count; ++i) {
        const size_t idx = (write_idx - count + i) % TELEMETRY_HISTORY;
        out[i] = telemetry_[idx];
    }
    
    return count;
}

size_t MemoryEngine::get_recent_events(EventRecord* out, size_t max_count) const {
    const size_t write_idx = event_write_idx_.load(std::memory_order_acquire);
    const size_t total = event_total_.load(std::memory_order_acquire);
    const size_t available = std::min(total, EVENT_HISTORY);
    const size_t count = std::min(max_count, available);
    
    for (size_t i = 0; i < count; ++i) {
        const size_t idx = (write_idx - count + i) % EVENT_HISTORY;
        out[i] = events_[idx];
    }
    
    return count;
}

TelemetryRecord MemoryEngine::get_latest_telemetry() const {
    const size_t write_idx = telem_write_idx_.load(std::memory_order_acquire);
    if (telem_total_.load(std::memory_order_acquire) == 0) {
        return {};
    }
    return telemetry_[(write_idx - 1) % TELEMETRY_HISTORY];
}

uint64_t MemoryEngine::total_telemetry_records() const {
    return telem_total_.load(std::memory_order_relaxed);
}

uint64_t MemoryEngine::total_event_records() const {
    return event_total_.load(std::memory_order_relaxed);
}

size_t MemoryEngine::memory_usage_bytes() const {
    return sizeof(telemetry_) + sizeof(events_);
}

void MemoryEngine::clear() {
    telem_write_idx_.store(0, std::memory_order_release);
    telem_total_.store(0, std::memory_order_release);
    event_write_idx_.store(0, std::memory_order_release);
    event_total_.store(0, std::memory_order_release);
}

} // namespace jtzero
