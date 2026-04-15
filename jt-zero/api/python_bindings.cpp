/**
 * JT-Zero Python Bindings (pybind11)
 * 
 * Exposes the C++ Runtime to Python for use with FastAPI.
 * All data returned as Python dicts for JSON serialization.
 * 
 * NOTE: This file is compiled WITH exceptions and RTTI 
 * (required by pybind11), even though the core library uses neither.
 */

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include <mutex>

#include "jt_zero/runtime.h"

namespace py = pybind11;
using namespace pybind11::literals;

// ─── Helper: Convert sensor data to Python dict ──────────

static py::dict imu_to_dict(const jtzero::IMUData& d) {
    return py::dict(
        "gyro_x"_a = d.gyro_x, "gyro_y"_a = d.gyro_y, "gyro_z"_a = d.gyro_z,
        "acc_x"_a = d.acc_x, "acc_y"_a = d.acc_y, "acc_z"_a = d.acc_z,
        "valid"_a = d.valid
    );
}

static py::dict baro_to_dict(const jtzero::BarometerData& d) {
    return py::dict(
        "pressure"_a = d.pressure, "altitude"_a = d.altitude,
        "temperature"_a = d.temperature, "valid"_a = d.valid
    );
}

static py::dict gps_to_dict(const jtzero::GPSData& d) {
    return py::dict(
        "lat"_a = d.lat, "lon"_a = d.lon, "alt"_a = d.alt,
        "speed"_a = d.speed, "satellites"_a = static_cast<int>(d.satellites),
        "fix_type"_a = static_cast<int>(d.fix_type), "valid"_a = d.valid
    );
}

static py::dict range_to_dict(const jtzero::RangefinderData& d) {
    return py::dict(
        "distance"_a = d.distance,
        "signal_quality"_a = d.signal_quality,
        "valid"_a = d.valid
    );
}

static py::dict flow_to_dict(const jtzero::OpticalFlowData& d) {
    return py::dict(
        "flow_x"_a = d.flow_x, "flow_y"_a = d.flow_y,
        "quality"_a = static_cast<int>(d.quality),
        "ground_distance"_a = d.ground_distance,
        "valid"_a = d.valid
    );
}

// ─── Helper: SystemState to Python dict ──────────────────

static py::dict state_to_dict(const jtzero::SystemState& s) {
    return py::dict(
        "flight_mode"_a = jtzero::flight_mode_str(s.flight_mode),
        "armed"_a = s.armed,
        "battery_voltage"_a = s.battery_voltage,
        "battery_percent"_a = s.battery_percent,
        "cpu_usage"_a = s.cpu_usage,
        "ram_usage_mb"_a = s.ram_usage_mb,
        "cpu_temp"_a = s.cpu_temp,
        "uptime_sec"_a = s.uptime_sec,
        "event_count"_a = s.event_count,
        "error_count"_a = s.error_count,
        "roll"_a = s.roll,
        "pitch"_a = s.pitch,
        "yaw"_a = s.yaw,
        "altitude_agl"_a = s.altitude_agl,
        "vx"_a = s.vx, "vy"_a = s.vy, "vz"_a = s.vz,
        "pos_n"_a = s.pos_n, "pos_e"_a = s.pos_e, "pos_d"_a = s.pos_d,
        "target_altitude"_a = s.target_altitude,
        "motor"_a = py::list(py::cast(std::vector<float>{s.motor[0], s.motor[1], s.motor[2], s.motor[3]})),
        "imu"_a = imu_to_dict(s.imu),
        "baro"_a = baro_to_dict(s.baro),
        "gps"_a = gps_to_dict(s.gps),
        "rangefinder"_a = range_to_dict(s.range),
        "optical_flow"_a = flow_to_dict(s.flow)
    );
}

// ─── Helper: Thread stats to Python list ─────────────────

static py::list thread_stats_to_list(const jtzero::Runtime& rt) {
    py::list result;
    for (int i = 0; i < 8; ++i) {
        auto ts = rt.get_thread_stats(i);
        result.append(py::dict(
            "name"_a = std::string(ts.name ? ts.name : ""),
            "target_hz"_a = jtzero::THREAD_CONFIGS[i].target_hz,
            "actual_hz"_a = ts.actual_hz,
            "cpu_percent"_a = ts.cpu_percent,
            "loop_count"_a = ts.loop_count,
            "max_latency_us"_a = ts.max_latency_us,
            "running"_a = ts.running
        ));
    }
    return result;
}

