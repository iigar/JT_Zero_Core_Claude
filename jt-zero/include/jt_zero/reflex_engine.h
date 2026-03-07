#pragma once
/**
 * JT-Zero Reflex Engine
 * Ultra-fast reactive responses (<5ms latency)
 * Pattern: Event → Condition → Immediate Action
 */

#include "jt_zero/common.h"
#include "jt_zero/event_engine.h"
#include <functional>

namespace jtzero {

// Reflex rule: condition + action pair
struct ReflexRule {
    const char*   name;
    EventType     trigger;
    uint8_t       min_priority;
    // Returns true if reflex should fire
    bool (*condition)(const Event& event, const SystemState& state);
    // Executes the reflex action
    void (*action)(const Event& event, SystemState& state, EventEngine& events);
    bool enabled{true};
    uint64_t fire_count{0};
    uint64_t last_fire_us{0};
    uint64_t cooldown_us{0};  // Minimum time between fires
};

class ReflexEngine {
public:
    static constexpr size_t MAX_RULES = 32;
    
    ReflexEngine();
    
    // Register a reflex rule
    bool add_rule(const ReflexRule& rule);
    
    // Process event through all matching reflexes
    // Returns number of reflexes that fired
    int process(const Event& event, SystemState& state, EventEngine& events);
    
    // Enable/disable rule by name
    void set_enabled(const char* name, bool enabled);
    
    // Statistics
    size_t rule_count() const;
    uint64_t total_fires() const;
    double avg_latency_us() const;
    
private:
    std::array<ReflexRule, MAX_RULES> rules_;
    size_t rule_count_{0};
    std::atomic<uint64_t> total_fires_{0};
    uint64_t latency_sum_us_{0};
    uint64_t latency_count_{0};
};

} // namespace jtzero
