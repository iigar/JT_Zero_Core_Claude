#pragma once
/**
 * JT-Zero Camera Pipeline
 * 
 * Abstract camera sources with simulated implementations.
 * Supports: Raspberry Pi CSI camera, USB cameras, IP cameras
 * 
 * Hardware profiles for different Pi models:
 *   Pi Zero 2W: 320x240 @ 15fps (default)
 *   Pi 4:       640x480 @ 30fps
 *   Pi 5:       800x600 @ 30fps
 * 
 * Adaptive VO parameters based on altitude + hover yaw correction.
 */

#include "jt_zero/common.h"
#include <cstdint>
#include <array>
#include <atomic>
#include <mutex>

namespace jtzero {

// ─── Frame Data ──────────────────────────────────────────

struct FrameInfo {
    uint64_t timestamp_us{0};
    uint32_t frame_id{0};
    uint16_t width{0};
    uint16_t height{0};
    uint8_t  channels{1};       // 1=grayscale, 3=RGB
    float    fps_actual{0};
    bool     valid{false};
};

// Default dimensions (Pi Zero 2W profile)
static constexpr uint16_t FRAME_WIDTH  = 320;
static constexpr uint16_t FRAME_HEIGHT = 240;

// Maximum frame buffer (supports up to Pi 5 profile: 800x600)
static constexpr uint16_t MAX_FRAME_WIDTH  = 800;
static constexpr uint16_t MAX_FRAME_HEIGHT = 600;
static constexpr size_t   MAX_FRAME_SIZE   = MAX_FRAME_WIDTH * MAX_FRAME_HEIGHT;
static constexpr size_t   FRAME_SIZE       = MAX_FRAME_SIZE;

struct FrameBuffer {
    alignas(64) uint8_t data[FRAME_SIZE]{};
    FrameInfo info;
};

// ─── Platform Config (auto-detected at startup) ─────────
// Determines camera resolution and focal length. NOT switchable at runtime.

enum class PlatformType : uint8_t {
    PI_ZERO_2W = 0,
    PI_4       = 1,
    PI_5       = 2
};

inline const char* platform_str(PlatformType t) {
    switch(t) {
        case PlatformType::PI_ZERO_2W: return "PI_ZERO_2W";
        case PlatformType::PI_4:       return "PI_4";
        case PlatformType::PI_5:       return "PI_5";
        default: return "UNKNOWN";
    }
}

struct PlatformConfig {
    const char*  name;
    PlatformType type;
    uint16_t frame_width;
    uint16_t frame_height;
    float    focal_length_px;
    float    target_fps;
};

static constexpr PlatformConfig PLATFORMS[] = {
    {"Pi Zero 2W", PlatformType::PI_ZERO_2W, 640, 480, 554.0f, 15.0f},
    {"Pi 4",       PlatformType::PI_4,      1280, 720, 830.0f, 30.0f},
    {"Pi 5",       PlatformType::PI_5,      1280, 960, 1108.0f, 30.0f},
};
static constexpr size_t NUM_PLATFORMS = sizeof(PLATFORMS) / sizeof(PLATFORMS[0]);

// ─── VO Mode (switchable at runtime) ─────────────────────
// Only algorithm parameters. Does NOT change camera resolution.

enum class VOModeType : uint8_t {
    LIGHT       = 0,   // Economy: fewer features, less CPU
    BALANCED    = 1,   // Default: good tracking, moderate CPU
    PERFORMANCE = 2    // Maximum accuracy: most features, high CPU
};

inline const char* vo_mode_str(VOModeType m) {
    switch(m) {
        case VOModeType::LIGHT:       return "LIGHT";
        case VOModeType::BALANCED:    return "BALANCED";
        case VOModeType::PERFORMANCE: return "PERFORMANCE";
        default: return "UNKNOWN";
    }
}

struct VOMode {
    const char* name;
    VOModeType  type;
    uint8_t  fast_threshold;
    int      lk_window_size;
    int      lk_iterations;
    size_t   max_features;
};

static constexpr VOMode VO_MODES[] = {
    {"Light",       VOModeType::LIGHT,       30, 5, 4, 100},
    {"Balanced",    VOModeType::BALANCED,     25, 7, 5, 180},
    {"Performance", VOModeType::PERFORMANCE,  20, 9, 6, 250},
};
static constexpr size_t NUM_VO_MODES = sizeof(VO_MODES) / sizeof(VO_MODES[0]);

// ─── Altitude-Adaptive VO Parameters ─────────────────────
// Automatically adjusts VO algorithm settings based on barometric altitude.
// Smooth interpolation between zone boundaries.

enum class AltitudeZone : uint8_t {
    LOW     = 0,   // 0-10m:   aggressive tracking, small features
    MEDIUM  = 1,   // 10-50m:  balanced
    HIGH    = 2,   // 50-200m: conservative, large features
    CRUISE  = 3    // 200m+:   maximum stability, biggest window
};

inline const char* altitude_zone_str(AltitudeZone z) {
    switch(z) {
        case AltitudeZone::LOW:    return "LOW";
        case AltitudeZone::MEDIUM: return "MEDIUM";
        case AltitudeZone::HIGH:   return "HIGH";
        case AltitudeZone::CRUISE: return "CRUISE";
        default: return "UNKNOWN";
    }
}

struct AdaptiveVOParams {
    AltitudeZone zone{AltitudeZone::LOW};
    uint8_t  fast_threshold{30};
    int      lk_window_size{5};
    int      lk_iterations{4};
    int      min_inliers{5};
    float    kalman_q{0.5f};       // process noise (m/s^2)
    float    kalman_r_base{0.3f};  // measurement noise base
    float    redetect_ratio{0.15f}; // re-detect if tracked < ratio * max
};

// Zone boundary altitudes (meters AGL)
static constexpr float ALT_ZONE_LOW_MAX     = 10.0f;
static constexpr float ALT_ZONE_MEDIUM_MAX  = 50.0f;
static constexpr float ALT_ZONE_HIGH_MAX    = 200.0f;

// ─── Hover Yaw Correction ────────────────────────────────
// Detects hovering state and corrects gyroscopic yaw drift
// by analyzing micro-movements of tracked features.

struct HoverState {
    bool  is_hovering{false};
    float hover_duration_sec{0};
    float yaw_drift_rate{0};         // rad/s, estimated gyro drift
    float corrected_yaw{0};          // rad, corrected yaw
    float micro_motion_avg{0};       // px, average feature displacement
    int   stable_frame_count{0};     // consecutive frames with low motion
    float accumulated_yaw_drift{0};  // rad, total drift correction applied
    // Fix 3: gyro bias estimated during confirmed hover (rad/s)
    // In true hover the drone doesn't rotate → any persistent gyro_z reading is bias
    float gyro_z_bias{0};

