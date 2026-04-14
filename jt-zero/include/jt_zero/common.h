#pragma once
/**
 * JT-Zero Common Types & Lock-Free Data Structures
 * 
 * Core primitives for the robotics runtime:
 * - Lock-free SPSC ring buffer (single-producer single-consumer)
 * - Fixed-size memory pool
 * - Timestamp utilities
 * - Common sensor data types
 * 
 * Target: Raspberry Pi Zero 2 W (ARM Cortex-A53)
 * Constraints: No dynamic allocation in realtime paths
 */

#include <atomic>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <array>
#include <cstdio>

namespace jtzero {

// ─── Timing ──────────────────────────────────────────────
using SteadyClock = std::chrono::steady_clock;
using TimePoint   = SteadyClock::time_point;
using Duration    = std::chrono::microseconds;

inline uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        SteadyClock::now().time_since_epoch()
    ).count();
}

inline double now_sec() {
    return static_cast<double>(now_us()) / 1'000'000.0;
}

// ─── Lock-Free SPSC Ring Buffer ──────────────────────────
// Single-Producer Single-Consumer, cache-line aligned
// Zero allocation after construction
template<typename T, size_t Capacity>
class RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr size_t MASK = Capacity - 1;

    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) std::array<T, Capacity> buffer_{};

public:
    RingBuffer() = default;

    // Non-copyable, non-movable
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    bool push(const T& item) noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t next = (h + 1) & MASK;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false; // Full
        }
        buffer_[h] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& item) noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) {
            return false; // Empty
        }
        item = buffer_[t];
        tail_.store((t + 1) & MASK, std::memory_order_release);
        return true;
    }

    size_t size() const noexcept {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & MASK;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == 
               tail_.load(std::memory_order_acquire);
    }

    static constexpr size_t capacity() noexcept { return Capacity; }

    void clear() noexcept {
        head_.store(0, std::memory_order_release);
        tail_.store(0, std::memory_order_release);
    }
};

