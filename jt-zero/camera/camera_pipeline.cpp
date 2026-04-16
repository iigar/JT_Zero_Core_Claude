/**
 * JT-Zero Camera Pipeline Implementation
 * 
 * Includes:
 * - Simulated camera with moving test patterns
 * - FAST-9 corner detector (simplified)
 * - Lucas-Kanade sparse optical flow tracker
 * - Visual Odometry estimator
 * - Pipeline orchestrator
 */

#include "jt_zero/camera.h"
#include "neon_accel.h"
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <algorithm>

namespace jtzero {

// Thread-local xorshift32 PRNG for camera thread
static thread_local uint32_t cam_prng_state_ = 67890;

static inline uint8_t fast_noise(uint8_t range) {
    cam_prng_state_ ^= cam_prng_state_ << 13;
    cam_prng_state_ ^= cam_prng_state_ >> 17;
    cam_prng_state_ ^= cam_prng_state_ << 5;
    return static_cast<uint8_t>(cam_prng_state_ % range);
}

// ═══════════════════════════════════════════════════════════
// Simulated Camera
// ═══════════════════════════════════════════════════════════

bool SimulatedCamera::open() {
    open_ = true;
    frame_counter_ = 0;
    last_capture_us_ = now_us();
    return true;
}

void SimulatedCamera::close() {
    open_ = false;
}

bool SimulatedCamera::capture(FrameBuffer& frame) {
    if (!open_) return false;
    
    uint64_t current_us = now_us();
    float dt = static_cast<float>(current_us - last_capture_us_) / 1'000'000.0f;
    
    generate_pattern(frame.data, FRAME_WIDTH, FRAME_HEIGHT, frame_counter_);
    
    frame.info.timestamp_us = current_us;
    frame.info.frame_id = frame_counter_;
    frame.info.width = FRAME_WIDTH;
    frame.info.height = FRAME_HEIGHT;
    frame.info.channels = 1;
    frame.info.fps_actual = (dt > 0) ? (1.0f / dt) : 0;
    frame.info.valid = true;
    
    frame_counter_++;
    last_capture_us_ = current_us;
    return true;
}

void SimulatedCamera::generate_pattern(uint8_t* data, uint16_t w, uint16_t h, uint32_t frame) {
    // Generate a scene with:
    // 1. Gradient background (simulates ground texture)
    // 2. Moving bright spots (simulates trackable features)
    // 3. Noise (simulates sensor noise)
    
    const float time = static_cast<float>(frame) * 0.08f;
    const float drift_x = 5.0f * std::sin(time * 0.3f);
    const float drift_y = 3.0f * std::cos(time * 0.2f);
    
    for (uint16_t y = 0; y < h; ++y) {
        for (uint16_t x = 0; x < w; ++x) {
            // Base: checkerboard pattern with drift (simulates ground)
            float fx = static_cast<float>(x) + drift_x;
            float fy = static_cast<float>(y) + drift_y;
            
            int checker = (static_cast<int>(fx / 20) + static_cast<int>(fy / 20)) & 1;
            uint8_t base = checker ? 120 : 80;
            
            // Add some texture variation
            float texture = 10.0f * std::sin(fx * 0.1f) * std::cos(fy * 0.1f);
            
            // Bright feature points (corners for FAST to detect)
            float val = base + texture;
            
            // Create several bright spots that move with drift
            for (int i = 0; i < 8; ++i) {
                float cx = 40.0f + i * 35.0f + drift_x * 0.5f;
                float cy = 30.0f + (i % 3) * 70.0f + drift_y * 0.5f;
                float dx = fx - cx;
                float dy = fy - cy;
                float dist_sq = dx * dx + dy * dy;
                constexpr float radius_sq = 16.0f; // 4.0^2
                if (dist_sq < radius_sq) {
                    val += 100.0f * (1.0f - dist_sq / radius_sq);
                }
            }
            
            // Add noise (thread-safe xorshift)
            val += static_cast<float>(fast_noise(10)) - 5.0f;
            
            // Clamp
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            
            data[y * w + x] = static_cast<uint8_t>(val);
        }
    }
}

// ═══════════════════════════════════════════════════════════
// FAST Corner Detector
// ═══════════════════════════════════════════════════════════

bool FASTDetector::is_corner(const uint8_t* frame, uint16_t width,
                              int x, int y, uint8_t threshold) const {
    // Simplified FAST-9: check 16 pixels in Bresenham circle of radius 3
    // A corner exists if N contiguous pixels are all brighter or darker
    
    static const int offsets[16][2] = {
        {0,-3}, {1,-3}, {2,-2}, {3,-1},
        {3,0},  {3,1},  {2,2},  {1,3},
        {0,3},  {-1,3}, {-2,2}, {-3,1},
        {-3,0}, {-3,-1},{-2,-2},{-1,-3}
    };
    
    const uint8_t center = frame[y * width + x];
    // Use int to prevent uint8_t overflow (center + threshold > 255)
    const int center_i = static_cast<int>(center);
    const int hi = std::min(255, center_i + threshold);
    const int lo = std::max(0, center_i - threshold);
    
    // Quick test: at least 3 of positions 0,4,8,12 must be brighter or darker
    int bright_count = 0, dark_count = 0;
    for (int i = 0; i < 16; i += 4) {
        int px = static_cast<int>(frame[(y + offsets[i][1]) * width + (x + offsets[i][0])]);
        if (px > hi) bright_count++;
        if (px < lo) dark_count++;
    }
    
    if (bright_count < 3 && dark_count < 3) return false;
    
    // Full test: 9 contiguous pixels
    int max_bright = 0, max_dark = 0;
    int cur_bright = 0, cur_dark = 0;
    
    // Check twice around to handle wrap-around
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < 16; ++i) {
            int px = static_cast<int>(frame[(y + offsets[i][1]) * width + (x + offsets[i][0])]);
            
            if (px > hi) {
                cur_bright++;
                cur_dark = 0;
            } else if (px < lo) {
                cur_dark++;
                cur_bright = 0;
            } else {
                cur_bright = 0;
                cur_dark = 0;
            }
            
            if (cur_bright > max_bright) max_bright = cur_bright;
            if (cur_dark > max_dark) max_dark = cur_dark;
        }
    }
    
    return max_bright >= 9 || max_dark >= 9;
}

int FASTDetector::detect(const uint8_t* frame, uint16_t width, uint16_t height,
                          FeaturePoint* features, size_t max_features,
                          uint8_t threshold) {
    int count = 0;
    const int border = 4; // Skip border pixels
    
    for (int y = border; y < height - border && static_cast<size_t>(count) < max_features; y += 3) {
        for (int x = border; x < width - border && static_cast<size_t>(count) < max_features; x += 3) {
            if (is_corner(frame, width, x, y, threshold)) {
                features[count].x = static_cast<float>(x);
                features[count].y = static_cast<float>(y);
                // Corner response = intensity contrast
                uint8_t center = frame[y * width + x];
                features[count].response = static_cast<float>(
                    std::abs(static_cast<int>(frame[(y-1)*width+x]) - center) +
                    std::abs(static_cast<int>(frame[(y+1)*width+x]) - center) +
                    std::abs(static_cast<int>(frame[y*width+x-1]) - center) +
                    std::abs(static_cast<int>(frame[y*width+x+1]) - center)
                );
                features[count].tracked = false;
                count++;
            }
        }
    }
    
    return count;
}

