#pragma once
/**
 * JT-Zero Runtime
 * Main runtime orchestrator - manages all engines and threads
 */

#include "jt_zero/common.h"
#include "jt_zero/event_engine.h"
#include "jt_zero/reflex_engine.h"
#include "jt_zero/rule_engine.h"
#include "jt_zero/memory_engine.h"
#include "jt_zero/output_engine.h"
#include "jt_zero/sensors.h"

#include <thread>
#include <atomic>

namespace jtzero {

class Runtime {
public:
    Runtime();
    ~Runtime();
    
    // Lifecycle
    bool initialize();
    void start();
    void stop();
    bool is_running() const;
    
    // Access engines (for Python bindings / API)
    EventEngine&   events()   { return event_engine_; }
    ReflexEngine&  reflexes() { return reflex_engine_; }
    RuleEngine&    rules()    { return rule_engine_; }
    MemoryEngine&  memory()   { return memory_engine_; }
    OutputEngine&  output()   { return output_engine_; }
    
    const EventEngine&   events()   const { return event_engine_; }
    const ReflexEngine&  reflexes() const { return reflex_engine_; }
    const RuleEngine&    rules()    const { return rule_engine_; }
    const MemoryEngine&  memory()   const { return memory_engine_; }
    const OutputEngine&  output()   const { return output_engine_; }
    
    // Access state
    const SystemState& state() const { return state_; }
    SystemState& state() { return state_; }
    
    // Access sensors
    IMUSensor&          imu()   { return imu_; }
    BarometerSensor&    baro()  { return baro_; }
    GPSSensor&          gps()   { return gps_sensor_; }
    RangefinderSensor&  range() { return range_; }
    OpticalFlowSensor&  flow()  { return flow_; }
    
    // Command interface
    bool send_command(const char* cmd, float param1 = 0, float param2 = 0);
    
    // Thread stats
    struct ThreadStats {
        const char* name;
        double actual_hz;
        double cpu_percent;
        uint64_t loop_count;
        uint64_t max_latency_us;
        bool running;
    };
    
    ThreadStats get_thread_stats(int thread_id) const;
    
    // Simulator mode
    void set_simulator_mode(bool enabled) { simulator_mode_ = enabled; }
    bool is_simulator_mode() const { return simulator_mode_; }
    
private:
    // Engines
    EventEngine   event_engine_;
    ReflexEngine  reflex_engine_;
    RuleEngine    rule_engine_;
    MemoryEngine  memory_engine_;
    OutputEngine  output_engine_;
    
    // Sensors
    IMUSensor          imu_;
    BarometerSensor    baro_;
    GPSSensor          gps_sensor_;
    RangefinderSensor  range_;
    OpticalFlowSensor  flow_;
    
    // State
    SystemState state_;
    std::atomic<bool> running_{false};
    bool simulator_mode_{true};
    
    // Threads
    std::thread t0_supervisor_;
    std::thread t1_sensors_;
    std::thread t2_events_;
    std::thread t3_reflex_;
    std::thread t4_rules_;
    
    // Thread stats (lock-free access)
    static constexpr int NUM_THREADS = 8;
    struct InternalThreadStats {
        std::atomic<double> actual_hz{0};
        std::atomic<double> cpu_percent{0};
        std::atomic<uint64_t> loop_count{0};
        std::atomic<uint64_t> max_latency_us{0};
        std::atomic<bool> running{false};
    };
    std::array<InternalThreadStats, NUM_THREADS> thread_stats_;
    
    // Thread functions
    void supervisor_loop();     // T0: 10 Hz
    void sensor_loop();         // T1: 200 Hz
    void event_loop();          // T2: 200 Hz
    void reflex_loop();         // T3: 200 Hz
    void rule_loop();           // T4: 20 Hz
    
    // Setup default rules
    void setup_default_reflexes();
    void setup_default_rules();
    
    // Helper: precise sleep to target frequency
    static void rate_sleep(TimePoint& next_wake, int hz);
    
    // Helper: update thread stats
    void update_thread_stats(int id, TimePoint start, TimePoint end, int target_hz);
};

} // namespace jtzero
