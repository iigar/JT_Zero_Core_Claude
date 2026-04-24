#pragma once
/**
 * JT-Zero MAVLink Interface
 * 
 * Handles communication with flight controllers via MAVLink protocol.
 * Sends: VISION_POSITION_ESTIMATE, ODOMETRY, OPTICAL_FLOW_RAD
 * Receives: HEARTBEAT, ATTITUDE, GPS, etc.
 * 
 * Transport options:
 *   - SERIAL: /dev/ttyAMA0 or /dev/serial0 (hardware UART on Pi)
 *   - UDP: localhost:14550 (for SITL / network FC)
 *   - SIMULATED: in-memory (development/testing)
 */

#include "jt_zero/common.h"
#include "jt_zero/camera.h"
#include <atomic>
#include <array>

namespace jtzero {

// ─── MAVLink Message Types (subset) ─────────────────────

enum class MAVMsgType : uint16_t {
    HEARTBEAT                  = 0,
    ATTITUDE                   = 30,
    GLOBAL_POSITION_INT        = 33,
    GPS_RAW_INT                = 24,
    SCALED_IMU                 = 26,
    RC_CHANNELS                = 65,
    VFR_HUD                    = 74,
    COMMAND_LONG               = 76,
    VISION_POSITION_ESTIMATE   = 102,
    ODOMETRY                   = 331,
    OPTICAL_FLOW_RAD           = 106,
    STATUSTEXT                 = 253,
};

inline const char* mavmsg_str(MAVMsgType t) {
    switch(t) {
        case MAVMsgType::HEARTBEAT: return "HEARTBEAT";
        case MAVMsgType::ATTITUDE: return "ATTITUDE";
        case MAVMsgType::GLOBAL_POSITION_INT: return "GLOBAL_POS";
        case MAVMsgType::GPS_RAW_INT: return "GPS_RAW";
        case MAVMsgType::SCALED_IMU: return "SCALED_IMU";
        case MAVMsgType::RC_CHANNELS: return "RC_CHANNELS";
        case MAVMsgType::VFR_HUD: return "VFR_HUD";
        case MAVMsgType::COMMAND_LONG: return "CMD_LONG";
        case MAVMsgType::VISION_POSITION_ESTIMATE: return "VISION_POS";
        case MAVMsgType::ODOMETRY: return "ODOMETRY";
        case MAVMsgType::OPTICAL_FLOW_RAD: return "OPT_FLOW_RAD";
        case MAVMsgType::STATUSTEXT: return "STATUSTEXT";
        default: return "UNKNOWN";
    }
}

// ─── MAVLink Connection State ────────────────────────────

enum class MAVLinkState : uint8_t {
    DISCONNECTED = 0,
    CONNECTING,
    CONNECTED,
    LOST
};

inline const char* mavstate_str(MAVLinkState s) {
    switch(s) {
        case MAVLinkState::DISCONNECTED: return "DISCONNECTED";
        case MAVLinkState::CONNECTING: return "CONNECTING";
        case MAVLinkState::CONNECTED: return "CONNECTED";
        case MAVLinkState::LOST: return "LOST";
        default: return "UNKNOWN";
    }
}

// ─── MAVLink Transport ───────────────────────────────────

enum class MAVTransport : uint8_t {
    SIMULATED = 0,
    SERIAL,      // Hardware UART (/dev/ttyAMA0, /dev/serial0)
    UDP,         // UDP socket (SITL or network FC)
};

inline const char* mavtransport_str(MAVTransport t) {
    switch(t) {
        case MAVTransport::SIMULATED: return "SIMULATED";
        case MAVTransport::SERIAL: return "SERIAL";
        case MAVTransport::UDP: return "UDP";
        default: return "UNKNOWN";
    }
}

// ─── MAVLink Messages ────────────────────────────────────

struct MAVVisionPositionEstimate {
    uint64_t usec{0};
    float x{0}, y{0}, z{0};             // m, NED
    float roll{0}, pitch{0}, yaw{0};     // rad
    float covariance[21]{};              // row-major upper triangle
};

struct MAVOdometry {
    uint64_t time_usec{0};
    float x{0}, y{0}, z{0};             // m
    float vx{0}, vy{0}, vz{0};          // m/s
    float rollspeed{0}, pitchspeed{0}, yawspeed{0}; // rad/s
    float q[4]{1, 0, 0, 0};             // quaternion (w,x,y,z)
    uint8_t frame_id{0};                // MAV_FRAME
    uint8_t child_frame_id{0};
    float quality{0};                    // 0-1
};

struct MAVOpticalFlowRad {
    uint64_t time_usec{0};
    float integrated_x{0};               // rad
    float integrated_y{0};               // rad
    float integrated_xgyro{0};           // rad
    float integrated_ygyro{0};           // rad
    float integrated_zgyro{0};           // rad
    uint32_t integration_time_us{0};
    float distance{0};                    // m
    int16_t temperature{0};               // cdeg
    uint8_t quality{0};                   // 0-255
    uint32_t time_delta_distance_us{0};
};

// ─── MAVLink Interface Stats ─────────────────────────────

struct MAVLinkStats {
    MAVLinkState state{MAVLinkState::DISCONNECTED};
    MAVTransport transport{MAVTransport::SIMULATED};
    uint32_t messages_sent{0};
    uint32_t messages_received{0};
    uint32_t heartbeats_received{0};
    size_t   bytes_received{0};
    size_t   bytes_sent{0};
    size_t   crc_errors{0};
    uint32_t errors{0};
    float    link_quality{0};            // 0-1
    uint64_t last_heartbeat_us{0};
    uint8_t  system_id{1};
    uint8_t  component_id{191};          // MAV_COMP_ID_ONBOARD_COMPUTER
    // FC info
    uint8_t  fc_system_id{1};
    uint8_t  fc_autopilot{0};            // MAV_AUTOPILOT type
    uint8_t  fc_type{0};                 // MAV_TYPE
    bool     fc_armed{false};
    char     fc_firmware[32]{};
    char     transport_info[64]{};       // e.g. "/dev/ttyAMA0@115200" or "127.0.0.1:14550"
};

// ─── Parsed FC Telemetry ─────────────────────────────────

struct FCTelemetry {
    // HEARTBEAT (msg 0)
    uint8_t  fc_type{0};          // MAV_TYPE (2=QUADROTOR)
    uint8_t  fc_autopilot{0};     // MAV_AUTOPILOT (3=ArduPilot)
    uint8_t  base_mode{0};
    uint32_t custom_mode{0};
    uint8_t  system_status{0};
    bool     armed{false};
    bool     heartbeat_valid{false};
    