    // Thresholds
    static constexpr float HOVER_MOTION_THRESH = 0.5f;  // px, below = hovering
    static constexpr int   HOVER_MIN_FRAMES    = 30;     // frames before hover confirmed
    static constexpr float DRIFT_ALPHA         = 0.02f;  // EMA smoothing for drift rate
    static constexpr float BIAS_ALPHA          = 0.005f; // much slower EMA for bias (stable estimate)
};

// ─── Visual Odometry Result ──────────────────────────────

struct VOResult {
    uint64_t timestamp_us{0};
    // Position estimate (body-frame delta)
    float dx{0}, dy{0}, dz{0};         // m
    // Velocity estimate (Kalman-filtered)
    float vx{0}, vy{0}, vz{0};         // m/s
    // Rotation delta
    float droll{0}, dpitch{0}, dyaw{0}; // rad
    // Quality metrics
    uint16_t features_detected{0};
    uint16_t features_tracked{0};
    uint16_t inlier_count{0};           // features after outlier rejection
    float    tracking_quality{0};        // 0-1
    float    confidence{0};              // 0-1, combined quality metric for EKF
    float    position_uncertainty{0};    // meters, grows with drift
    bool     valid{false};
    
    // Platform (auto-detected, not switchable)
    uint8_t  platform{0};                // PlatformType
    
    // VO Mode (switchable at runtime)
    uint8_t  vo_mode{1};                 // VOModeType (default: BALANCED)
    
    // Adaptive altitude zone
    uint8_t  altitude_zone{0};           // AltitudeZone
    float    adaptive_fast_thresh{30};
    float    adaptive_lk_window{5};
    
