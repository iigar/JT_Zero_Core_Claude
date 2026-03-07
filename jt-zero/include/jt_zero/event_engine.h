#pragma once
/**
 * JT-Zero Event Engine
 * Lock-free event queue with priority dispatch
 */

#include "jt_zero/common.h"

namespace jtzero {

class EventEngine {
public:
    static constexpr size_t QUEUE_SIZE = 1024;  // Must be power of 2
    
    EventEngine();
    
    // Push event into queue (thread-safe, lock-free)
    bool emit(const Event& event);
    
    // Convenience emitter
    bool emit(EventType type, uint8_t priority = 0, const char* msg = nullptr);
    
    // Pop next event (thread-safe, lock-free)
    bool poll(Event& event);
    
    // Check if events available
    bool has_events() const;
    
    // Queue statistics
    size_t pending_count() const;
    uint64_t total_events() const;
    uint64_t dropped_events() const;
    
    // Reset counters
    void reset_stats();
    
private:
    RingBuffer<Event, QUEUE_SIZE> queue_;
    std::atomic<uint64_t> total_events_{0};
    std::atomic<uint64_t> dropped_events_{0};
};

} // namespace jtzero
