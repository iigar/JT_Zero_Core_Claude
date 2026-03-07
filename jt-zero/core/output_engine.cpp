/**
 * JT-Zero Output Engine Implementation
 */

#include "jt_zero/output_engine.h"
#include <cstdio>
#include <cstring>

namespace jtzero {

OutputEngine::OutputEngine() {
    handler_ = [this](const OutputCommand& cmd) { default_handler(cmd); };
}

bool OutputEngine::queue(const OutputCommand& cmd) {
    return queue_.push(cmd);
}

bool OutputEngine::log_info(const char* msg) {
    OutputCommand cmd;
    cmd.timestamp_us = now_us();
    cmd.type = OutputType::LOG_INFO;
    cmd.set_message(msg);
    return queue(cmd);
}

bool OutputEngine::log_warning(const char* msg) {
    OutputCommand cmd;
    cmd.timestamp_us = now_us();
    cmd.type = OutputType::LOG_WARNING;
    cmd.set_message(msg);
    return queue(cmd);
}

bool OutputEngine::log_error(const char* msg) {
    OutputCommand cmd;
    cmd.timestamp_us = now_us();
    cmd.type = OutputType::LOG_ERROR;
    cmd.set_message(msg);
    return queue(cmd);
}

bool OutputEngine::set_gpio(uint8_t pin, float value) {
    OutputCommand cmd;
    cmd.timestamp_us = now_us();
    cmd.type = OutputType::GPIO_SET;
    cmd.channel = pin;
    cmd.value = value;
    char msg[128];
    std::snprintf(msg, sizeof(msg), "GPIO pin %d = %.2f", pin, value);
    cmd.set_message(msg);
    return queue(cmd);
}

bool OutputEngine::send_mavlink(uint8_t cmd_id, float p1, float p2) {
    OutputCommand cmd;
    cmd.timestamp_us = now_us();
    cmd.type = OutputType::MAVLINK_CMD;
    cmd.channel = cmd_id;
    cmd.value = p1;
    char msg[128];
    std::snprintf(msg, sizeof(msg), "MAVLink CMD %d (%.2f, %.2f)", cmd_id, p1, p2);
    cmd.set_message(msg);
    return queue(cmd);
}

int OutputEngine::process_pending() {
    int processed = 0;
    OutputCommand cmd;
    
    while (queue_.pop(cmd)) {
        if (handler_) {
            handler_(cmd);
        }
        total_outputs_.fetch_add(1, std::memory_order_relaxed);
        processed++;
    }
    
    return processed;
}

void OutputEngine::set_handler(OutputHandler handler) {
    handler_ = handler;
}

uint64_t OutputEngine::total_outputs() const {
    return total_outputs_.load(std::memory_order_relaxed);
}

uint64_t OutputEngine::pending_count() const {
    return queue_.size();
}

void OutputEngine::default_handler(const OutputCommand& cmd) {
    const char* prefix = "INFO";
    switch (cmd.type) {
        case OutputType::LOG_WARNING: prefix = "WARN"; break;
        case OutputType::LOG_ERROR:   prefix = "ERR "; break;
        case OutputType::GPIO_SET:    prefix = "GPIO"; break;
        case OutputType::GPIO_PWM:    prefix = "PWM "; break;
        case OutputType::MAVLINK_CMD: prefix = "MAV "; break;
        case OutputType::API_NOTIFY:  prefix = "API "; break;
        case OutputType::BUZZER:      prefix = "BZR "; break;
        case OutputType::LED:         prefix = "LED "; break;
        default:                      prefix = "INFO"; break;
    }
    std::printf("[OUT/%s] %s\n", prefix, cmd.message);
}

} // namespace jtzero