    // Hover yaw correction
    bool     hover_detected{false};
    float    hover_duration{0};          // seconds
    float    yaw_drift_rate{0};          // rad/s estimated drift
    float    corrected_yaw{0};           // rad, corrected yaw
};

// ─── Feature Point ───────────────────────────────────────

struct FeaturePoint {
    float x{0}, y{0};          // pixel coordinates
    float response{0};         // corner strength
    bool  tracked{false};
};

static constexpr size_t MAX_FEATURES = 300;

// ─── CSI Sensor Variants ─────────────────────────────────
// Auto-detected via rpicam-hello --list-cameras

enum class CSISensorType : uint8_t {
    UNKNOWN    = 0,
    OV5647     = 1,   // Pi Camera v1 — 5MP, fixed focus, FOV 62°
    IMX219     = 2,   // Pi Camera v2 — 8MP, fixed focus, FOV 62°
    IMX477     = 3,   // Pi HQ Camera — 12.3MP, C/CS-mount lens
    IMX708     = 4,   // Pi Camera v3 — 12MP, autofocus, FOV 66°
    OV9281     = 5,   // Global shutter — 1MP, ideal for VO
    IMX296     = 6,   // Pi GS Camera — 1.6MP, global shutter
    OV64A40    = 7,   // Arducam 64MP
    IMX290     = 8,   // Sony STARVIS — 2MP, excellent low-light, 1/2.8"
    GENERIC    = 99,  // Unknown CSI sensor detected by rpicam-hello
};

struct CSISensorInfo {
    CSISensorType sensor;
    const char*   name;
    const char*   chip_id;      // string to match in rpicam-hello output
    uint16_t      max_width;
    uint16_t      max_height;
    float         default_focal_px;  // focal length at 640x480
    float         fov_h_deg;         // horizontal FOV
    bool          autofocus;
    bool          global_shutter;
};

static constexpr CSISensorInfo CSI_SENSORS[] = {
    {CSISensorType::OV5647,  "Pi Camera v1",      "ov5647",  2592, 1944, 554.0f,  62.2f, false, false},
    {CSISensorType::IMX219,  "Pi Camera v2",      "imx219",  3280, 2464, 620.0f,  62.2f, false, false},
    {CSISensorType::IMX477,  "Pi HQ Camera",      "imx477",  4056, 3040, 0.0f,    0.0f,  false, false},  // lens-dependent
    {CSISensorType::IMX708,  "Pi Camera v3",      "imx708",  4608, 2592, 630.0f,  66.0f, true,  false},
    {CSISensorType::OV9281,  "OV9281 GlobalShtr",  "ov9281",  1280, 800,  450.0f,  80.0f, false, true},
    {CSISensorType::IMX296,  "Pi GS Camera",      "imx296",  1456, 1088, 490.0f,  48.8f, false, true},
    {CSISensorType::OV64A40, "Arducam 64MP",      "ov64a40", 9248, 6944, 600.0f,  84.0f, true,  false},
    {CSISensorType::IMX290,  "IMX290 STARVIS",    "imx290",  1920, 1080, 400.0f,  82.0f, false, false},
};
static constexpr size_t NUM_CSI_SENSORS = sizeof(CSI_SENSORS) / sizeof(CSI_SENSORS[0]);

inline const char* csi_sensor_str(CSISensorType s) {
    for (size_t i = 0; i < NUM_CSI_SENSORS; ++i) {
        if (CSI_SENSORS[i].sensor == s) return CSI_SENSORS[i].name;
    }
    if (s == CSISensorType::GENERIC) return "Generic CSI";
    return "Unknown CSI";
}

// ─── Camera Source Interface ─────────────────────────────

enum class CameraType : uint8_t {
    NONE = 0,
    PI_CSI,      // Raspberry Pi CSI camera
    USB,         // USB webcam (V4L2)
    IP_STREAM,   // RTSP/HTTP IP camera
    SIMULATED    // Test pattern generator
};

inline const char* camera_type_str(CameraType t) {
    switch(t) {
        case CameraType::NONE: return "NONE";
        case CameraType::PI_CSI: return "PI_CSI";
        case CameraType::USB: return "USB";
        case CameraType::IP_STREAM: return "IP";
        case CameraType::SIMULATED: return "SIM";
        default: return "UNKNOWN";
    }
}

class CameraSource {
public:
    virtual ~CameraSource() = default;
    virtual bool open() = 0;
    virtual bool capture(FrameBuffer& frame) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;
    virtual CameraType type() const = 0;
    virtual const char* name() const = 0;
};

// ─── Simulated Camera ────────────────────────────────────
// Generates test patterns with moving features for VO testing

class SimulatedCamera : public CameraSource {
public:
    bool open() override;
    bool capture(FrameBuffer& frame) override;
    void close() override;
    bool is_open() const override { return open_; }
    CameraType type() const override { return CameraType::SIMULATED; }
    const char* name() const override { return "SimulatedCamera"; }

private:
    bool open_{false};
    uint32_t frame_counter_{0};
    uint64_t last_capture_us_{0};
    
