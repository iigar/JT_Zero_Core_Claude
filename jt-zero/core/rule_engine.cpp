/**
 * JT-Zero Rule Engine Implementation
 * Complex behavior logic with priority-based evaluation
 */

#include "jt_zero/rule_engine.h"
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace jtzero {

RuleEngine::RuleEngine() = default;

bool RuleEngine::add_rule(const BehaviorRule& rule) {
    if (rule_count_ >= MAX_RULES) return false;
    rules_[rule_count_++] = rule;
    sort_rules_by_priority();
    return true;
}

RuleResult RuleEngine::evaluate(const SystemState& state) {
    total_evals_.fetch_add(1, std::memory_order_relaxed);
    
    RuleResult result;
    
    for (size_t i = 0; i < rule_count_; ++i) {
        auto& rule = rules_[i];
        
        if (!rule.enabled) continue;
        
        // Check mode requirement
        if (rule.required_mode != FlightMode::IDLE && 
            rule.required_mode != state.flight_mode) continue;
        
        rule.eval_count++;
        
        RuleResult candidate;
        if (rule.evaluate && rule.evaluate(state, candidate)) {
            rule.trigger_count++;
            // First triggered rule wins (sorted by priority)
            if (result.action == RuleAction::NONE) {
                result = candidate;
            }
        }
    }
    
    return result;
}

void RuleEngine::execute(const RuleResult& result, SystemState& state, EventEngine& events) {
    switch (result.action) {
        case RuleAction::NONE:
            break;
        case RuleAction::ARM:
            state.armed = true;
            state.flight_mode = FlightMode::ARMED;
            events.emit(EventType::FLIGHT_ARM, 200, "Armed by rule engine");
            break;
        case RuleAction::DISARM:
            state.armed = false;
            state.flight_mode = FlightMode::IDLE;
            events.emit(EventType::FLIGHT_DISARM, 200, "Disarmed by rule engine");
            break;
        case RuleAction::TAKEOFF:
            state.flight_mode = FlightMode::TAKEOFF;
            events.emit(EventType::FLIGHT_TAKEOFF, 200, "Takeoff initiated");
            break;
        case RuleAction::LAND:
            state.flight_mode = FlightMode::LAND;
            events.emit(EventType::FLIGHT_LAND, 200, "Landing initiated");
            break;
        case RuleAction::HOLD:
            state.flight_mode = FlightMode::HOVER;
            events.emit(EventType::FLIGHT_HOLD, 150, "Position hold");
            break;
        case RuleAction::RTL:
            state.flight_mode = FlightMode::RTL;
            events.emit(EventType::FLIGHT_RTL, 220, "Return to launch");
            break;
        case RuleAction::EMIT_WARNING:
            events.emit(EventType::SYSTEM_WARNING, 100, result.message);
            break;
        case RuleAction::EMIT_ERROR:
            events.emit(EventType::SYSTEM_ERROR, 200, result.message);
            break;
        default:
            break;
    }
}

void RuleEngine::set_enabled(const char* name, bool enabled) {
    for (size_t i = 0; i < rule_count_; ++i) {
        if (std::strcmp(rules_[i].name, name) == 0) {
            rules_[i].enabled = enabled;
            return;
        }
    }
}

size_t RuleEngine::rule_count() const {
    return rule_count_;
}

uint64_t RuleEngine::total_evaluations() const {
    return total_evals_.load(std::memory_order_relaxed);
}

void RuleEngine::sort_rules_by_priority() {
    std::sort(rules_.begin(), rules_.begin() + rule_count_,
        [](const BehaviorRule& a, const BehaviorRule& b) {
            return a.priority > b.priority;
        });
}

} // namespace jtzero
