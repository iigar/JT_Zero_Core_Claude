#pragma once
/**
 * JT-Zero Rule Engine
 * Complex behavior logic with state machine support
 * Runs at lower frequency (10-20 Hz) for complex decisions
 */

#include "jt_zero/common.h"
#include "jt_zero/event_engine.h"

namespace jtzero {

// Rule evaluation result
enum class RuleAction : uint8_t {
    NONE = 0,
    ARM,
    DISARM,
    TAKEOFF,
    LAND,
    HOLD,
    RTL,
    SET_ALTITUDE,
    SET_VELOCITY,
    EMIT_WARNING,
    EMIT_ERROR,
    CUSTOM
};

struct RuleResult {
    RuleAction action{RuleAction::NONE};
    float params[4]{0};
    char  message[64]{};
};

// Behavior rule with priority and state requirements
struct BehaviorRule {
    const char*  name;
    int          priority;  // Higher = checked first
    FlightMode   required_mode;  // IDLE = any mode
    bool (*evaluate)(const SystemState& state, RuleResult& result);
    bool enabled{true};
    uint64_t eval_count{0};
    uint64_t trigger_count{0};
};

class RuleEngine {
public:
    static constexpr size_t MAX_RULES = 64;
    
    RuleEngine();
    
    // Register behavior rule
    bool add_rule(const BehaviorRule& rule);
    
    // Evaluate all rules and return highest-priority triggered action
    RuleResult evaluate(const SystemState& state);
    
    // Execute rule result (modifies state, emits events)
    void execute(const RuleResult& result, SystemState& state, EventEngine& events);
    
    // Rule management
    void set_enabled(const char* name, bool enabled);
    size_t rule_count() const;
    
    // Get rule statistics
    uint64_t total_evaluations() const;
    
private:
    std::array<BehaviorRule, MAX_RULES> rules_;
    size_t rule_count_{0};
    std::atomic<uint64_t> total_evals_{0};
    
    void sort_rules_by_priority();
};

} // namespace jtzero