    void generate_pattern(uint8_t* data, uint16_t w, uint16_t h, uint32_t frame);
};

// ─── Pi CSI Camera (via libcamera / V4L2) ────────────────
// Raspberry Pi Camera Module v2/v3 (CSI interface)
// Uses V4L2 /dev/video0 with libcamera-bridge

class PiCSICamera : public CameraSource {
public:
    bool open() override;
    bool capture(FrameBuffer& frame) override;
    void close() override;
    bool is_open() const override { return open_; }
    CameraType type() const override { return CameraType::PI_CSI; }
    const char* name() const override { return sensor_name_; }
    
    // Auto-detect: check if rpicam-hello can see ANY camera
    static bool detect();
    
    // Detect and identify the CSI sensor model
    // Returns known type or GENERIC (with raw name stored)
    static CSISensorType detect_sensor();
    
    // Raw sensor chip string from rpicam-hello (e.g. "imx283")
    static const char* detected_raw_name() { return raw_sensor_name_; }
    
    // Get detected sensor info (valid after detect_sensor())
    CSISensorType sensor_type() const { return sensor_type_; }
    const CSISensorInfo* sensor_info() const { return sensor_info_; }

private:
    bool open_{false};
    FILE* pipe_{nullptr};  // rpicam-vid subprocess pipe
    uint16_t cap_w_{0};
    uint16_t cap_h_{0};
    uint32_t frame_counter_{0};
    uint64_t last_capture_us_{0};
    CSISensorType sensor_type_{CSISensorType::UNKNOWN};
    const CSISensorInfo* sensor_info_{nullptr};
    char sensor_name_[48]{"PiCSI_rpicam"};
    static char raw_sensor_name_[64];  // raw chip id from rpicam-hello
};

// ─── USB Camera (via V4L2) ───────────────────────────────
// Generic USB webcam using Video4Linux2

class USBCamera : public CameraSource {
public:
    explicit USBCamera(const char* device = "/dev/video0") : device_(device) {}
    
    bool open() override;
    bool capture(FrameBuffer& frame) override;
    void close() override;
    bool is_open() const override { return open_; }
    CameraType type() const override { return CameraType::USB; }
    const char* name() const override { return "USB_V4L2"; }
    
    static bool detect(const char* device = "/dev/video0");

private:
    const char* device_;
    bool open_{false};
    int fd_{-1};
    uint16_t cap_w_{0};
    uint16_t cap_h_{0};
    uint32_t frame_counter_{0};
    uint64_t last_capture_us_{0};
    // V4L2 MMAP streaming
    static constexpr int MAX_V4L2_BUFS = 4;
    struct MappedBuffer { void* start{nullptr}; size_t length{0}; };
    MappedBuffer buffers_[MAX_V4L2_BUFS]{};
    int n_buffers_{0};
    bool streaming_{false};
};

// ─── FAST Corner Detector ────────────────────────────────
// Simplified FAST-9 corner detection for embedded use

class FASTDetector {
public:
    // Detect corners in grayscale frame
    // Returns number of features found
    int detect(const uint8_t* frame, uint16_t width, uint16_t height,
               FeaturePoint* features, size_t max_features,
               uint8_t threshold = 30);

private:
    // FAST-9 circle test (simplified)
    bool is_corner(const uint8_t* frame, uint16_t width,
                   int x, int y, uint8_t threshold) const;
};

// ─── Lucas-Kanade Optical Flow Tracker ───────────────────
// Sparse optical flow for feature tracking

class LKTracker {
public:
    // Track features from prev frame to curr frame.
    // Updates feature positions in-place, sets tracked flag.
    // hint_dx / hint_dy: optional per-feature initial flow guess (from IMU pre-integration).
    // Providing hints lets LK start the iterative search closer to the true displacement,
    // improving convergence rate and reducing lost tracks during fast rotation.
    int track(const uint8_t* prev_frame, const uint8_t* curr_frame,
              uint16_t width, uint16_t height,
              FeaturePoint* features, size_t feature_count,
              int window_size = 7, int iterations = 5,
              const float* hint_dx = nullptr, const float* hint_dy = nullptr);

private:
    // Compute image gradients at a point
    void compute_gradient(const uint8_t* frame, uint16_t width,
                         int x, int y, float& gx, float& gy) const;
};

// ─── Visual Odometry Estimator ───────────────────────────

class VisualOdometry {
public:
    VisualOdometry();
    
    // Process a new frame and compute VO estimate
    VOResult process(const FrameBuffer& frame, float ground_distance = 1.0f);
    
    // Set IMU data: accelerations for Kalman prediction + gyro for bias/consistency (call before process())
    void set_imu_hint(float ax, float ay, float gyro_z);