// ─── Helper: Engine stats to Python dict ─────────────────

static py::dict engine_stats_to_dict(const jtzero::Runtime& rt) {
    return py::dict(
        "events"_a = py::dict(
            "total"_a = rt.events().total_events(),
            "dropped"_a = rt.events().dropped_events(),
            "pending"_a = rt.events().pending_count(),
            "queue_size"_a = jtzero::EventEngine::QUEUE_SIZE
        ),
        "reflexes"_a = py::dict(
            "rules"_a = rt.reflexes().rule_count(),
            "total_fires"_a = rt.reflexes().total_fires(),
            "avg_latency_us"_a = rt.reflexes().avg_latency_us()
        ),
        "rules"_a = py::dict(
            "count"_a = rt.rules().rule_count(),
            "total_evaluations"_a = rt.rules().total_evaluations()
        ),
        "memory"_a = py::dict(
            "telemetry_records"_a = rt.memory().total_telemetry_records(),
            "event_records"_a = rt.memory().total_event_records(),
            "usage_bytes"_a = rt.memory().memory_usage_bytes()
        ),
        "output"_a = py::dict(
            "total_outputs"_a = rt.output().total_outputs(),
            "pending"_a = rt.output().pending_count()
        )
    );
}

// ─── Helper: Camera stats to Python dict ─────────────────

static py::dict camera_stats_to_dict(const jtzero::Runtime& rt) {
    auto cs = rt.camera().get_stats();
    return py::dict(
        "camera_type"_a = jtzero::camera_type_str(cs.camera_type),
        "camera_open"_a = cs.camera_open,
        "frame_count"_a = cs.frame_count,
        "fps_actual"_a = cs.fps_actual,
        "width"_a = cs.width,
        "height"_a = cs.height,
        "vo_features_detected"_a = cs.vo_features_detected,
        "vo_features_tracked"_a = cs.vo_features_tracked,
        "vo_inlier_count"_a = cs.vo_inlier_count,
        "vo_tracking_quality"_a = cs.vo_tracking_quality,
        "vo_confidence"_a = cs.vo_confidence,
        "vo_position_uncertainty"_a = cs.vo_position_uncertainty,
        "vo_total_distance"_a = cs.vo_total_distance,
        "vo_dx"_a = cs.vo_dx, "vo_dy"_a = cs.vo_dy, "vo_dz"_a = cs.vo_dz,
        "vo_vx"_a = cs.vo_vx, "vo_vy"_a = cs.vo_vy,
        "vo_valid"_a = cs.vo_valid,
        // Platform (auto-detected)
        "platform"_a = static_cast<int>(cs.platform),
        "platform_name"_a = std::string(cs.platform_name),
        // VO Mode (switchable)
        "vo_mode"_a = static_cast<int>(cs.vo_mode),
        "vo_mode_name"_a = std::string(cs.vo_mode_name),
        // Adaptive parameters
        "altitude_zone"_a = static_cast<int>(cs.altitude_zone),
        "adaptive_fast_thresh"_a = cs.adaptive_fast_thresh,
        "adaptive_lk_window"_a = cs.adaptive_lk_window,
        // Hover yaw correction
        "hover_detected"_a = cs.hover_detected,
        "hover_duration"_a = cs.hover_duration,
        "yaw_drift_rate"_a = cs.yaw_drift_rate,
        "corrected_yaw"_a = cs.corrected_yaw,
        // CSI sensor info
        "csi_sensor_type"_a = static_cast<int>(cs.csi_sensor_type),
        "csi_sensor_name"_a = std::string(cs.csi_sensor_name),
        // VO Fallback state
        "vo_source"_a = jtzero::vo_source_str(static_cast<jtzero::VOSource>(cs.vo_source)),
        "vo_fallback_reason"_a = std::string(cs.vo_fallback_reason),
        "vo_fallback_duration"_a = cs.vo_fallback_duration,
        "vo_fallback_switches"_a = cs.vo_fallback_switches,
        "frame_brightness"_a = cs.frame_brightness
    );
}