    // ATTITUDE (msg 30)
    float roll{0};                // rad
    float pitch{0};               // rad
    float yaw{0};                 // rad
    float rollspeed{0};           // rad/s
    float pitchspeed{0};          // rad/s
    float yawspeed{0};            // rad/s
    bool  attitude_valid{false};
    
    // SCALED_IMU (msg 26)
    float acc_x{0}, acc_y{0}, acc_z{0};     // m/s^2
    float gyro_x{0}, gyro_y{0}, gyro_z{0};  // rad/s
    float mag_x{0}, mag_y{0}, mag_z{0};     // gauss
    bool  imu_valid{false};
    
    // SCALED_PRESSURE (msg 29)
    float pressure{0};            // hPa
    float temperature{0};         // Celsius
    bool  baro_valid{false};
    
    // GPS_RAW_INT (msg 24)
    double gps_lat{0};            // degrees
    double gps_lon{0};            // degrees
    float  gps_alt{0};            // m
    float  gps_speed{0};          // m/s
    uint8_t gps_fix{0};           // 0=none, 2=2D, 3=3D
    uint8_t gps_sats{0};
    bool   gps_valid{false};
    
    // VFR_HUD (msg 74)
    float airspeed{0};            // m/s
    float groundspeed{0};         // m/s
    float alt{0};                 // m
    float climb{0};               // m/s
    int16_t heading{0};           // deg (0-360)
    uint16_t throttle{0};         // %
    bool  hud_valid{false};
    
    // SYS_STATUS (msg 1)
    float battery_voltage{0};     // V
    float battery_current{0};     // A
    int8_t battery_remaining{-1}; // %
    bool  status_valid{false};
    
    // Counters
    uint32_t msg_count{0};
    uint64_t last_update_us{0};
    
    // RC_CHANNELS (msg 65)
    uint16_t rc_channels[18]{};
    uint8_t  rc_chancount{0};
    uint8_t  rc_rssi{0};
    bool     rc_valid{false};
};

// ─── RAII Spinlock Guard ─────────────────────────────────

struct ScopedSpinLock {
    std::atomic<bool>& lock_;
    ScopedSpinLock(std::atomic<bool>& lock) : lock_(lock) {
        while (lock_.exchange(true, std::memory_order_acquire)) {}
    }
    ~ScopedSpinLock() {
        lock_.store(false, std::memory_order_release);
    }
    ScopedSpinLock(const ScopedSpinLock&) = delete;
    ScopedSpinLock& operator=(const ScopedSpinLock&) = delete;
};

// ─── MAVLink Interface ──────────────────────────────────

class MAVLinkInterface {
public:
    MAVLinkInterface();
    
    // Lifecycle
    bool initialize(bool simulated = true);
    
    // Real transport initialization
    bool initialize_serial(const char* device = "/dev/ttyAMA0", int baudrate = 115200);
    bool initialize_serial_auto_baud(const char* device);
    bool initialize_udp(const char* host = "127.0.0.1", int port = 14550);
    
    // Auto-detect: try serial first, then UDP, fallback to simulation
    MAVTransport auto_detect_transport();
    