// ═══════════════════════════════════════════════════════════
// Lucas-Kanade Optical Flow Tracker
// ═══════════════════════════════════════════════════════════

void LKTracker::compute_gradient(const uint8_t* frame, uint16_t width,
                                  int x, int y, float& gx, float& gy) const {
    // Sobel 3x3 gradient — smooths noise while amplifying edges (~4x stronger than central diff)
    const uint8_t* r0 = frame + (y - 1) * width + x;  // row above
    const uint8_t* r1 = frame + y * width + x;          // center row
    const uint8_t* r2 = frame + (y + 1) * width + x;   // row below
    gx = static_cast<float>(-r0[-1] + r0[1] - 2*r1[-1] + 2*r1[1] - r2[-1] + r2[1]);
    gy = static_cast<float>(-r0[-1] - 2*r0[0] - r0[1] + r2[-1] + 2*r2[0] + r2[1]);
}

int LKTracker::track(const uint8_t* prev_frame, const uint8_t* curr_frame,
                      uint16_t width, uint16_t height,
                      FeaturePoint* features, size_t feature_count,
                      int window_size, int iterations,
                      const float* hint_dx, const float* hint_dy) {
    const int half_win = window_size / 2;
    int tracked = 0;

    // Bilinear interpolation helper (sub-pixel accuracy — critical for convergence)
    auto bilerp = [width, height](const uint8_t* img, float fx, float fy) -> float {
        int ix = static_cast<int>(fx);
        int iy = static_cast<int>(fy);
        if (ix < 0 || ix >= width - 1 || iy < 0 || iy >= height - 1) return -1.0f;
        float dx = fx - ix;
        float dy = fy - iy;
        float tl = img[iy * width + ix];
        float tr = img[iy * width + ix + 1];
        float bl = img[(iy + 1) * width + ix];
        float br = img[(iy + 1) * width + ix + 1];
        return (1 - dx) * (1 - dy) * tl + dx * (1 - dy) * tr
             + (1 - dx) * dy * bl + dx * dy * br;
    };

    for (size_t f = 0; f < feature_count; ++f) {
        float px = features[f].x;
        float py = features[f].y;

        // Skip if too close to border
        if (px < half_win + 2 || px >= width - half_win - 2 ||
            py < half_win + 2 || py >= height - half_win - 2) {
            features[f].tracked = false;
            continue;
        }

        // Fix 6: seed flow from IMU pre-integration hint (reduces iterations needed,
        // improves tracking during fast rotations between frames)
        float flow_x = (hint_dx != nullptr) ? hint_dx[f] : 0.0f;
        float flow_y = (hint_dy != nullptr) ? hint_dy[f] : 0.0f;
        bool converged = false;
        
        for (int iter = 0; iter < iterations; ++iter) {
            float sum_ixx = 0, sum_iyy = 0, sum_ixy = 0;
            float sum_itx = 0, sum_ity = 0;
            
            for (int wy = -half_win; wy <= half_win; ++wy) {
                for (int wx = -half_win; wx <= half_win; ++wx) {
                    int ox = static_cast<int>(px) + wx;
                    int oy = static_cast<int>(py) + wy;
                    
                    // Bounds check for prev_frame (integer access)
                    if (ox < 1 || ox >= width - 1 || oy < 1 || oy >= height - 1) continue;
                    
                    float gx, gy;
                    compute_gradient(prev_frame, width, ox, oy, gx, gy);
                    
                    // Bilinear interpolation for curr_frame (sub-pixel accuracy)
                    float cx = px + flow_x + wx;
                    float cy = py + flow_y + wy;
                    float curr_val = bilerp(curr_frame, cx, cy);
                    if (curr_val < 0) continue;  // out of bounds
                    
                    float it = curr_val - static_cast<float>(prev_frame[oy * width + ox]);
                    
                    sum_ixx += gx * gx;
                    sum_iyy += gy * gy;
                    sum_ixy += gx * gy;
                    sum_itx += gx * it;
                    sum_ity += gy * it;
                }
            }
            
            // Solve 2x2 system: [Ixx Ixy; Ixy Iyy] * [vx;vy] = -[Itx;Ity]
            float det = sum_ixx * sum_iyy - sum_ixy * sum_ixy;
            if (std::abs(det) < 1e-6f) break;
            
            float dvx = -(sum_iyy * sum_itx - sum_ixy * sum_ity) / det;
            float dvy = -(sum_ixx * sum_ity - sum_ixy * sum_itx) / det;
            
            flow_x += dvx;
            flow_y += dvy;
            
            if (std::abs(dvx) < 0.05f && std::abs(dvy) < 0.05f) {
                converged = true;
                break;
            }
        }
        
        // Validate flow
        float new_x = px + flow_x;
        float new_y = py + flow_y;
        
        if (converged && 
            new_x >= half_win && new_x < width - half_win &&
            new_y >= half_win && new_y < height - half_win &&
            std::abs(flow_x) < 50.0f && std::abs(flow_y) < 50.0f) {
            features[f].x = new_x;
            features[f].y = new_y;
            features[f].tracked = true;
            tracked++;
        } else {
            features[f].tracked = false;
        }
    }
    
    return tracked;
}

// ═══════════════════════════════════════════════════════════
// Visual Odometry
// ═══════════════════════════════════════════════════════════

VisualOdometry::VisualOdometry() {
    std::memset(prev_frame_, 0, FRAME_SIZE);
    features_.fill({});
    prev_features_.fill({});
    // Default platform: Pi Zero 2W, default mode: Balanced
    platform_ = PLATFORMS[0];
    vo_mode_ = VO_MODES[1]; // Balanced
}

void VisualOdometry::set_imu_hint(float ax, float ay, float gyro_z) {
    imu_ax_ = ax;
    imu_ay_ = ay;
    imu_gz_ = gyro_z;
    imu_hint_valid_ = true;
}

void VisualOdometry::accumulate_gyro(float gx, float gy, float gz, float dt) {
    // Fix 6: called from T1 at 200 Hz; accumulates rotation between T6 frames
    std::lock_guard<std::mutex> lk(preint_mtx_);
    preint_.dgx += gx * dt;
    preint_.dgy += gy * dt;
    preint_.dgz += gz * dt;
    preint_.valid = true;
}

void VisualOdometry::set_altitude(float altitude_agl) {
    current_altitude_ = altitude_agl;
    update_adaptive_params();
}

void VisualOdometry::set_yaw_hint(float yaw_rad) {
    yaw_hint_ = yaw_rad;
    yaw_hint_valid_ = true;
}

void VisualOdometry::set_platform(const PlatformConfig& platform) {
    platform_ = platform;
    // Platform change = resolution change, requires full reset
    reset();
}

void VisualOdometry::set_vo_mode(const VOMode& mode) {
    // Mode change = only algorithm parameters, NO reset, NO lost tracking
    vo_mode_ = mode;
    update_adaptive_params();
}