// ─── Helper: MAVLink stats to Python dict ────────────────

static py::dict mavlink_stats_to_dict(const jtzero::Runtime& rt) {
    auto ms = rt.mavlink().get_stats();
    auto fc = rt.mavlink().get_fc_telemetry();
    
    // Autopilot type string
    std::string ap_str = "Unknown";
    if (fc.heartbeat_valid) {
        if (fc.fc_autopilot == 3) ap_str = "ArduPilot";
        else if (fc.fc_autopilot == 12) ap_str = "PX4";
    } else if (ms.state == jtzero::MAVLinkState::CONNECTED) {
        ap_str = "Simulated";
    }
    
    // MAV_TYPE string
    std::string type_str = "UNKNOWN";
    if (fc.heartbeat_valid) {
        switch (fc.fc_type) {
            case 0: type_str = "GENERIC"; break;
            case 1: type_str = "FIXED_WING"; break;
            case 2: type_str = "QUADROTOR"; break;
            case 3: type_str = "COAXIAL"; break;
            case 4: type_str = "HELICOPTER"; break;
            case 10: type_str = "GROUND_ROVER"; break;
            case 11: type_str = "SURFACE_BOAT"; break;
            case 12: type_str = "SUBMARINE"; break;
            case 13: type_str = "HEXAROTOR"; break;
            case 14: type_str = "OCTOROTOR"; break;
            case 15: type_str = "TRICOPTER"; break;
            case 19: type_str = "VTOL_TAILSITTER"; break;
            case 20: type_str = "VTOL_TILTROTOR"; break;
            case 21: type_str = "VTOL_FIXEDROTOR"; break;
            case 22: type_str = "VTOL_TAILSITTER_DUOROTOR"; break;
            case 29: type_str = "DODECAROTOR"; break;
            default: type_str = "MAV_TYPE_" + std::to_string(fc.fc_type); break;
        }
    }
    
    // Diagnostic msg IDs — always available for debugging
    py::list msg_ids;
    for (size_t i = 0; i < rt.mavlink().diag_unique_count_; i++) {
        msg_ids.append(static_cast<int>(rt.mavlink().diag_msg_ids_[i]));
    }
    
    py::dict result(
        "state"_a = jtzero::mavstate_str(ms.state),
        "messages_sent"_a = ms.messages_sent,
        "messages_received"_a = ms.messages_received,
        "heartbeats_received"_a = ms.heartbeats_received,
        "bytes_received"_a = static_cast<uint64_t>(ms.bytes_received),
        "bytes_sent"_a = static_cast<uint64_t>(ms.bytes_sent),
        "crc_errors"_a = static_cast<uint64_t>(ms.crc_errors),
        "errors"_a = ms.errors,
        "link_quality"_a = ms.link_quality,
        "system_id"_a = static_cast<int>(ms.system_id),
        "component_id"_a = static_cast<int>(ms.component_id),
        "fc_system_id"_a = static_cast<int>(ms.fc_system_id),
        "fc_autopilot"_a = ap_str,
        "fc_firmware"_a = std::string(ms.fc_firmware),
        "fc_type"_a = type_str,
        "fc_armed"_a = ms.fc_armed,
        "transport_info"_a = std::string(ms.transport_info),
        "detected_msg_ids"_a = msg_ids,
        "vision_pos_sent"_a = ms.messages_sent / 3,
        "odometry_sent"_a = ms.messages_sent / 3,
        "optical_flow_sent"_a = ms.messages_sent / 6
    );
    
    // Add FC telemetry if available
    if (fc.heartbeat_valid) {
        result["fc_telemetry"] = py::dict(
            "attitude_valid"_a = fc.attitude_valid,
            "imu_valid"_a = fc.imu_valid,
            "baro_valid"_a = fc.baro_valid,
            "gps_valid"_a = fc.gps_valid,
            "hud_valid"_a = fc.hud_valid,
            "status_valid"_a = fc.status_valid,
            "msg_count"_a = fc.msg_count,
            "battery_voltage"_a = fc.battery_voltage,
            "battery_remaining"_a = static_cast<int>(fc.battery_remaining),
            "gps_fix"_a = static_cast<int>(fc.gps_fix),
            "gps_sats"_a = static_cast<int>(fc.gps_sats)
        );
    }
    
    // Add RC channels if available
    if (fc.rc_valid) {
        py::list rc_list;
        for (int i = 0; i < 18; i++) {
            rc_list.append(static_cast<int>(fc.rc_channels[i]));
        }
        result["rc_channels"] = rc_list;
        result["rc_chancount"] = static_cast<int>(fc.rc_chancount);
        result["rc_rssi"] = static_cast<int>(fc.rc_rssi);
    }
    
    return result;
}

