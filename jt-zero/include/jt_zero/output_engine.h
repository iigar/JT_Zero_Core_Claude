#pragma once
/**
 * JT-Zero Output Engine
 * Hardware action execution and logging
 * GPIO, MAVLink commands, API notifications
 */

#include "jt_zero/common.h"
#include <functional>

namespace jtzero {

enum class OutputType : uint8_t {
    LOG_INFO = 0,
    LOG_WARNING,
    LOG_ERROR,
    GPIO_SET,
    GPIO_PWM,
    MAVLINK_CMD,
    API_NOTIFY,
    BUZZER,
    LED
};

struct OutputCommand {
    uint64_t   timestamp_us{0};
    OutputType type{OutputType::LOG_INFO};
    uint8_t    channel{0};
    float      value{0};
    char       message[128]{};
    
    void set_message(const char* msg) {
        std::strncpy(message, msg, sizeof(message) - 1);
        message[sizeof(message) - 1] = '\0';
    }
};

class OutputEngine {
public:
    static constexpr size_t QUEUE_SIZE = 256;
    
    OutputEngine();
    
    // Queue an output command
    bool queue(const OutputCommand& cmd);
    
    // Convenience methods
    bool log_info(const char* msg);
    bool log_warning(const char* msg);
    bool log_error(const char* msg);
    bool set_gpio(uint8_t pin, float value);
    bool send_mavlink(uint8_t cmd_id, float p1 = 0, float p2 = 0);
    
    // Process pending outputs (call from output thread)
    int process_pending();
    
    // Set custom output handler
    using OutputHandler = std::function<void(const OutputCommand&)>;
    void set_handler(OutputHandler handler);
    
    // Statistics
    uint64_t total_outputs() const;
    uint64_t pending_count() const;
    
private:
    RingBuffer<OutputCommand, QUEUE_SIZE> queue_;
    std::atomic<uint64_t> total_outputs_{0};
    OutputHandler handler_;
    
    void default_handler(const OutputCommand& cmd);
};

} // namespace jtzero