    // Accumulate gyroscope rotation between frames for LK initial-flow hints.
    // Called at 200 Hz from T1; consumed and reset each T6 frame in process().
    // Thread-safe via internal mutex.
    void accumulate_gyro(float gx, float gy, float gz, float dt);
    
    // Set current altitude for adaptive parameter adjustment
    void set_altitude(float altitude_agl);
    
    // Set current yaw from IMU/EKF for hover correction reference
    void set_yaw_hint(float yaw_rad);
    
    // Set hardware profile
    void set_platform(const PlatformConfig& platform);
    
    // Set VO mode (algorithm parameters only, no reset)
    void set_vo_mode(const VOMode& mode);
    
    // Reset state
    void reset();
    
    // Get current feature state
    size_t active_features() const { return active_count_; }
    
    // Get feature positions (read-only access)
    const std::array<FeaturePoint, MAX_FEATURES>& features() const { return features_; }
    size_t feature_count() const { return active_count_; }
    
    // Get accumulated pose
    float pose_x() const { return pose_x_; }
    float pose_y() const { return pose_y_; }
    float total_distance() const { return total_distance_; }
    
    // Get adaptive state
    const AdaptiveVOParams& adaptive_params() const { return adaptive_; }
    const HoverState& hover_state() const { return hover_; }
    const PlatformConfig& platform() const { return platform_; }
    const VOMode& vo_mode() const { return vo_mode_; }

    // Fix #60: VO velocity bias accessors
    float vx_bias() const { return vx_bias_; }
    float vy_bias() const { return vy_bias_; }
    void  clear_velocity_bias() { vx_bias_ = 0; vy_bias_ = 0; }

private:
    FASTDetector detector_;
    LKTracker    tracker_;
    
    // Double-buffer for frame storage (max size)
    alignas(64) uint8_t prev_frame_[FRAME_SIZE]{};
    bool has_prev_frame_{false};
    
    // Feature buffers (current + previous for displacement)
    std::array<FeaturePoint, MAX_FEATURES> features_;
    std::array<FeaturePoint, MAX_FEATURES> prev_features_;
    size_t active_count_{0};
    
    // Accumulated local pose (NED frame)
    float pose_x_{0}, pose_y_{0}, pose_z_{0};
    float total_distance_{0};
    
    uint64_t prev_timestamp_us_{0};
    
    // ── Long-range drift reduction ──
    
    // Kalman filter state per axis (simple 1D: [position_rate])
    float kf_vx_{0}, kf_vy_{0};               // filtered velocity
    float kf_vx_var_{1.0f}, kf_vy_var_{1.0f}; // velocity variance
    // Fix 1: pre-update velocity snapshot for correct imu_consistency computation
    float kf_vx_prev_{0}, kf_vy_prev_{0};

    // Fix 5: accumulated position variance from KF covariance (m²)
    // Grows each frame by kf_v*_var_ * dt², decays slowly when confidence is high
    float pose_var_x_{0.5f}, pose_var_y_{0.5f};

    // IMU hint for cross-validation and Kalman prediction
    float imu_ax_{0}, imu_ay_{0}, imu_gz_{0};
    bool  imu_hint_valid_{false};

    // Fix 6: pre-integrated gyro rotation between T6 frames (rad, body frame)
    // Written at 200 Hz by T1 via accumulate_gyro(); read+reset by T6 in process()
    struct PreIntState {
        float dgx{0}, dgy{0}, dgz{0};  // accumulated angle deltas (rad)
        bool  valid{false};
    };
    PreIntState preint_;
    std::mutex  preint_mtx_;
    
    // Running confidence metric
    float running_confidence_{0.5f};

    // Debounced vo_valid state: prevents single-frame confidence dips from
    // cutting off VISION_POSITION_ESTIMATE to EKF3 (which causes position drift).
    // vo_valid_stable_ → false only after INVALID_FRAMES_THRESH consecutive bad frames.
    // vo_valid_stable_ → true  after VALID_FRAMES_THRESH  consecutive good frames.
    static constexpr int INVALID_FRAMES_THRESH = 5;  // 5×66ms = 333ms to go invalid
    static constexpr int VALID_FRAMES_THRESH   = 2;  // 2×66ms = 133ms to restore valid
    int   invalid_frames_count_{0};
    int   valid_frames_count_{0};
    bool  vo_valid_stable_{false};
    
    // ── Platform + VO Mode ──
    PlatformConfig platform_;
    VOMode vo_mode_;
    
    // ── Altitude-Adaptive Parameters ──
    AdaptiveVOParams adaptive_;
    float current_altitude_{0};
    void update_adaptive_params();
    