// ── Adaptive parameter interpolation ──
// Base values come from active VO mode, then scaled by altitude zone
void VisualOdometry::update_adaptive_params() {
    float alt = current_altitude_;
    uint8_t base_fast = vo_mode_.fast_threshold;
    int base_lk = vo_mode_.lk_window_size;
    int base_iter = vo_mode_.lk_iterations;
    
    if (alt < ALT_ZONE_LOW_MAX) {
        adaptive_.zone = AltitudeZone::LOW;
        adaptive_.fast_threshold = base_fast;
        adaptive_.lk_window_size = base_lk;
        adaptive_.lk_iterations = base_iter;
        adaptive_.min_inliers = 5;
        adaptive_.kalman_q = 0.5f;
        adaptive_.kalman_r_base = 0.3f;
        adaptive_.redetect_ratio = 0.15f;
    } else if (alt < ALT_ZONE_MEDIUM_MAX) {
        adaptive_.zone = AltitudeZone::MEDIUM;
        float t = (alt - ALT_ZONE_LOW_MAX) / (ALT_ZONE_MEDIUM_MAX - ALT_ZONE_LOW_MAX);
        adaptive_.fast_threshold = static_cast<uint8_t>(
            std::max(12, static_cast<int>(base_fast) - static_cast<int>(t * 5)));
        adaptive_.lk_window_size = base_lk + static_cast<int>(t * 2);
        adaptive_.lk_iterations = base_iter + static_cast<int>(t * 1);
        adaptive_.min_inliers = 5 + static_cast<int>(t * 3);
        adaptive_.kalman_q = 0.5f - t * 0.15f;
        adaptive_.kalman_r_base = 0.3f + t * 0.1f;
        adaptive_.redetect_ratio = 0.15f + t * 0.05f;
    } else if (alt < ALT_ZONE_HIGH_MAX) {
        adaptive_.zone = AltitudeZone::HIGH;
        float t = (alt - ALT_ZONE_MEDIUM_MAX) / (ALT_ZONE_HIGH_MAX - ALT_ZONE_MEDIUM_MAX);
        adaptive_.fast_threshold = static_cast<uint8_t>(
            std::max(12, static_cast<int>(base_fast) - 5 - static_cast<int>(t * 5)));
        adaptive_.lk_window_size = base_lk + 2 + static_cast<int>(t * 2);
        adaptive_.lk_iterations = base_iter + 1 + static_cast<int>(t * 1);
        adaptive_.min_inliers = 8 + static_cast<int>(t * 4);
        adaptive_.kalman_q = 0.35f - t * 0.1f;
        adaptive_.kalman_r_base = 0.4f + t * 0.15f;
        adaptive_.redetect_ratio = 0.20f + t * 0.05f;
    } else {
        adaptive_.zone = AltitudeZone::CRUISE;
        adaptive_.fast_threshold = static_cast<uint8_t>(
            std::max(12, static_cast<int>(base_fast) - 10));
        adaptive_.lk_window_size = base_lk + 4;
        adaptive_.lk_iterations = base_iter + 2;
        adaptive_.min_inliers = 12;
        adaptive_.kalman_q = 0.25f;
        adaptive_.kalman_r_base = 0.55f;
        adaptive_.redetect_ratio = 0.25f;
    }
}

// ── Hover yaw correction ──
void VisualOdometry::update_hover_state(float median_dx, float median_dy, float dt, float gyro_z) {
    // Compute average micro-motion magnitude (in pixels)
    float motion_mag = std::sqrt(median_dx * median_dx + median_dy * median_dy);

    // EMA of motion magnitude
    hover_.micro_motion_avg = 0.1f * motion_mag + 0.9f * hover_.micro_motion_avg;

    if (hover_.micro_motion_avg < HoverState::HOVER_MOTION_THRESH) {
        hover_.stable_frame_count++;
        if (hover_.stable_frame_count >= HoverState::HOVER_MIN_FRAMES) {
            hover_.is_hovering = true;
            hover_.hover_duration_sec += dt;

            // Fix 3: gyro bias estimation during confirmed hover.
            // In true hover the drone does not rotate → any persistent gyro_z reading = bias.
            // Sanity gate: only update if gyro_z is small (not a real intentional rotation).
            if (std::fabs(gyro_z) < 0.3f) {  // ~17°/s gate
                hover_.gyro_z_bias += (gyro_z - hover_.gyro_z_bias) * HoverState::BIAS_ALPHA;
            }

            // Estimate yaw drift from micro-motion pattern.
            // During hover, systematic x-displacement indicates residual yaw rotation.
            // drift_rate ≈ median_dx / focal_length (rad/frame), then convert to rad/s.
            // Subtract known gyro bias so only true optical drift accumulates.
            if (platform_.focal_length_px > 0 && dt > 0) {
                float frame_yaw_drift = median_dx / platform_.focal_length_px;
                float rate = frame_yaw_drift / dt;
                // Remove bias component from rate (gyro_z ≈ bias + true_rotation)
                float corrected_rate = rate - hover_.gyro_z_bias;

                hover_.yaw_drift_rate = HoverState::DRIFT_ALPHA * corrected_rate +
                    (1.0f - HoverState::DRIFT_ALPHA) * hover_.yaw_drift_rate;

                hover_.accumulated_yaw_drift += hover_.yaw_drift_rate * dt;
            }

            // Update corrected yaw (subtract estimated drift)
            if (yaw_hint_valid_) {
                hover_.corrected_yaw = yaw_hint_ - hover_.accumulated_yaw_drift;
            }
        }
    } else {
        // Motion detected — exit hover or reset counter
        if (hover_.is_hovering) {
            // Keep accumulated correction and bias estimate, stop accumulating
            hover_.is_hovering = false;
        }
        hover_.stable_frame_count = 0;
        hover_.hover_duration_sec = 0;
        hover_.yaw_drift_rate = 0;
    }
}