// ─── Helper: Recent events to Python list ────────────────

static py::list recent_events_to_list(const jtzero::Runtime& rt, size_t count) {
    py::list result;
    
    std::vector<jtzero::EventRecord> records(count);
    size_t actual = rt.memory().get_recent_events(records.data(), count);
    
    for (size_t i = 0; i < actual; ++i) {
        auto& e = records[i];
        result.append(py::dict(
            "timestamp"_a = static_cast<double>(e.timestamp_us) / 1000000.0,
            "type"_a = jtzero::event_type_str(e.type),
            "priority"_a = static_cast<int>(e.priority),
            "message"_a = std::string(e.message)
        ));
    }
    
    return result;
}

// ─── Helper: Telemetry history to Python list ────────────

static py::list telemetry_history_to_list(const jtzero::Runtime& rt, size_t count) {
    py::list result;
    
    std::vector<jtzero::TelemetryRecord> records(count);
    size_t actual = rt.memory().get_recent_telemetry(records.data(), count);
    
    for (size_t i = 0; i < actual; ++i) {
        auto& r = records[i];
        result.append(py::dict(
            "timestamp"_a = static_cast<double>(r.timestamp_us) / 1000000.0,
            "roll"_a = r.roll, "pitch"_a = r.pitch, "yaw"_a = r.yaw,
            "altitude"_a = r.baro.altitude,
            "battery_voltage"_a = r.battery_voltage,
            "cpu_usage"_a = r.cpu_usage,
            "imu_gyro_x"_a = r.imu.gyro_x,
            "imu_gyro_y"_a = r.imu.gyro_y,
            "imu_gyro_z"_a = r.imu.gyro_z,
            "imu_acc_x"_a = r.imu.acc_x,
            "imu_acc_y"_a = r.imu.acc_y,
            "imu_acc_z"_a = r.imu.acc_z,
            "baro_pressure"_a = r.baro.pressure,
            "baro_temp"_a = r.baro.temperature,
            "gps_lat"_a = r.gps.lat, "gps_lon"_a = r.gps.lon,
            "gps_alt"_a = r.gps.alt, "gps_speed"_a = r.gps.speed,
            "range_distance"_a = r.range.distance,
            "flow_x"_a = r.flow.flow_x,
            "flow_y"_a = r.flow.flow_y
        ));
    }
    
    return result;
}

// ═════════════════════════════════════════════════════════
// PYBIND11 MODULE DEFINITION
// ═════════════════════════════════════════════════════════