    void shutdown();
    
    // Send messages to FC
    bool send_vision_position(const MAVVisionPositionEstimate& msg);
    bool send_odometry(const MAVOdometry& msg);
    bool send_optical_flow_rad(const MAVOpticalFlowRad& msg);
    bool send_heartbeat();
    
    // Send STATUSTEXT message (severity: 0=EMERGENCY..6=INFO)
    bool send_statustext(uint8_t severity, const char* text);

    // Reset accumulated VO pose to (0,0,0) — call on vo_reset / ARM
    void reset_vo_pose() { vo_pose_x_ = 0.0f; vo_pose_y_ = 0.0f; }
    
    // Build messages from runtime state
    MAVVisionPositionEstimate build_vision_position(const SystemState& state, const VOResult& vo);
    MAVOdometry build_odometry(const SystemState& state, const VOResult& vo);
    MAVOpticalFlowRad build_optical_flow_rad(const OpticalFlowData& flow, const VOResult& vo);
    
    // Process tick (send periodic messages, check connection)
    void tick(const SystemState& state, const VOResult& vo);
    
    // State
    MAVLinkState connection_state() const { return state_; }
    MAVTransport transport() const { return transport_; }
    MAVLinkStats get_stats() const;
    bool is_connected() const { return state_ == MAVLinkState::CONNECTED; }
    
    // Parsed FC telemetry (thread-safe read via RAII spinlock)
    FCTelemetry get_fc_telemetry() const {
        ScopedSpinLock guard(telem_lock_);
        return fc_telem_;
    }
    bool has_fc_data() const { return fc_telem_.heartbeat_valid; }
    
    // Diagnostic: track unique message IDs (public for API access)
    uint32_t diag_msg_ids_[32]{};
    size_t   diag_unique_count_{0};
    
private:
    MAVLinkState state_{MAVLinkState::DISCONNECTED};
    MAVTransport transport_{MAVTransport::SIMULATED};
    bool simulated_{true};
    
    // Serial transport
    int serial_fd_{-1};
    int serial_baud_{115200};
    char detected_serial_[32]{}; // auto-detected device path
    
    // UDP transport
    int udp_fd_{-1};
    char udp_host_[64]{};
    int udp_port_{14550};
    
    // Transport info string
    char transport_info_[64]{};
    
    // Raw send/receive (transport-agnostic)
    bool send_raw(const uint8_t* data, size_t len);
    int recv_raw(uint8_t* buf, size_t max_len);
    
    // Stats
    std::atomic<uint32_t> msgs_sent_{0};
    std::atomic<uint32_t> msgs_received_{0};
    std::atomic<uint32_t> heartbeats_received_{0};
    std::atomic<uint32_t> errors_{0};
    uint64_t last_heartbeat_us_{0};
    uint64_t last_vision_us_{0};
    uint32_t heartbeat_count_{0};
    size_t   bytes_received_{0};
    size_t   bytes_sent_{0};
    size_t   crc_errors_{0};
    uint32_t heartbeats_seen_{0};  // All heartbeats including filtered
    bool     diag_raw_logged_{false};
    
    // Accumulated VO local pose (NED frame, relative to home)
    float vo_pose_x_{0};
    float vo_pose_y_{0};
    
    // Simulated FC state
    uint8_t fc_system_id_{1};
    bool    fc_armed_{false};
    
    // Parsed FC telemetry
    FCTelemetry fc_telem_;
    mutable std::atomic<bool> telem_lock_{false};  // spinlock for copy
    
    // MAVLink frame parser
    static constexpr size_t RX_BUF_SIZE = 2048;
    uint8_t rx_buf_[RX_BUF_SIZE]{};
    size_t  rx_head_{0};
    size_t  rx_tail_{0};
    uint8_t seq_{0};               // MAVLink frame sequence counter
    bool    streams_requested_{false};  // Have we requested data streams?
    int     stream_request_count_{0};  // Number of times streams requested
    uint64_t last_stream_request_us_{0};  // Last request timestamp
    
    // Process incoming bytes: parse MAVLink frames and update fc_telem_
    void process_incoming();
    
    // Parse a single MAVLink frame payload by message ID
    void handle_message(uint32_t msg_id, const uint8_t* payload, uint8_t len, uint8_t sysid);
    
    // Send a MAVLink v2 frame with CRC
    bool send_mavlink_v2(uint32_t msg_id, const uint8_t* payload, uint8_t len, uint8_t crc_extra);
    
    // Request data streams from FC (called once after connection)
    void request_data_streams();
    
    // Diagnostic: track unique message IDs
    void log_msg_id(uint32_t msg_id);
    
    // Connection monitoring
    static constexpr uint64_t HEARTBEAT_TIMEOUT_US = 3'000'000; // 3 seconds
    void check_connection();
};

} // namespace jtzero