    // ── Hover Yaw Correction ──
    HoverState hover_;
    float yaw_hint_{0};
    bool  yaw_hint_valid_{false};
    // Fix 3: gyro_z passed so hover can estimate IMU bias (in true hover rotation = 0)
    void update_hover_state(float median_dx, float median_dy, float dt, float gyro_z);

    // Fix #60: VO velocity bias (camera tilt + floor texture → systematic raw_v ≠ 0 in hover)
    // EMA estimated during stable hover, subtracted from raw_v BEFORE Kalman update.
    // Persists across reset() — physical calibration, clear only via clear_velocity_bias().
    float vx_bias_{0};
    float vy_bias_{0};
    static constexpr float VEL_BIAS_ALPHA      = 0.005f;  // ~30s settle at 15fps (fast: confirmed hover)
    static constexpr float VEL_BIAS_ALPHA_SLOW = 0.001f; // ~67s settle at 15fps (slow: general flight)
    static constexpr float VEL_BIAS_GATE       = 0.5f;   // m/s gate — skip during fast maneuvers
    static constexpr float MIN_HOVER_FOR_BIAS  = 5.0f;   // sec stable hover before fast-path fires
    static constexpr int   BIAS_MIN_BRIGHTNESS = 4;       // camera must see something (>dark floor bright=1-2)

    // Median + MAD computation helpers
    static float compute_median(float* arr, int n);
    static float compute_mad(float* arr, int n, float median);
};

// ─── VO Fallback Source ──────────────────────────────────
// When CSI camera loses tracking, VO can fall back to USB thermal

enum class VOSource : uint8_t {
    CSI_PRIMARY      = 0,   // Normal: CSI forward camera
    THERMAL_FALLBACK = 1,   // Fallback: USB thermal camera
};

inline const char* vo_source_str(VOSource s) {
    switch(s) {
        case VOSource::CSI_PRIMARY:      return "CSI_PRIMARY";
        case VOSource::THERMAL_FALLBACK: return "THERMAL_FALLBACK";
        default: return "UNKNOWN";
    }
}

// VO Fallback configuration thresholds
struct VOFallbackConfig {
    float    conf_drop_thresh{0.30f};      // switch to fallback below this (rolling avg)
    float    conf_recover_thresh{0.40f};   // return to CSI above this (brightness-based quality)
    uint16_t frames_to_switch{15};         // unused — Python uses rolling average now
    float    csi_probe_interval_s{2.0f};   // seconds between CSI recovery probes (fast for drone)
    float    thermal_focal_px{180.0f};     // default focal length for USB thermal at 640x480
};

// VO Fallback runtime state
struct VOFallbackState {
    VOSource  source{VOSource::CSI_PRIMARY};
    char      reason[64]{};                // human-readable switch reason
    uint16_t  low_conf_count{0};           // consecutive low-confidence frames
    float     fallback_start_time{0};      // when fallback started (runtime seconds)
    float     fallback_duration{0};        // seconds in fallback mode
    float     last_csi_probe_time{0};      // last time CSI was probed for recovery
    float     last_csi_probe_conf{0};      // confidence from last CSI probe
    uint32_t  total_switches{0};           // total number of switches since boot
};

// ─── Camera Slot (multi-camera support) ──────────────────
// PRIMARY = main camera for VO (CSI forward)
// SECONDARY = auxiliary camera (USB thermal downward), on-demand

enum class CameraSlot : uint8_t {
    PRIMARY   = 0,   // CSI forward — always active, feeds VO
    SECONDARY = 1,   // USB thermal downward — on-demand capture
    MAX_SLOTS = 2
};

inline const char* camera_slot_str(CameraSlot s) {
    switch(s) {
        case CameraSlot::PRIMARY:   return "PRIMARY";
        case CameraSlot::SECONDARY: return "SECONDARY";
        default: return "UNKNOWN";
    }
}

// ─── Camera Slot Info ────────────────────────────────────

struct CameraSlotInfo {
    CameraSlot  slot{CameraSlot::PRIMARY};
    CameraType  camera_type{CameraType::NONE};
    bool        camera_open{false};
    bool        active{false};       // currently capturing
    uint32_t    frame_count{0};
    float       fps_actual{0};
    uint16_t    width{0};
    uint16_t    height{0};
    char        label[48]{};         // "IMX219 (VO)", "Thermal USB"
    char        device[64]{};        // "/dev/video0", "rpicam-vid"
    // CSI-specific
    uint8_t     csi_sensor{0};       // CSISensorType
    char        sensor_name[32]{};   // "Pi Camera v2"
};

// ─── Camera Pipeline Stats (extended for multi-camera) ───

struct CameraPipelineStats {
    CameraType camera_type{CameraType::NONE};
    bool       camera_open{false};
    uint32_t   frame_count{0};
    float      fps_actual{0};
    uint16_t   width{0};
    uint16_t   height{0};
    // VO stats
    uint16_t   vo_features_detected{0};
    uint16_t   vo_features_tracked{0};
    uint16_t   vo_inlier_count{0};
    float      vo_tracking_quality{0};
    float      vo_confidence{0};          // 0-1 combined confidence
    float      vo_position_uncertainty{0}; // meters
    float      vo_total_distance{0};       // meters total path
    float      vo_dx{0}, vo_dy{0}, vo_dz{0};
    float      vo_vx{0}, vo_vy{0};
    bool       vo_valid{false};
    // Platform info (auto-detected)
    uint8_t    platform{0};                // PlatformType
    char       platform_name[32]{};
    // VO Mode (switchable)
    uint8_t    vo_mode{1};                 // VOModeType
    char       vo_mode_name[32]{};
    // Adaptive parameters
    uint8_t    altitude_zone{0};           // AltitudeZone
    float      adaptive_fast_thresh{30};
    float      adaptive_lk_window{5};
    // Hover yaw correction
    bool       hover_detected{false};
    float      hover_duration{0};
    float      yaw_drift_rate{0};
    float      corrected_yaw{0};
    // CSI sensor info
    uint8_t    csi_sensor_type{0};     // CSISensorType enum
    char       csi_sensor_name[48]{};  // human-readable or raw chip id
    // VO Fallback state
    uint8_t    vo_source{0};           // VOSource enum
    char       vo_fallback_reason[64]{};
    float      vo_fallback_duration{0};
    uint32_t   vo_fallback_switches{0};
    // Frame brightness (for fallback trigger — dark camera detection)
    float      frame_brightness{0};    // average pixel value 0-255
    // Fix #60: VO velocity bias (camera tilt + floor texture calibration)
    float      vx_bias{0};             // m/s, estimated horizontal bias
    float      vy_bias{0};             // m/s, estimated vertical bias
};

class CameraPipeline {
public:
    CameraPipeline();
    
