/**
 * JT-Zero Event Engine Implementation
 */

#include "jt_zero/event_engine.h"

namespace jtzero {

EventEngine::EventEngine() = default;

bool EventEngine::emit(const Event& event) {
    std::lock_guard<std::mutex> lk(emit_mutex_);
    if (queue_.push(event)) {
        total_events_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    dropped_events_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool EventEngine::emit(EventType type, uint8_t priority, const char* msg) {
    Event e;
    e.timestamp_us = now_us();
    e.type = type;
    e.priority = priority;
    if (msg) e.set_message(msg);
    return emit(e);
}

bool EventEngine::poll(Event& event) {
    return queue_.pop(event);
}

bool EventEngine::has_events() const {
    return !queue_.empty();
}

size_t EventEngine::pending_count() const {
    return queue_.size();
}

uint64_t EventEngine::total_events() const {
    return total_events_.load(std::memory_order_relaxed);
}

uint64_t EventEngine::dropped_events() const {
    return dropped_events_.load(std::memory_order_relaxed);
}

void EventEngine::reset_stats() {
    total_events_.store(0, std::memory_order_relaxed);
    dropped_events_.store(0, std::memory_order_relaxed);
}

} // namespace jtzero