// ─── Fixed Memory Pool (Lock-Free Free-List) ─────────────
// O(1) allocate/deallocate, thread-safe via atomic CAS.
//
// ABA prevention: free_head_ packs a 32-bit block index and a 32-bit
// generation counter into one atomic<uint64_t>.  Every successful CAS
// increments the generation, so a recycled block pointer is never
// indistinguishable from the original — the ABA scenario is impossible.
//
// Layout of the packed uint64_t (little-endian view):
//   bits 63-32  generation counter (increments on every allocate CAS)
//   bits 31- 0  block index  (INVALID_IDX == UINT32_MAX means empty)
template<typename T, size_t PoolSize>
class MemoryPool {
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "uint64_t atomic must be lock-free on this platform");

    static constexpr uint32_t INVALID_IDX = UINT32_MAX;

    // TaggedHead — the logical type stored inside free_head_.
    struct TaggedHead {
        uint32_t index; // block index, or INVALID_IDX when list is empty
        uint32_t gen;   // generation counter, wraps around safely
    };

    static uint64_t pack(TaggedHead h) noexcept {
        return (static_cast<uint64_t>(h.gen) << 32) | h.index;
    }

    static TaggedHead unpack(uint64_t raw) noexcept {
        return {static_cast<uint32_t>(raw & 0xFFFF'FFFFu),
                static_cast<uint32_t>(raw >> 32)};
    }

    struct Block {
        T        data;
        uint32_t next{INVALID_IDX}; // plain — only read while block is off
                                    // the free list (owner holds it), or
                                    // during the constructor
    };

    std::array<Block, PoolSize> pool_{};
    alignas(64) std::atomic<uint64_t> free_head_; // packed TaggedHead
    std::atomic<size_t> alloc_count_{0};

public:
    MemoryPool() {
        // Initialize free list: 0 -> 1 -> 2 -> ... -> PoolSize-1 -> INVALID
        for (size_t i = 0; i < PoolSize - 1; ++i) {
            pool_[i].next = static_cast<uint32_t>(i + 1);
        }
        pool_[PoolSize - 1].next = INVALID_IDX;
        // Head starts at index 0, generation 0
        free_head_.store(pack({0u, 0u}), std::memory_order_relaxed);
    }

    T* allocate() noexcept {
        uint64_t old_raw = free_head_.load(std::memory_order_acquire);
        while (true) {
            TaggedHead old = unpack(old_raw);
            if (old.index == INVALID_IDX) return nullptr; // Pool exhausted

            // Safe to read next: this block is at the head of the free list
            // and we have a snapshot of old.index.  Another thread may also
            // be reading pool_[old.index].next concurrently, but both are
            // reads — no data race.  If the CAS below fails, old_raw is
            // refreshed by compare_exchange_weak (failure path).
            uint32_t next = pool_[old.index].next;

            // Increment generation to defeat ABA: even if old.index is
            // freed and re-linked before our CAS, the generation will differ.
            TaggedHead new_head{next, old.gen + 1};
            if (free_head_.compare_exchange_weak(old_raw, pack(new_head),
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                alloc_count_.fetch_add(1, std::memory_order_relaxed);
                return &pool_[old.index].data;
            }
            // CAS failed — old_raw was refreshed; retry with new snapshot
        }
    }

    void deallocate(T* ptr) noexcept {
        // Recover block index from pointer arithmetic
        auto* base   = reinterpret_cast<char*>(&pool_[0]);
        auto* target = reinterpret_cast<char*>(ptr);
        size_t byte_offset = static_cast<size_t>(target - base);
        size_t idx = byte_offset / sizeof(Block);

        if (idx >= PoolSize) return; // Invalid pointer — silently ignore

        uint64_t old_raw = free_head_.load(std::memory_order_acquire);
        while (true) {
            TaggedHead old = unpack(old_raw);
            // Link this block in front of the current head
            pool_[idx].next = old.index;
            // Generation also increments on deallocate to be safe
            TaggedHead new_head{static_cast<uint32_t>(idx), old.gen + 1};
            if (free_head_.compare_exchange_weak(old_raw, pack(new_head),
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                break;
            }
            // CAS failed — old_raw refreshed; retry
        }

        alloc_count_.fetch_sub(1, std::memory_order_relaxed);
    }

    size_t used() const noexcept {
        return alloc_count_.load(std::memory_order_relaxed);
    }

    static constexpr size_t capacity() noexcept { return PoolSize; }
};

// ─── Sensor Data Types ───────────────────────────────────

struct IMUData {
    uint64_t timestamp_us{0};
    float gyro_x{0}, gyro_y{0}, gyro_z{0};   // rad/s
    float acc_x{0},  acc_y{0},  acc_z{0};     // m/s^2
    bool  valid{false};
};

struct BarometerData {
    uint64_t timestamp_us{0};
    float pressure{0};      // hPa
    float altitude{0};      // m
    float temperature{0};   // C
    bool  valid{false};
};

struct GPSData {
    uint64_t timestamp_us{0};
    double lat{0}, lon{0};  // degrees
    float  alt{0};          // m
    float  speed{0};        // m/s
    uint8_t satellites{0};
    uint8_t fix_type{0};    // 0=none, 2=2D, 3=3D
    bool   valid{false};
};

struct RangefinderData {
    uint64_t timestamp_us{0};
    float distance{0};      // m
    float signal_quality{0}; // 0-1
    bool  valid{false};
};

struct OpticalFlowData {
    uint64_t timestamp_us{0};
    float flow_x{0}, flow_y{0};  // rad/s
    uint8_t quality{0};           // 0-255
    float ground_distance{0};     // m
    bool  valid{false};
};

// ─── Event System ────────────────────────────────────────

enum class EventType : uint8_t {
    NONE = 0,
    // Sensor events
    SENSOR_IMU_UPDATE,
    SENSOR_BARO_UPDATE,
    SENSOR_GPS_UPDATE,
    SENSOR_RANGE_UPDATE,
    SENSOR_FLOW_UPDATE,
    // System events
    SYSTEM_STARTUP,
    SYSTEM_SHUTDOWN,
    SYSTEM_HEARTBEAT,
    SYSTEM_ERROR,
    SYSTEM_WARNING,
    // Flight events
    FLIGHT_ARM,
    FLIGHT_DISARM,
    FLIGHT_TAKEOFF,
    FLIGHT_LAND,
    FLIGHT_RTL,
    FLIGHT_HOLD,
    FLIGHT_ALTITUDE_REACHED,
    FLIGHT_OBSTACLE_DETECTED,
    // Camera events
    CAMERA_FRAME_READY,
    CAMERA_VO_UPDATE,
    // MAVLink events
    MAVLINK_CONNECTED,
    MAVLINK_DISCONNECTED,
    MAVLINK_HEARTBEAT,
    // Command events
    CMD_USER,
    CMD_API,
    // Custom
    CUSTOM = 200
};

inline const char* event_type_str(EventType t) {
    switch(t) {
        case EventType::NONE: return "NONE";
        case EventType::SENSOR_IMU_UPDATE: return "IMU_UPDATE";
        case EventType::SENSOR_BARO_UPDATE: return "BARO_UPDATE";
        case EventType::SENSOR_GPS_UPDATE: return "GPS_UPDATE";
        case EventType::SENSOR_RANGE_UPDATE: return "RANGE_UPDATE";
        case EventType::SENSOR_FLOW_UPDATE: return "FLOW_UPDATE";
        case EventType::SYSTEM_STARTUP: return "SYS_STARTUP";
        case EventType::SYSTEM_SHUTDOWN: return "SYS_SHUTDOWN";
        case EventType::SYSTEM_HEARTBEAT: return "SYS_HEARTBEAT";
        case EventType::SYSTEM_ERROR: return "SYS_ERROR";
        case EventType::SYSTEM_WARNING: return "SYS_WARNING";
        case EventType::FLIGHT_ARM: return "FLIGHT_ARM";
        case EventType::FLIGHT_DISARM: return "FLIGHT_DISARM";
        case EventType::FLIGHT_TAKEOFF: return "FLIGHT_TAKEOFF";
        case EventType::FLIGHT_LAND: return "FLIGHT_LAND";
        case EventType::FLIGHT_RTL: return "FLIGHT_RTL";
        case EventType::FLIGHT_HOLD: return "FLIGHT_HOLD";
        case EventType::FLIGHT_ALTITUDE_REACHED: return "ALT_REACHED";
        case EventType::FLIGHT_OBSTACLE_DETECTED: return "OBSTACLE";
        case EventType::CAMERA_FRAME_READY: return "CAM_FRAME";
        case EventType::CAMERA_VO_UPDATE: return "VO_UPDATE";
        case EventType::MAVLINK_CONNECTED: return "MAV_CONNECTED";
        case EventType::MAVLINK_DISCONNECTED: return "MAV_DISCONNECTED";
        case EventType::MAVLINK_HEARTBEAT: return "MAV_HEARTBEAT";
        case EventType::CMD_USER: return "CMD_USER";
        case EventType::CMD_API: return "CMD_API";
        case EventType::CUSTOM: return "CUSTOM";
        default: return "UNKNOWN";
    }
}

struct Event {
    uint64_t  timestamp_us{0};
    EventType type{EventType::NONE};
    uint8_t   priority{0};       // 0=lowest, 255=highest
    uint8_t   source_id{0};
    float     data[4]{0};        // Generic payload
    char      message[64]{};     // Optional text

    void set_message(const char* msg) {
        std::strncpy(message, msg, sizeof(message) - 1);
        message[sizeof(message) - 1] = '\0';
    }
};

// ─── System State ────────────────────────────────────────

enum class FlightMode : uint8_t {
    IDLE = 0,
    ARMED,
    TAKEOFF,
    HOVER,
    NAVIGATE,
    LAND,
    RTL,
    EMERGENCY
};

inline const char* flight_mode_str(FlightMode m) {
    switch(m) {
        case FlightMode::IDLE: return "IDLE";
        case FlightMode::ARMED: return "ARMED";
        case FlightMode::TAKEOFF: return "TAKEOFF";
        case FlightMode::HOVER: return "HOVER";
        case FlightMode::NAVIGATE: return "NAVIGATE";
        case FlightMode::LAND: return "LAND";
        case FlightMode::RTL: return "RTL";
        case FlightMode::EMERGENCY: return "EMERGENCY";
        default: return "UNKNOWN";
    }
}

struct SystemState {
    FlightMode flight_mode{FlightMode::IDLE};
    bool  armed{false};
    float battery_voltage{12.6f};
    float battery_percent{100.0f};
    float cpu_usage{0.0f};
    float ram_usage_mb{0.0f};
    float cpu_temp{40.0f};
    uint32_t uptime_sec{0};
    uint32_t event_count{0};
    uint32_t error_count{0};

    // Latest sensor data
    IMUData          imu;
    BarometerData    baro;
    GPSData          gps;
    RangefinderData  range;
    OpticalFlowData  flow;
    
    // Attitude (derived)
    float roll{0}, pitch{0}, yaw{0};     // degrees
    float altitude_agl{0};                // m above ground
    float vx{0}, vy{0}, vz{0};           // m/s body frame
    
    // Position (NED, relative to home)
    float pos_n{0}, pos_e{0}, pos_d{0};  // m, North-East-Down
    float target_altitude{10.0f};         // m, for takeoff/hover
    
    // Motor output (0-1)
    float motor[4]{0, 0, 0, 0};
};

// ─── Simulator Config (tuneable at runtime) ──────────────

struct SimulatorConfig {
    float wind_speed{0.0f};       // m/s
    float wind_direction{0.0f};   // degrees
    float sensor_noise{1.0f};     // noise multiplier (0=none, 1=default)
    float battery_drain{1.0f};    // drain rate multiplier
    float gravity{9.81f};
    float mass_kg{1.2f};          // drone mass
    float max_thrust{20.0f};      // N, total
    float drag_coeff{0.3f};
    bool  turbulence{false};
};

// ─── Thread Configuration ────────────────────────────────

struct ThreadConfig {
    const char* name;
    int   target_hz;
    int   priority;  // RT priority (higher = more important)
    int   cpu_core;  // CPU affinity (-1 = any)
};

constexpr ThreadConfig THREAD_CONFIGS[] = {
    {"T0_Supervisor",   10,   90,  0},
    {"T1_Sensors",     200,   95,  1},
    {"T2_Events",      200,   85,  2},
    {"T3_Reflex",      200,   98,  2},  // Highest priority
    {"T4_Rules",        20,   70,  3},
    {"T5_MAVLink",      50,   80,  1},
    {"T6_Camera",       15,   60,  3},
    {"T7_API",          30,   50, -1},
};

} // namespace jtzero