    // Initialize with camera source (auto-detects if SIMULATED)
    bool initialize(CameraType type = CameraType::SIMULATED);
    
    // ── Variant B: CSI priority, USB fallback ──
    // 1. CSI found → PRIMARY(VO), scan USB → SECONDARY(thermal)
    // 2. Only USB  → USB=PRIMARY(VO), no SECONDARY
    // 3. Only CSI  → CSI=PRIMARY(VO), no SECONDARY
    // 4. Nothing   → SIMULATED=PRIMARY(VO)
    bool initialize_multicam();
    
    // Auto-detect: try PI_CSI, then USB, fallback to SIMULATED
    CameraType auto_detect_camera();
    
    // Find first available USB camera device (/dev/video0..9)
    // Skips CSI-owned V4L2 devices
    static const char* find_usb_device();
    
    // Process one frame (capture + VO)
    bool tick(float ground_distance = 1.0f);
    
    // Shutdown
    void shutdown();
    
    // Access results
    const FrameInfo& last_frame_info() const { return current_frame_.info; }
    const VOResult&  last_vo_result() const  { return vo_result_; }
    const FrameBuffer& current_frame() const { return current_frame_; }
    CameraPipelineStats get_stats() const;
    
    bool is_running() const { return running_; }
    
    // Get current VO feature positions
    const VisualOdometry& vo() const { return vo_; }
    
    // Reset VO origin to (0,0,0) — "Set Homepoint"
    void reset_vo() { vo_.reset(); }
    
    // Platform (auto-detected at startup, sets camera resolution)
    void set_platform(PlatformType type);
    PlatformType active_platform() const;
    
    // VO mode (switchable at runtime, only algorithm parameters)
    void set_vo_mode(VOModeType type);
    VOModeType active_vo_mode() const;
    
    // Set altitude for adaptive parameters (call from runtime)
    void set_altitude(float altitude_agl);

    // Set yaw hint for hover correction (call from runtime)
    void set_yaw_hint(float yaw_rad);

    // Feed IMU data to VO for Kalman prediction + consistency check (call before tick())
    void set_imu_hint(float ax, float ay, float gz) { vo_.set_imu_hint(ax, ay, gz); }

