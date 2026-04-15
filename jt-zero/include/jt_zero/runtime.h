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
#include "jt_zero/camera.h"
#include "jt_zero/mavlink_interface.h"
#include "../drivers/bus.h"

#include <thread>
#include <atomic>
#include <mutex>

namespace jtzero {

// Atomic snapshot of safety-critical fields for T3 (200 Hz, priority 98).
// Packed into 8 bytes so std::atomic<SafetySnapshot> is lock-free on ARM64.
struct SafetySnapshot {
    float   range_distance;  // meters
    bool    range_valid;
    uint8_t _pad[3];         // explicit padding to 8 bytes
};
static_assert(sizeof(SafetySnapshot) == 8, "SafetySnapshot must be 8 bytes for lock-free atomic");

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
    
    // Access state — raw reference (caller must hold sensor_mutex_ + slow_mutex_)
    const SystemState& state() const { return state_; }
    SystemState& state() { return state_; }

    // Thread-safe snapshot — returns a copy under both mutexes (canonical order)
    SystemState state_snapshot() const {
        std::lock_guard<std::mutex> lk_b(sensor_mutex_);
        std::lock_guard<std::mutex> lk_c(slow_mutex_);
        return state_;
    }
    
    // Access sensors
    IMUSensor&          imu()   { return imu_; }
    BarometerSensor&    baro()  { return baro_; }
    GPSSensor&          gps()   { return gps_sensor_; }
    RangefinderSensor&  range() { return range_; }
    OpticalFlowSensor&  flow()  { return flow_; }
    
    const IMUSensor&          imu()   const { return imu_; }
    const BarometerSensor&    baro()  const { return baro_; }
    const GPSSensor&          gps()   const { return gps_sensor_; }
    const RangefinderSensor&  range() const { return range_; }
    const OpticalFlowSensor&  flow()  const { return flow_; }
    
    // Access camera & MAVLink
    CameraPipeline&    camera()  { return camera_; }
    MAVLinkInterface&  mavlink() { return mavlink_; }
    const CameraPipeline&   camera()  const { return camera_; }
    const MAVLinkInterface& mavlink() const { return mavlink_; }
    
    // Command interface
    bool send_command(const char* cmd, float param1 = 0, float param2 = 0);
    
    // Hardware info
    const HardwareInfo& hw_info() const { return hw_info_; }
    
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
    
    // Simulator config (tuneable)
    SimulatorConfig& sim_config() { return sim_config_; }
    const SimulatorConfig& sim_config() const { return sim_config_; }

    // Synchronization accessors (needed by python_bindings.cpp)
    std::mutex& sensor_mutex() noexcept { return sensor_mutex_; }

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
    
    // Camera & MAVLink
    CameraPipeline    camera_;
    MAVLinkInterface  mavlink_;
    
    // Hardware buses (for direct sensor access)
    I2CBus  i2c_bus_;
    UARTBus gps_uart_;
    
    // Hardware detection result
    HardwareInfo hw_info_;
    
    // State
    SystemState state_;
    SimulatorConfig sim_config_;
    std::atomic<bool> running_{false};
    bool simulator_mode_{true};
    
    // Threads
    std::thread t0_supervisor_;
    std::thread t1_sensors_;
    std::thread t2_events_;
    std::thread t3_reflex_;
    std::thread t4_rules_;
    std::thread t5_mavlink_;
    std::thread t6_camera_;
    std::thread t7_api_;
    
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
    void mavlink_loop();        // T5: 50 Hz
    void camera_loop();         // T6: 15 FPS
    void api_bridge_loop();     // T7: 30 Hz
    
    // Setup default rules
    void setup_default_reflexes();
    void setup_default_rules();
    
    // Flight physics simulation
    void update_flight_physics(float dt);
    
    // Helper: precise sleep to target frequency
    static void rate_sleep(TimePoint& next_wake, int hz);
    
    // Helper: update thread stats
    void update_thread_stats(int id, TimePoint start, TimePoint end, int target_hz);

    // --- Thread synchronization (added for data-race elimination) ---
    std::atomic<uint64_t>        safety_snapshot_{0};  // packed SafetySnapshot
    mutable std::mutex           sensor_mutex_;          // Zone B: IMU/baro/GPS writes (T1, T5)
    mutable std::mutex           slow_mutex_;            // Zone C: battery/motor/flight_mode (T0)
    mutable std::mutex           motor_mutex_;           // Zone D: motor[] writes

    // Helper: atomically update the safety snapshot (implemented in runtime.cpp)
    void update_safety_snapshot() noexcept;
};

} // namespace jtzero
