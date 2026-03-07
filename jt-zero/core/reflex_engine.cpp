/**
 * JT-Zero Reflex Engine Implementation
 * Ultra-fast pattern matching and response
 */

#include "jt_zero/reflex_engine.h"
#include <cstring>

namespace jtzero {

ReflexEngine::ReflexEngine() = default;

bool ReflexEngine::add_rule(const ReflexRule& rule) {
    if (rule_count_ >= MAX_RULES) return false;
    rules_[rule_count_++] = rule;
    return true;
}

int ReflexEngine::process(const Event& event, SystemState& state, EventEngine& events) {
    int fired = 0;
    const uint64_t now = now_us();
    
    for (size_t i = 0; i < rule_count_; ++i) {
        auto& rule = rules_[i];
        
        if (!rule.enabled) continue;
        if (rule.trigger != event.type && rule.trigger != EventType::NONE) continue;
        if (event.priority < rule.min_priority) continue;
        
        // Cooldown check
        if (rule.cooldown_us > 0 && 
            (now - rule.last_fire_us) < rule.cooldown_us) continue;
        
        // Evaluate condition
        if (rule.condition && !rule.condition(event, state)) continue;
        
        // Fire reflex action
        const uint64_t before = now_us();
        if (rule.action) {
            rule.action(event, state, events);
        }
        const uint64_t after = now_us();
        
        rule.fire_count++;
        rule.last_fire_us = now;
        fired++;
        total_fires_.fetch_add(1, std::memory_order_relaxed);
        
        latency_sum_us_ += (after - before);
        latency_count_++;
    }
    
    return fired;
}

void ReflexEngine::set_enabled(const char* name, bool enabled) {
    for (size_t i = 0; i < rule_count_; ++i) {
        if (std::strcmp(rules_[i].name, name) == 0) {
            rules_[i].enabled = enabled;
            return;
        }
    }
}

size_t ReflexEngine::rule_count() const {
    return rule_count_;
}

uint64_t ReflexEngine::total_fires() const {
    return total_fires_.load(std::memory_order_relaxed);
}

double ReflexEngine::avg_latency_us() const {
    if (latency_count_ == 0) return 0.0;
    return static_cast<double>(latency_sum_us_) / static_cast<double>(latency_count_);
}

} // namespace jtzero