    // Accumulate gyro rotation between frames for LK initial-flow hints (call from T1 at 200 Hz)
    void accumulate_gyro(float gx, float gy, float gz, float dt) {
        vo_.accumulate_gyro(gx, gy, gz, dt);
    }

    // Fix #60: VO velocity bias accessors + reset (for RC ch12 disarmed full-calibration reset)
    float vx_bias() const { return vo_.vx_bias(); }
    float vy_bias() const { return vo_.vy_bias(); }
    void  clear_velocity_bias() { vo_.clear_velocity_bias(); }
    
    // ── Multi-camera support ──
    
    // Initialize secondary camera (USB thermal) on specified device
    bool init_secondary(const char* device = "/dev/video2");
    
    // Capture one frame from secondary camera (on-demand)
    bool capture_secondary();
    
    // Get secondary camera frame buffer (last captured)
    const FrameBuffer& secondary_frame() const { return secondary_frame_; }
    
    // Get info about all camera slots
    CameraSlotInfo get_slot_info(CameraSlot slot) const;
    uint8_t camera_count() const;
    
    // Check if secondary camera is available
    bool has_secondary() const { return secondary_camera_ != nullptr && secondary_open_; }
    
    // CSI sensor info (from primary camera)
    CSISensorType csi_sensor_type() const { return csi_camera_.sensor_type(); }
    
    // ── VO Fallback ──
    
    // Get current VO source
    VOSource vo_source() const { return fallback_state_.source; }
    
    // Get fallback state (read-only)
    const VOFallbackState& fallback_state() const { return fallback_state_; }
    
    // Get fallback config
    const VOFallbackConfig& fallback_config() const { return fallback_config_; }
    
    // Thread-safe feature snapshot access (Python reads, T6 writes)
    const FeaturePoint* features_snapshot() const { return features_snapshot_; }
    uint32_t features_snapshot_count() const { 
        return features_snapshot_count_.load(std::memory_order_acquire); 
    }
    
    // External fallback control (called from Python via pybind11)
    void activate_fallback(const char* reason);
    void deactivate_fallback();
    
    // Inject external frame for VO (Python-captured thermal frame)
    // Thread-safe: Python writes, T6 reads via atomic state machine
    // Data must be grayscale, size = width * height bytes
    bool inject_frame(const uint8_t* data, uint16_t width, uint16_t height);
    
    // True if CSI confidence has been below threshold for N frames
    bool is_confidence_low() const { 
        return fallback_state_.low_conf_count >= fallback_config_.frames_to_switch; 
    }

private:
    SimulatedCamera sim_camera_;
    PiCSICamera     csi_camera_;
    USBCamera       usb_camera_;
    CameraSource*   active_camera_{nullptr};
    
    FrameBuffer current_frame_;
    
    VisualOdometry vo_;
    VOResult       vo_result_;
    
    bool running_{false};
    uint32_t frame_count_{0};
    uint64_t start_time_us_{0};
    
    // ── Secondary camera (thermal/USB) ──
    USBCamera       secondary_usb_{"/dev/video2"};
    SimulatedCamera secondary_sim_;         // fallback for testing
    CameraSource*   secondary_camera_{nullptr};
    FrameBuffer     secondary_frame_;
    bool            secondary_open_{false};
    uint32_t        secondary_frame_count_{0};
    uint64_t        secondary_start_us_{0};
    
    // ── Device path for USB cameras ──
    static char usb_device_buf_[16];  // e.g., "/dev/video2"
    
    // ── VO Fallback state ──
    VOFallbackConfig fallback_config_;
    VOFallbackState  fallback_state_;
    CameraSource*    primary_camera_{nullptr};  // saved CSI ref during fallback
    float            runtime_seconds_{0};       // elapsed time for fallback tracking
    
    // ── External frame injection (Python → C++ thread-safe) ──
    // State machine: 0=idle (Python can write), 1=writing, 2=ready (T6 can read)
    std::atomic<int> inject_state_{0};
    uint8_t          inject_buf_[FRAME_SIZE];
    uint16_t         inject_w_{0};
    uint16_t         inject_h_{0};
    std::atomic<bool> external_fallback_{false};
    float frame_brightness_{0};       // average pixel brightness 0-255
    float bright_rolling_avg_{0};     // EMA of brightness (spike baseline)
    int   spike_suppress_frames_{0};  // frames remaining in spike suppression
    
    // ── Thread-safe feature snapshot (T6 writes with release, Python reads with acquire) ──
    FeaturePoint     features_snapshot_[MAX_FEATURES];
    std::atomic<uint32_t> features_snapshot_count_{0};
};

} // namespace jtzero