PYBIND11_MODULE(jtzero_native, m) {
    m.doc() = "JT-Zero Native C++ Runtime Python Bindings";
    m.attr("__version__") = "1.0.0";
    m.attr("__runtime__") = "native_cpp";
    
    // ─── Runtime Class ───────────────────────────────────
    py::class_<jtzero::Runtime>(m, "Runtime")
        .def(py::init<>())
        
        // Lifecycle
        .def("initialize", &jtzero::Runtime::initialize,
             "Initialize the runtime (sensors, engines, camera, MAVLink)")
        .def("start", &jtzero::Runtime::start,
             "Start all runtime threads")
        .def("stop", &jtzero::Runtime::stop,
             "Stop all threads and shutdown")
        .def("is_running", &jtzero::Runtime::is_running)
        
        // Mode
        .def("set_simulator_mode", &jtzero::Runtime::set_simulator_mode)
        .def("is_simulator_mode", &jtzero::Runtime::is_simulator_mode)
        
        // Command interface
        .def("send_command", &jtzero::Runtime::send_command,
             py::arg("cmd"), py::arg("param1") = 0.0f, py::arg("param2") = 0.0f,
             "Send a command: arm, disarm, takeoff, land, hold, rtl, emergency")
        
        // Simulator config
        .def("get_sim_config", [](const jtzero::Runtime& self) {
            auto& c = self.sim_config();
            return py::dict(
                "wind_speed"_a = c.wind_speed, "wind_direction"_a = c.wind_direction,
                "sensor_noise"_a = c.sensor_noise, "battery_drain"_a = c.battery_drain,
                "gravity"_a = c.gravity, "mass_kg"_a = c.mass_kg,
                "max_thrust"_a = c.max_thrust, "drag_coeff"_a = c.drag_coeff,
                "turbulence"_a = c.turbulence
            );
        })
        .def("set_sim_config", [](jtzero::Runtime& self, py::dict d) {
            auto& c = self.sim_config();
            if (d.contains("wind_speed"))     c.wind_speed = d["wind_speed"].cast<float>();
            if (d.contains("wind_direction")) c.wind_direction = d["wind_direction"].cast<float>();
            if (d.contains("sensor_noise"))   c.sensor_noise = d["sensor_noise"].cast<float>();
            if (d.contains("battery_drain"))  c.battery_drain = d["battery_drain"].cast<float>();
            if (d.contains("gravity"))        c.gravity = d["gravity"].cast<float>();
            if (d.contains("mass_kg"))        c.mass_kg = d["mass_kg"].cast<float>();
            if (d.contains("drag_coeff"))     c.drag_coeff = d["drag_coeff"].cast<float>();
            if (d.contains("turbulence"))     c.turbulence = d["turbulence"].cast<bool>();
        })
        
        // Data access (returns Python dicts for JSON serialization)
        .def("get_state", [](jtzero::Runtime& self) {
            return state_to_dict(self.state_snapshot());
        }, "Get full system state as dict (snapshot copy under sensor_mutex_ + slow_mutex_)")
        
        .def("get_threads", [](const jtzero::Runtime& self) {
            return thread_stats_to_list(self);
        }, "Get thread statistics as list of dicts")
        
        .def("get_engines", [](const jtzero::Runtime& self) {
            return engine_stats_to_dict(self);
        }, "Get engine statistics as dict")
        
        .def("get_camera", [](const jtzero::Runtime& self) {
            return camera_stats_to_dict(self);
        }, "Get camera pipeline stats as dict")
        
        .def("get_frame_data", [](const jtzero::Runtime& self) {
            const auto& frame = self.camera().current_frame();
            if (!frame.info.valid) return py::bytes();
            return py::bytes(reinterpret_cast<const char*>(frame.data),
                             static_cast<size_t>(frame.info.width) * frame.info.height);
        }, "Get latest camera frame as raw grayscale bytes")
        
        .def("get_features", [](const jtzero::Runtime& self) {
            // Read from thread-safe snapshot (acquire pairs with release in tick())
            const auto& cam = self.camera();
            uint32_t count = cam.features_snapshot_count();
            const auto* feats = cam.features_snapshot();
            py::list result;
            for (uint32_t i = 0; i < count && i < jtzero::MAX_FEATURES; ++i) {
                result.append(py::dict(
                    "x"_a = feats[i].x,
                    "y"_a = feats[i].y,
                    "tracked"_a = feats[i].tracked,
                    "response"_a = feats[i].response
                ));
            }
            return result;
        }, "Get current VO feature positions as list of dicts (thread-safe snapshot)")
        
        .def("get_mavlink", [](const jtzero::Runtime& self) {
            return mavlink_stats_to_dict(self);
        }, "Get MAVLink interface stats as dict")
        
        .def("send_statustext", [](jtzero::Runtime& self, int severity, const std::string& text) {
            return self.mavlink().send_statustext(static_cast<uint8_t>(severity), text.c_str());
        }, py::arg("severity"), py::arg("text"), "Send STATUSTEXT via MAVLink (0=EMERG..6=INFO)")
        
        .def("set_vo_profile", [](jtzero::Runtime& self, int profile_id) {
            if (profile_id >= 0 && profile_id < static_cast<int>(jtzero::NUM_VO_MODES)) {
                self.camera().set_vo_mode(static_cast<jtzero::VOModeType>(profile_id));
                return true;
            }
            return false;
        }, py::arg("profile_id"), "Set VO mode (0=Light, 1=Balanced, 2=Performance)")
        
        .def("get_vo_profiles", []() {
            py::list modes;
            for (size_t i = 0; i < jtzero::NUM_VO_MODES; ++i) {
                auto& m = jtzero::VO_MODES[i];
                modes.append(py::dict(
                    "id"_a = static_cast<int>(i),
                    "name"_a = std::string(m.name),
                    "type"_a = jtzero::vo_mode_str(m.type),
                    "fast_threshold"_a = static_cast<int>(m.fast_threshold),
                    "lk_window"_a = m.lk_window_size,
                    "lk_iterations"_a = m.lk_iterations,
                    "max_features"_a = static_cast<int>(m.max_features)
                ));
            }
            return modes;
        }, "Get available VO modes")
        
        // ── VO Fallback Control ──
        
        .def("activate_fallback", [](jtzero::Runtime& self, const std::string& reason) {
            self.camera().activate_fallback(reason.c_str());
        }, py::arg("reason"), "Activate VO fallback to thermal camera")
        
        .def("deactivate_fallback", [](jtzero::Runtime& self) {
            self.camera().deactivate_fallback();
        }, "Deactivate VO fallback, return to CSI")
        
        .def("inject_frame", [](jtzero::Runtime& self, py::bytes data, int width, int height) {
            std::string buf = data;
            return self.camera().inject_frame(
                reinterpret_cast<const uint8_t*>(buf.data()),
                static_cast<uint16_t>(width),
                static_cast<uint16_t>(height));
        }, py::arg("data"), py::arg("width"), py::arg("height"),
        "Inject external grayscale frame for VO processing (thread-safe)")
        
        .def("is_confidence_low", [](const jtzero::Runtime& self) {
            return self.camera().is_confidence_low();
        }, "Check if CSI confidence is below fallback threshold for N frames")
        
        .def("get_fallback_state", [](const jtzero::Runtime& self) {
            const auto& fs = self.camera().fallback_state();
            return py::dict(
                "source"_a = jtzero::vo_source_str(fs.source),
                "reason"_a = std::string(fs.reason),
                "low_conf_count"_a = static_cast<int>(fs.low_conf_count),
                "fallback_duration"_a = fs.fallback_duration,
                "last_csi_probe_conf"_a = fs.last_csi_probe_conf,
                "total_switches"_a = fs.total_switches
            );
        }, "Get VO fallback state details")
        
        .def("get_sensor_modes", [](const jtzero::Runtime& self) {
            const auto& hw = self.hw_info();
            // Determine actual data source for each sensor:
            // - "hardware" = direct I2C/SPI driver active (hw_info detected + try_hardware succeeded)
            // - "mavlink"  = native mode, data comes from flight controller via MAVLink
            // - "simulated" = simulation mode
            auto sensor_mode = [&](bool hw_detected) -> std::string {
                if (!self.is_simulator_mode()) {
                    return hw_detected ? "hardware" : "mavlink";
                }
                return "simulated";
            };
            return py::dict(
                "imu"_a = sensor_mode(hw.imu_detected),
                "baro"_a = sensor_mode(hw.baro_detected),
                "gps"_a = sensor_mode(hw.gps_detected),
                "rangefinder"_a = self.is_simulator_mode() ? "simulated" : "mavlink",
                "optical_flow"_a = self.is_simulator_mode() ? "simulated" : "mavlink",
                "hw_info"_a = py::dict(
                    "i2c_available"_a = hw.i2c_available,
                    "imu_detected"_a = hw.imu_detected,
                    "baro_detected"_a = hw.baro_detected,
                    "gps_detected"_a = hw.gps_detected,
                    "spi_available"_a = hw.spi_available,
                    "uart_available"_a = hw.uart_available,
                    "imu_model"_a = std::string(hw.imu_model),
                    "baro_model"_a = std::string(hw.baro_model),
                    "gps_model"_a = std::string(hw.gps_model)
                )
            );
        }, "Get sensor modes (hardware vs mavlink vs simulated) and detection info")
        
        .def("get_events", [](const jtzero::Runtime& self, size_t count) {
            return recent_events_to_list(self, count);
        }, py::arg("count") = 50, "Get recent events as list of dicts")
        
        .def("get_telemetry_history", [](const jtzero::Runtime& self, size_t count) {
            return telemetry_history_to_list(self, count);
        }, py::arg("count") = 100, "Get telemetry history as list of dicts")
        
        // Performance counters
        .def("get_performance", [](const jtzero::Runtime& self) {
            py::dict perf;
            
            // Thread performance
            py::list threads;
            double total_cpu = 0;
            for (int i = 0; i < 8; ++i) {
                auto ts = self.get_thread_stats(i);
                total_cpu += ts.cpu_percent;
                threads.append(py::dict(
                    "name"_a = std::string(ts.name ? ts.name : ""),
                    "actual_hz"_a = ts.actual_hz,
                    "cpu_percent"_a = ts.cpu_percent,
                    "max_latency_us"_a = ts.max_latency_us,
                    "running"_a = ts.running
                ));
            }
            perf["threads"] = threads;
            perf["total_cpu_percent"] = total_cpu;
            
            // Memory usage
            size_t mem_engines = self.memory().memory_usage_bytes();
            size_t mem_event_queue = sizeof(jtzero::Event) * jtzero::EventEngine::QUEUE_SIZE;
            size_t mem_camera = sizeof(jtzero::FrameBuffer) * 2; // Double buffer
            size_t mem_total = mem_engines + mem_event_queue + mem_camera;
            
            perf["memory"] = py::dict(
                "engines_bytes"_a = mem_engines,
                "event_queue_bytes"_a = mem_event_queue,
                "camera_bytes"_a = mem_camera,
                "total_bytes"_a = mem_total,
                "total_mb"_a = static_cast<double>(mem_total) / (1024.0 * 1024.0)
            );
            
            // Latency
            perf["latency"] = py::dict(
                "reflex_avg_us"_a = self.reflexes().avg_latency_us(),
                "reflex_fires"_a = self.reflexes().total_fires()
            );
            
            // Throughput
            perf["throughput"] = py::dict(
                "events_total"_a = self.events().total_events(),
                "events_dropped"_a = self.events().dropped_events(),
                "drop_rate"_a = (self.events().total_events() > 0) ?
                    static_cast<double>(self.events().dropped_events()) / 
                    static_cast<double>(self.events().total_events()) : 0.0,
                "telemetry_records"_a = self.memory().total_telemetry_records(),
                "output_commands"_a = self.output().total_outputs()
            );
            
            return perf;
        }, "Get detailed performance metrics")
    ;
    
    // ─── Utility functions ───────────────────────────────
    m.def("get_build_info", []() {
        return py::dict(
            "version"_a = "1.0.0",
            "runtime"_a = "native_cpp",
            "compiler"_a = 
#ifdef __GNUC__
                "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__),
#else
                "unknown",
#endif
            "cpp_standard"_a = __cplusplus,
            "platform"_a = 
#ifdef __aarch64__
                "aarch64",
#elif defined(__arm__)
                "arm",
#elif defined(__x86_64__)
                "x86_64",
#else
                "unknown",
#endif
            "ring_buffer_size"_a = jtzero::EventEngine::QUEUE_SIZE,
            "max_features"_a = jtzero::MAX_FEATURES,
            "frame_width"_a = jtzero::FRAME_WIDTH,
            "frame_height"_a = jtzero::FRAME_HEIGHT,
            "max_frame_width"_a = jtzero::MAX_FRAME_WIDTH,
            "max_frame_height"_a = jtzero::MAX_FRAME_HEIGHT,
            "num_platforms"_a = static_cast<int>(jtzero::NUM_PLATFORMS),
            "num_vo_modes"_a = static_cast<int>(jtzero::NUM_VO_MODES),
            "telemetry_history"_a = jtzero::MemoryEngine::TELEMETRY_HISTORY,
            "event_history"_a = jtzero::MemoryEngine::EVENT_HISTORY
        );
    }, "Get build information and compile-time constants");
}