float VisualOdometry::compute_median(float* arr, int n) {
    if (n <= 0) return 0;
    // Simple insertion sort for small arrays (n < 200)
    for (int i = 1; i < n; i++) {
        float key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
    if (n % 2 == 0) return (arr[n/2 - 1] + arr[n/2]) * 0.5f;
    return arr[n/2];
}

float VisualOdometry::compute_mad(float* arr, int n, float median) {
    if (n <= 0) return 0;
    // Compute absolute deviations, reuse array
    for (int i = 0; i < n; i++) {
        arr[i] = std::fabs(arr[i] - median);
    }
    return compute_median(arr, n) * 1.4826f; // MAD to std dev estimator
}

VOResult VisualOdometry::process(const FrameBuffer& frame, float ground_distance) {
    VOResult result;
    result.timestamp_us = frame.info.timestamp_us;
    result.platform = static_cast<uint8_t>(platform_.type);
    result.vo_mode = static_cast<uint8_t>(vo_mode_.type);
    result.altitude_zone = static_cast<uint8_t>(adaptive_.zone);
    result.adaptive_fast_thresh = static_cast<float>(adaptive_.fast_threshold);
    result.adaptive_lk_window = static_cast<float>(adaptive_.lk_window_size);
    
    // Use adaptive or mode FAST threshold
    uint8_t fast_thresh = adaptive_.fast_threshold;
    int lk_win = adaptive_.lk_window_size;
    int lk_iter = adaptive_.lk_iterations;
    size_t max_feat = vo_mode_.max_features;
    
    // ── Shi-Tomasi grid corner detector for thermal / low-contrast images ──
    // Unlike simple gradient threshold, this computes the structure tensor in a
    // 5x5 window at each grid point and keeps only features where the minimum
    // eigenvalue is large enough (= actual corners, not edges). LK can only
    // track corners, so this is critical for thermal cameras.
    auto shi_tomasi_detect = [](const uint8_t* img, uint16_t w, uint16_t h,
                                FeaturePoint* features, size_t max_features) -> size_t {
        const int spacing = 8;
        const int border = 6;
        const float min_eigen = 100.0f;
        size_t count = 0;
        for (int y = border; y < h - border && count < max_features; y += spacing) {
            for (int x = border; x < w - border && count < max_features; x += spacing) {
                float sxx, syy, sxy;
                neon::structure_tensor_5x5(img, x, y, w, sxx, syy, sxy);
                float trace = sxx + syy;
                float det = sxx * syy - sxy * sxy;
                float disc = trace * trace - 4.0f * det;
                if (disc < 0) continue;
                float min_eig = (trace - std::sqrt(disc)) * 0.5f;
                if (min_eig >= min_eigen) {
                    features[count].x = static_cast<float>(x);
                    features[count].y = static_cast<float>(y);
                    features[count].tracked = false;
                    features[count].response = min_eig;
                    ++count;
                }
            }
        }
        return count;
    };
    
    if (!has_prev_frame_) {
        active_count_ = static_cast<size_t>(
            detector_.detect(frame.data, frame.info.width, frame.info.height,
                           features_.data(), max_feat, fast_thresh));
        // Fallback: Shi-Tomasi grid corner detector for thermal images
        if (active_count_ < 15) {
            active_count_ = shi_tomasi_detect(frame.data, frame.info.width, frame.info.height,
                                              features_.data(), max_feat);
        }
        
        size_t frame_bytes = static_cast<size_t>(frame.info.width) * frame.info.height;
        if (frame_bytes > FRAME_SIZE) frame_bytes = FRAME_SIZE;
        std::memcpy(prev_frame_, frame.data, frame_bytes);
        prev_timestamp_us_ = frame.info.timestamp_us;
        has_prev_frame_ = true;
        
        result.features_detected = static_cast<uint16_t>(active_count_);
        result.valid = false;
        result.confidence = 0;
        return result;
    }
    
    // Save previous feature positions
    for (size_t i = 0; i < active_count_; ++i) {
        prev_features_[i] = features_[i];
    }

    // Fix 6: build per-feature initial flow hints from IMU pre-integrated rotation.
    // Accumulated between frames at 200 Hz by T1; read and reset atomically here.
    // For a forward-looking camera: yaw (dgz) → horizontal shift, pitch (dgy) → vertical.
    // These hints let LK start closer to the true displacement, reducing convergence
    // iterations and preventing track loss during abrupt rotations.
    float hint_dx_buf[MAX_FEATURES];
    float hint_dy_buf[MAX_FEATURES];
    const float* lk_hint_dx = nullptr;
    const float* lk_hint_dy = nullptr;

    {
        std::lock_guard<std::mutex> lk(preint_mtx_);
        if (preint_.valid && platform_.focal_length_px > 0) {
            float shift_x =  platform_.focal_length_px * preint_.dgz;  // yaw → lateral
            float shift_y = -platform_.focal_length_px * preint_.dgy;  // pitch → vertical
            // Only apply if the predicted shift is non-trivial (>0.3 px) to avoid
            // polluting LK with floating-point noise when the drone is stationary
            if (std::fabs(shift_x) + std::fabs(shift_y) > 0.3f) {
                for (size_t i = 0; i < active_count_; ++i) {
                    hint_dx_buf[i] = shift_x;
                    hint_dy_buf[i] = shift_y;
                }
                lk_hint_dx = hint_dx_buf;
                lk_hint_dy = hint_dy_buf;
            }
        }
        preint_ = {};  // reset for next frame regardless
    }

    // Track existing features using adaptive LK parameters + optional IMU rotation hint
    int tracked_count = tracker_.track(
        prev_frame_, frame.data,
        frame.info.width, frame.info.height,
        features_.data(), active_count_,
        lk_win, lk_iter,
        lk_hint_dx, lk_hint_dy);
    
    // ════════════════════════════════════════════════════
    // Phase 1: Median filter + MAD outlier rejection
    // ════════════════════════════════════════════════════
    
    float dx_arr[MAX_FEATURES];
    float dy_arr[MAX_FEATURES];
    float dx_copy[MAX_FEATURES];
    float dy_copy[MAX_FEATURES];
    int valid_flow = 0;
    
    for (size_t i = 0; i < active_count_; ++i) {
        if (features_[i].tracked) {
            dx_arr[valid_flow] = features_[i].x - prev_features_[i].x;
            dy_arr[valid_flow] = features_[i].y - prev_features_[i].y;
            valid_flow++;
        }
    }
    
    float median_dx = 0, median_dy = 0;
    int inlier_count = 0;
    float inlier_sum_x = 0, inlier_sum_y = 0;
    
    if (valid_flow >= adaptive_.min_inliers) {
        std::memcpy(dx_copy, dx_arr, valid_flow * sizeof(float));
        std::memcpy(dy_copy, dy_arr, valid_flow * sizeof(float));
        median_dx = compute_median(dx_copy, valid_flow);
        median_dy = compute_median(dy_copy, valid_flow);
        
        std::memcpy(dx_copy, dx_arr, valid_flow * sizeof(float));
        std::memcpy(dy_copy, dy_arr, valid_flow * sizeof(float));
        float mad_x = compute_mad(dx_copy, valid_flow, median_dx);
        float mad_y = compute_mad(dy_copy, valid_flow, median_dy);
        
        float thresh_x = std::max(2.5f * mad_x, 1.0f);
        float thresh_y = std::max(2.5f * mad_y, 1.0f);
        
        for (int i = 0; i < valid_flow; ++i) {
            if (std::fabs(dx_arr[i] - median_dx) <= thresh_x &&
                std::fabs(dy_arr[i] - median_dy) <= thresh_y) {
                inlier_sum_x += dx_arr[i];
                inlier_sum_y += dy_arr[i];
                inlier_count++;
            }
        }
    }
    
    float filtered_dx_px = (inlier_count > 3) ? inlier_sum_x / static_cast<float>(inlier_count) : median_dx;
    float filtered_dy_px = (inlier_count > 3) ? inlier_sum_y / static_cast<float>(inlier_count) : median_dy;
    
    // ════════════════════════════════════════════════════
    // Compute dt and convert to meters
    // ════════════════════════════════════════════════════
    
    float dt = static_cast<float>(frame.info.timestamp_us - prev_timestamp_us_) / 1'000'000.0f;
    if (dt <= 0 || dt > 1.0f) dt = 0.066f;
    
    // Use platform-specific focal length
    float focal = platform_.focal_length_px;
    float pixel_to_meter = (ground_distance > 0.1f) ? ground_distance / focal : 0;
    
    float raw_dx = filtered_dx_px * pixel_to_meter;
    float raw_dy = filtered_dy_px * pixel_to_meter;
    float raw_vx = (dt > 0) ? raw_dx / dt : 0;
    float raw_vy = (dt > 0) ? raw_dy / dt : 0;
    
    // ════════════════════════════════════════════════════
    // Phase 2: Kalman filter (adaptive noise + IMU prediction)
    // ════════════════════════════════════════════════════

    float Q_accel = adaptive_.kalman_q;
    float Q = Q_accel * Q_accel * dt * dt;

    float inlier_ratio = (valid_flow > 0) ? static_cast<float>(inlier_count) / static_cast<float>(valid_flow) : 0;
    float R = adaptive_.kalman_r_base / std::max(0.1f, inlier_ratio);

    // Fix 1: snapshot velocity BEFORE predict so Phase 3 can compare
    // ΔV_VO = raw_vx - kf_vx_prev_ (VO-measured velocity change this frame)
    // ΔV_IMU = imu_ax_ * dt         (IMU-predicted velocity change this frame)
    // Previously used (raw_vx - kf_vx_) which is the post-update residual, not ΔV.
    kf_vx_prev_ = kf_vx_;
    kf_vy_prev_ = kf_vy_;

    // Fix 4: IMU prediction step — use accelerometer to advance state estimate.
    // Replaces the pure variance-growth predict with a physics-based predict:
    //   v_predicted = v_prev + a * dt
    // This reduces velocity error during fast maneuvers where VO measurement
    // lags behind actual motion.
    if (imu_hint_valid_) {
        kf_vx_ += imu_ax_ * dt;
        kf_vy_ += imu_ay_ * dt;
    }
    // Process noise grows variance each frame regardless of IMU availability
    kf_vx_var_ += Q;
    kf_vy_var_ += Q;

    // Update (VO measurement corrects IMU-predicted state)
    float Kx = kf_vx_var_ / (kf_vx_var_ + R);
    float Ky = kf_vy_var_ / (kf_vy_var_ + R);
    kf_vx_ += Kx * (raw_vx - kf_vx_);
    kf_vy_ += Ky * (raw_vy - kf_vy_);
    kf_vx_var_ *= (1.0f - Kx);
    kf_vy_var_ *= (1.0f - Ky);

    result.vx = kf_vx_;
    result.vy = kf_vy_;
    result.dx = kf_vx_ * dt;
    result.dy = kf_vy_ * dt;
    result.dz = 0;
    result.vz = 0;

    // ════════════════════════════════════════════════════
    // Phase 3: IMU-aided validation (fixed)
    // ════════════════════════════════════════════════════

    float imu_consistency = 1.0f;

    if (imu_hint_valid_ && dt > 0) {
        // Fix 1: correct comparison — both quantities must be velocity DELTAS.
        // ΔV_IMU = a * dt  (accelerometer-predicted velocity change this frame)
        // ΔV_VO  = raw_vx - kf_vx_prev_  (VO-observed velocity change, using
        //          pre-predict state so we compare matching time intervals)
        float expected_dvx = imu_ax_ * dt;
        float expected_dvy = imu_ay_ * dt;
        float actual_dvx   = raw_vx - kf_vx_prev_;
        float actual_dvy   = raw_vy - kf_vy_prev_;
        float discrepancy = std::sqrt(
            (actual_dvx - expected_dvx) * (actual_dvx - expected_dvx) +
            (actual_dvy - expected_dvy) * (actual_dvy - expected_dvy));
        // Scale: 1 m/s² discrepancy over 1 frame (66ms) ≈ 0.066 m/s delta → ~0.33 penalty
        imu_consistency = std::max(0.1f, 1.0f - discrepancy * 5.0f);
        imu_hint_valid_ = false;
    }
    
    // ════════════════════════════════════════════════════
    // Phase 4: Confidence metric
    // ════════════════════════════════════════════════════
    
    float track_quality = (active_count_ > 0) ? 
        static_cast<float>(tracked_count) / static_cast<float>(active_count_) : 0;
    
    float feature_quality = std::min(1.0f, static_cast<float>(inlier_count) / 30.0f);
    
    float raw_confidence = track_quality * inlier_ratio * imu_consistency * feature_quality;
    
    constexpr float alpha = 0.3f;
    running_confidence_ = alpha * raw_confidence + (1.0f - alpha) * running_confidence_;
    
    bool position_update = running_confidence_ > 0.40f && inlier_count >= adaptive_.min_inliers;
    
    if (position_update) {
        pose_x_ += result.dx;
        pose_y_ += result.dy;
        total_distance_ += std::sqrt(result.dx * result.dx + result.dy * result.dy);
        // Fix 5: accumulate position variance from Kalman velocity variance.
        // pose_var += kf_v_var * dt²  (integrated position uncertainty each step)
        // Decay when tracking is good so uncertainty doesn't grow unboundedly.
        pose_var_x_ += kf_vx_var_ * dt * dt;
        pose_var_y_ += kf_vy_var_ * dt * dt;
        if (running_confidence_ > 0.7f) {
            // High-confidence tracking: slowly shrink accumulated uncertainty
            pose_var_x_ *= 0.995f;
            pose_var_y_ *= 0.995f;
        }
    } else {
        result.dx = 0;
        result.dy = 0;
        result.vx = 0;
        result.vy = 0;
        // No position update → uncertainty grows faster (we're dead-reckoning)
        pose_var_x_ += kf_vx_var_ * dt * dt * 4.0f;
        pose_var_y_ += kf_vy_var_ * dt * dt * 4.0f;
    }

    // Fix 5: position uncertainty from KF covariance, not ad-hoc distance * drift_rate.
    // sqrt(pose_var_x + pose_var_y) gives 1-sigma radial position uncertainty in meters.
    result.position_uncertainty = std::sqrt(pose_var_x_ + pose_var_y_);

    // ════════════════════════════════════════════════════
    // Phase 5: Hover yaw correction
    // ════════════════════════════════════════════════════

    // Fix 3: pass gyro_z so hover state can estimate IMU bias during stationary hover
    update_hover_state(median_dx, median_dy, dt, imu_gz_);
    
    result.hover_detected = hover_.is_hovering;
    result.hover_duration = hover_.hover_duration_sec;
    result.yaw_drift_rate = hover_.yaw_drift_rate;
    result.corrected_yaw = hover_.corrected_yaw;
    
    // If hovering, apply additional position freeze to prevent drift accumulation
    if (hover_.is_hovering && hover_.hover_duration_sec > 2.0f) {
        // During confirmed hover, apply stronger position damping
        result.dx *= 0.1f;
        result.dy *= 0.1f;
    }
    
    // ════════════════════════════════════════════════════
    // Fill result
    // ════════════════════════════════════════════════════
    
    result.features_tracked = static_cast<uint16_t>(tracked_count);
    result.features_detected = static_cast<uint16_t>(active_count_);
    result.inlier_count = static_cast<uint16_t>(inlier_count);
    result.tracking_quality = track_quality;
    result.confidence = running_confidence_;
    result.valid = position_update;
    
    // Re-detect features using adaptive threshold if too few tracked
    size_t redetect_thresh = static_cast<size_t>(
        adaptive_.redetect_ratio * static_cast<float>(max_feat));
    if (static_cast<size_t>(tracked_count) < redetect_thresh || active_count_ < redetect_thresh * 2) {
        active_count_ = static_cast<size_t>(
            detector_.detect(frame.data, frame.info.width, frame.info.height,
                           features_.data(), max_feat, fast_thresh));
        // Fallback: Shi-Tomasi grid corner detector for thermal images
        if (active_count_ < 15) {
            active_count_ = shi_tomasi_detect(frame.data, frame.info.width, frame.info.height,
                                              features_.data(), max_feat);
        }
        result.features_detected = static_cast<uint16_t>(active_count_);
    }
    
    // Update state
    size_t frame_bytes = static_cast<size_t>(frame.info.width) * frame.info.height;
    if (frame_bytes > FRAME_SIZE) frame_bytes = FRAME_SIZE;
    std::memcpy(prev_frame_, frame.data, frame_bytes);
    prev_timestamp_us_ = frame.info.timestamp_us;
    
    return result;
}

void VisualOdometry::reset() {
    has_prev_frame_ = false;
    active_count_ = 0;
    prev_timestamp_us_ = 0;
    pose_x_ = 0;
    pose_y_ = 0;
    pose_z_ = 0;
    total_distance_ = 0;
    kf_vx_ = 0;
    kf_vy_ = 0;
    kf_vx_var_ = 1.0f;
    kf_vy_var_ = 1.0f;
    running_confidence_ = 0.5f;
    imu_hint_valid_ = false;
    yaw_hint_valid_ = false;
    current_altitude_ = 0;
    // Reset hover state
    hover_ = HoverState{};
    // Reset adaptive params to profile defaults
    update_adaptive_params();
}

// ═══════════════════════════════════════════════════════════
// Camera Pipeline
// ═══════════════════════════════════════════════════════════

CameraPipeline::CameraPipeline() = default;

bool CameraPipeline::initialize(CameraType type) {
    CameraType actual = type;
    if (type == CameraType::SIMULATED || type == CameraType::NONE) {
        actual = auto_detect_camera();
    }
    
    switch (actual) {
        case CameraType::PI_CSI:
            if (csi_camera_.open()) {
                active_camera_ = &csi_camera_;
                break;
            }
            std::printf("[CameraPipeline] CSI open failed, trying USB...\n");
            [[fallthrough]];
        case CameraType::USB:
            if (usb_camera_.open()) {
                active_camera_ = &usb_camera_;
                break;
            }
            std::printf("[CameraPipeline] USB open failed, using simulation\n");
            [[fallthrough]];
        case CameraType::SIMULATED:
        default:
            if (!sim_camera_.open()) return false;
            active_camera_ = &sim_camera_;
            break;
    }
    
    running_ = true;
    frame_count_ = 0;
    start_time_us_ = now_us();
    runtime_seconds_ = 0;
    vo_.reset();
    
    // Save primary camera reference for VO fallback recovery
    primary_camera_ = active_camera_;
    
    // Auto-detect platform based on Pi model (sets resolution + focal length)
#ifdef __aarch64__
    FILE* f = std::fopen("/proc/device-tree/model", "r");
    if (f) {
        char model[128]{};
        std::fread(model, 1, 127, f);
        std::fclose(f);
        if (std::strstr(model, "Pi 5")) {
            set_platform(PlatformType::PI_5);
        } else if (std::strstr(model, "Pi 4")) {
            set_platform(PlatformType::PI_4);
        } else {
            set_platform(PlatformType::PI_ZERO_2W);
        }
        std::printf("[CameraPipeline] Detected: %s -> Platform: %s (%ux%u)\n", 
                    model, vo_.platform().name,
                    vo_.platform().frame_width, vo_.platform().frame_height);
    } else {
        set_platform(PlatformType::PI_ZERO_2W);
    }
#else
    set_platform(PlatformType::PI_ZERO_2W);
    std::printf("[CameraPipeline] Non-ARM platform, using Pi Zero 2W\n");
#endif
    
    // Default VO mode: Balanced
    set_vo_mode(VOModeType::BALANCED);
    
    return true;
}

void CameraPipeline::set_platform(PlatformType type) {
    size_t idx = static_cast<size_t>(type);
    if (idx < NUM_PLATFORMS) {
        vo_.set_platform(PLATFORMS[idx]);
        std::printf("[CameraPipeline] Platform: %s (%ux%u, focal=%.0fpx)\n",
                    PLATFORMS[idx].name,
                    PLATFORMS[idx].frame_width,
                    PLATFORMS[idx].frame_height,
                    PLATFORMS[idx].focal_length_px);
    }
}

PlatformType CameraPipeline::active_platform() const {
    return vo_.platform().type;
}

void CameraPipeline::set_vo_mode(VOModeType type) {
    size_t idx = static_cast<size_t>(type);
    if (idx < NUM_VO_MODES) {
        vo_.set_vo_mode(VO_MODES[idx]);
        std::printf("[CameraPipeline] VO Mode: %s (FAST=%u, LK=%dpx, features=%zu)\n",
                    VO_MODES[idx].name,
                    VO_MODES[idx].fast_threshold,
                    VO_MODES[idx].lk_window_size,
                    VO_MODES[idx].max_features);
    }
}

VOModeType CameraPipeline::active_vo_mode() const {
    return vo_.vo_mode().type;
}

void CameraPipeline::set_altitude(float altitude_agl) {
    vo_.set_altitude(altitude_agl);
}

void CameraPipeline::set_yaw_hint(float yaw_rad) {
    vo_.set_yaw_hint(yaw_rad);
}

bool CameraPipeline::tick(float ground_distance) {
    if (!running_ || !active_camera_) return false;
    
    // Update runtime clock
    uint64_t elapsed_us = now_us() - start_time_us_;
    runtime_seconds_ = static_cast<float>(elapsed_us) / 1'000'000.0f;
    
    // ═══════════════════════════════════════════════════════
    // VO Fallback: External injection mode (Python → C++)
    // ═══════════════════════════════════════════════════════
    
    if (external_fallback_.load(std::memory_order_acquire)) {
        // Fallback active: process injected thermal frame if available
        fallback_state_.fallback_duration = runtime_seconds_ - fallback_state_.fallback_start_time;
        
        if (inject_state_.load(std::memory_order_acquire) == 2) {
            // Copy injected frame into current_frame_
            size_t sz = static_cast<size_t>(inject_w_) * inject_h_;
            if (sz > 0 && sz <= FRAME_SIZE) {
                std::memcpy(current_frame_.data, inject_buf_, sz);
                current_frame_.info.width = inject_w_;
                current_frame_.info.height = inject_h_;
                current_frame_.info.channels = 1;
                current_frame_.info.timestamp_us = now_us();
                current_frame_.info.frame_id = frame_count_;
                current_frame_.info.valid = true;
            }
            inject_state_.store(0, std::memory_order_release);  // free for Python to write
            
            // Run VO on injected thermal frame
            vo_result_ = vo_.process(current_frame_, ground_distance);
            frame_count_++;
            
            // Copy features to thread-safe snapshot (release barrier ensures
            // features_snapshot_ data is visible when count is read with acquire)
            size_t fc = vo_.feature_count();
            if (fc > MAX_FEATURES) fc = MAX_FEATURES;
            std::memcpy(features_snapshot_, vo_.features().data(), fc * sizeof(FeaturePoint));
            features_snapshot_count_.store(static_cast<uint32_t>(fc), std::memory_order_release);
        }
        
        // Periodic CSI probe for recovery (use primary camera)
        if (primary_camera_ &&
            (runtime_seconds_ - fallback_state_.last_csi_probe_time) >= fallback_config_.csi_probe_interval_s) {
            
            fallback_state_.last_csi_probe_time = runtime_seconds_;
            
            FrameBuffer probe_frame;
            if (primary_camera_->capture(probe_frame)) {
                // Brightness-only probe using NEON-accelerated mean
                int pixel_count = probe_frame.info.width * probe_frame.info.height;
                float avg_brightness = neon::frame_brightness(probe_frame.data, static_cast<size_t>(pixel_count));
                
                // Normalize: brightness 150 = quality 1.0, brightness < 20 = quality 0
                float probe_quality = 0;
                if (avg_brightness >= 20.0f) {
                    probe_quality = avg_brightness / 150.0f;
                    if (probe_quality > 1.0f) probe_quality = 1.0f;
                }
                fallback_state_.last_csi_probe_conf = probe_quality;
                
                std::printf("[VO Fallback] CSI probe: brightness=%.1f quality=%.2f (need %.2f)\n",
                           avg_brightness, probe_quality, fallback_config_.conf_recover_thresh);
            }
        }
        
        return true;
    }
    
    // ═══════════════════════════════════════════════════════
    // Normal mode: CSI capture + confidence monitoring
    // ═══════════════════════════════════════════════════════
    
    if (!active_camera_->capture(current_frame_)) {
        return false;
    }
    
    vo_result_ = vo_.process(current_frame_, ground_distance);
    frame_count_++;
    
    // Copy features to thread-safe snapshot
    {
        size_t fc = vo_.feature_count();
        if (fc > MAX_FEATURES) fc = MAX_FEATURES;
        std::memcpy(features_snapshot_, vo_.features().data(), fc * sizeof(FeaturePoint));
        features_snapshot_count_.store(static_cast<uint32_t>(fc), std::memory_order_release);
    }
    
    // Compute frame brightness using NEON-accelerated sum (for fallback trigger)
    int pixel_count = current_frame_.info.width * current_frame_.info.height;
    frame_brightness_ = neon::frame_brightness(current_frame_.data, static_cast<size_t>(pixel_count));
    
    // Monitor confidence for fallback trigger
    if (vo_result_.confidence < fallback_config_.conf_drop_thresh) {
        fallback_state_.low_conf_count++;
    } else {
        fallback_state_.low_conf_count = 0;
    }
    
    return true;
}

// ── External Fallback Control ──

void CameraPipeline::activate_fallback(const char* reason) {
    if (external_fallback_.load()) return;  // already active
    
    std::printf("[VO Fallback] ACTIVATED: %s\n", reason);
    
    // Save primary camera reference
    if (!primary_camera_) primary_camera_ = active_camera_;
    
    // Reset VO for thermal focal length
    vo_.reset();
    features_snapshot_count_.store(0, std::memory_order_release);  // clear snapshot
    PlatformConfig thermal_platform = vo_.platform();
    thermal_platform.focal_length_px = fallback_config_.thermal_focal_px;
    vo_.set_platform(thermal_platform);
    set_vo_mode(active_vo_mode());
    
    // Update state
    fallback_state_.source = VOSource::THERMAL_FALLBACK;
    std::snprintf(fallback_state_.reason, sizeof(fallback_state_.reason), "%s", reason);
    fallback_state_.fallback_start_time = runtime_seconds_;
    fallback_state_.fallback_duration = 0;
    fallback_state_.last_csi_probe_time = runtime_seconds_;
    fallback_state_.last_csi_probe_conf = 0;  // Clear stale probe — prevents instant recovery
    fallback_state_.total_switches++;
    
    external_fallback_.store(true, std::memory_order_release);
}

void CameraPipeline::deactivate_fallback() {
    if (!external_fallback_.load()) return;  // not active
    
    std::printf("[VO Fallback] DEACTIVATED — returning to CSI\n");
    
    external_fallback_.store(false, std::memory_order_release);
    
    // Restore CSI camera
    if (primary_camera_) {
        active_camera_ = primary_camera_;
    }
    
    // Reset VO for CSI focal length
    vo_.reset();
    features_snapshot_count_.store(0, std::memory_order_release);  // clear snapshot
    PlatformType current_platform = active_platform();
    set_platform(current_platform);
    set_vo_mode(active_vo_mode());
    
    // Clear state
    fallback_state_.source = VOSource::CSI_PRIMARY;
    std::memset(fallback_state_.reason, 0, sizeof(fallback_state_.reason));
    fallback_state_.fallback_duration = 0;
    fallback_state_.low_conf_count = 0;
}

bool CameraPipeline::inject_frame(const uint8_t* data, uint16_t width, uint16_t height) {
    // SPSC: Python → T6. Only write when state = 0 (idle)
    int expected = 0;
    if (!inject_state_.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        return false;  // T6 is still reading previous frame, skip this one
    }
    
    size_t sz = static_cast<size_t>(width) * height;
    if (sz == 0 || sz > FRAME_SIZE) {
        inject_state_.store(0, std::memory_order_release);
        return false;
    }
    
    std::memcpy(inject_buf_, data, sz);
    inject_w_ = width;
    inject_h_ = height;
    inject_state_.store(2, std::memory_order_release);  // ready for T6
    return true;
}

void CameraPipeline::shutdown() {
    running_ = false;
    if (active_camera_) {
        active_camera_->close();
        active_camera_ = nullptr;
    }
    // Shutdown secondary camera if open
    if (secondary_camera_) {
        secondary_camera_->close();
        secondary_camera_ = nullptr;
        secondary_open_ = false;
    }
}

// ═══════════════════════════════════════════════════════════
// Multi-Camera: Secondary (Thermal) Camera
// ═══════════════════════════════════════════════════════════

bool CameraPipeline::init_secondary(const char* device) {
#ifdef __linux__
    // Try USB camera on specified device (typically /dev/video2 for thermal)
    secondary_usb_ = USBCamera(device);
    if (USBCamera::detect(device)) {
        if (secondary_usb_.open()) {
            secondary_camera_ = &secondary_usb_;
            secondary_open_ = true;
            secondary_frame_count_ = 0;
            secondary_start_us_ = now_us();
            std::printf("[MultiCam] Secondary (thermal) opened: %s\n", device);
            return true;
        }
        std::printf("[MultiCam] Secondary USB detected but failed to open: %s\n", device);
    }
#endif
    // Fallback to simulated secondary for testing
    if (secondary_sim_.open()) {
        secondary_camera_ = &secondary_sim_;
        secondary_open_ = true;
        secondary_frame_count_ = 0;
        secondary_start_us_ = now_us();
        std::printf("[MultiCam] Secondary using simulation (no thermal camera found)\n");
        return true;
    }
    return false;
}

bool CameraPipeline::capture_secondary() {
    if (!secondary_camera_ || !secondary_open_) return false;
    
    if (secondary_camera_->capture(secondary_frame_)) {
        secondary_frame_count_++;
        return true;
    }
    return false;
}

CameraSlotInfo CameraPipeline::get_slot_info(CameraSlot slot) const {
    CameraSlotInfo info;
    info.slot = slot;
    
    if (slot == CameraSlot::PRIMARY) {
        if (active_camera_) {
            info.camera_type = active_camera_->type();
            info.camera_open = active_camera_->is_open();
            info.active = running_;
            info.frame_count = frame_count_;
            info.width = current_frame_.info.width;
            info.height = current_frame_.info.height;
            
            uint64_t elapsed_us = now_us() - start_time_us_;
            if (elapsed_us > 0 && frame_count_ > 1) {
                info.fps_actual = static_cast<float>(frame_count_) * 1'000'000.0f / 
                                 static_cast<float>(elapsed_us);
            }
            
            if (active_camera_->type() == CameraType::PI_CSI) {
                // Include CSI sensor info
                info.csi_sensor = static_cast<uint8_t>(csi_camera_.sensor_type());
                const CSISensorInfo* si = csi_camera_.sensor_info();
                if (si) {
                    std::strncpy(info.sensor_name, si->name, 31);
                    info.sensor_name[31] = '\0';
                    std::snprintf(info.label, sizeof(info.label), "%s (VO)", si->name);
                } else {
                    std::strncpy(info.label, "CSI (VO)", sizeof(info.label) - 1);
                }
                std::strncpy(info.device, "rpicam-vid", 63);
            } else if (active_camera_->type() == CameraType::USB) {
                std::strncpy(info.label, "USB (VO fallback)", sizeof(info.label) - 1);
                std::strncpy(info.device, "/dev/video0", 63);
            } else {
                std::strncpy(info.label, "Simulated (VO)", sizeof(info.label) - 1);
                std::strncpy(info.device, "simulated", 63);
            }
            info.label[sizeof(info.label) - 1] = '\0';
            info.device[63] = '\0';
        }
    } else if (slot == CameraSlot::SECONDARY) {
        if (secondary_camera_) {
            info.camera_type = secondary_camera_->type();
            info.camera_open = secondary_open_;
            info.active = secondary_open_;
            info.frame_count = secondary_frame_count_;
            info.width = secondary_frame_.info.width;
            info.height = secondary_frame_.info.height;
            
            uint64_t elapsed_us = now_us() - secondary_start_us_;
            if (elapsed_us > 0 && secondary_frame_count_ > 1) {
                info.fps_actual = static_cast<float>(secondary_frame_count_) * 1'000'000.0f / 
                                 static_cast<float>(elapsed_us);
            }
            std::strncpy(info.label, "USB Thermal (Down)", sizeof(info.label) - 1);
            info.label[sizeof(info.label) - 1] = '\0';
            std::strncpy(info.device, usb_device_buf_, 63);
            info.device[63] = '\0';
        }
    }
    
    return info;
}

uint8_t CameraPipeline::camera_count() const {
    uint8_t count = 0;
    if (active_camera_ && active_camera_->is_open()) count++;
    if (secondary_camera_ && secondary_open_) count++;
    return count;
}

CameraPipelineStats CameraPipeline::get_stats() const {
    CameraPipelineStats stats;
    
    if (active_camera_) {
        stats.camera_type = active_camera_->type();
        stats.camera_open = active_camera_->is_open();
    }
    
    stats.frame_count = frame_count_;
    stats.width = current_frame_.info.width;
    stats.height = current_frame_.info.height;
    
    uint64_t elapsed_us = now_us() - start_time_us_;
    if (elapsed_us > 0 && frame_count_ > 1) {
        stats.fps_actual = static_cast<float>(frame_count_) * 1'000'000.0f / 
                          static_cast<float>(elapsed_us);
    }
    
    stats.vo_features_detected = vo_result_.features_detected;
    stats.vo_features_tracked  = vo_result_.features_tracked;
    stats.vo_inlier_count      = vo_result_.inlier_count;
    stats.vo_tracking_quality  = vo_result_.tracking_quality;
    stats.vo_confidence        = vo_result_.confidence;
    stats.vo_position_uncertainty = vo_result_.position_uncertainty;
    stats.vo_total_distance    = vo_.total_distance();
    stats.vo_dx = vo_result_.dx;
    stats.vo_dy = vo_result_.dy;
    stats.vo_dz = vo_result_.dz;
    stats.vo_vx = vo_result_.vx;
    stats.vo_vy = vo_result_.vy;
    stats.vo_valid = vo_result_.valid;
    
    // Platform info
    stats.platform = static_cast<uint8_t>(vo_.platform().type);
    std::strncpy(stats.platform_name, vo_.platform().name, 31);
    stats.platform_name[31] = '\0';
    
    // VO Mode info
    stats.vo_mode = static_cast<uint8_t>(vo_.vo_mode().type);
    std::strncpy(stats.vo_mode_name, vo_.vo_mode().name, 31);
    stats.vo_mode_name[31] = '\0';
    
    // Adaptive parameters
    stats.altitude_zone = static_cast<uint8_t>(vo_.adaptive_params().zone);
    stats.adaptive_fast_thresh = static_cast<float>(vo_.adaptive_params().fast_threshold);
    stats.adaptive_lk_window = static_cast<float>(vo_.adaptive_params().lk_window_size);
    
    // Hover state
    stats.hover_detected = vo_.hover_state().is_hovering;
    stats.hover_duration = vo_.hover_state().hover_duration_sec;
    stats.yaw_drift_rate = vo_.hover_state().yaw_drift_rate;
    stats.corrected_yaw  = vo_.hover_state().corrected_yaw;
    
    // CSI sensor info
    stats.csi_sensor_type = static_cast<uint8_t>(csi_camera_.sensor_type());
    if (csi_camera_.sensor_type() == CSISensorType::GENERIC) {
        std::strncpy(stats.csi_sensor_name, PiCSICamera::detected_raw_name(), 47);
    } else if (csi_camera_.sensor_info()) {
        std::strncpy(stats.csi_sensor_name, csi_camera_.sensor_info()->name, 47);
    } else {
        std::strncpy(stats.csi_sensor_name, "none", 47);
    }
    stats.csi_sensor_name[47] = '\0';
    
    // VO Fallback state
    stats.vo_source = static_cast<uint8_t>(fallback_state_.source);
    std::strncpy(stats.vo_fallback_reason, fallback_state_.reason, 63);
    stats.vo_fallback_reason[63] = '\0';
    stats.vo_fallback_duration = fallback_state_.fallback_duration;
    stats.vo_fallback_switches = fallback_state_.total_switches;
    stats.frame_brightness = frame_brightness_;
    
    return stats;
}

} // namespace jtzero
