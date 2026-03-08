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
        "vo_tracking_quality"_a = cs.vo_tracking_quality,
        "vo_dx"_a = cs.vo_dx, "vo_dy"_a = cs.vo_dy, "vo_dz"_a = cs.vo_dz,
        "vo_vx"_a = cs.vo_vx, "vo_vy"_a = cs.vo_vy,
        "vo_valid"_a = cs.vo_valid
    );
}

// ─── Helper: MAVLink stats to Python dict ────────────────

static py::dict mavlink_stats_to_dict(const jtzero::Runtime& rt) {
    auto ms = rt.mavlink().get_stats();
    return py::dict(
        "state"_a = jtzero::mavstate_str(ms.state),
        "messages_sent"_a = ms.messages_sent,
        "messages_received"_a = ms.messages_received,
        "errors"_a = ms.errors,
        "link_quality"_a = ms.link_quality,
        "system_id"_a = static_cast<int>(ms.system_id),
        "component_id"_a = static_cast<int>(ms.component_id),
        "fc_system_id"_a = static_cast<int>(ms.fc_system_id),
        "fc_autopilot"_a = "ArduPilot",
        "fc_firmware"_a = std::string(ms.fc_firmware),
        "fc_type"_a = "QUADROTOR",
        "fc_armed"_a = ms.fc_armed,
        "vision_pos_sent"_a = ms.messages_sent / 3,
        "odometry_sent"_a = ms.messages_sent / 3,
        "optical_flow_sent"_a = ms.messages_sent / 6
    );
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
        
        // Data access (returns Python dicts for JSON serialization)
        .def("get_state", [](const jtzero::Runtime& self) {
            return state_to_dict(self.state());
        }, "Get full system state as dict")
        
        .def("get_threads", [](const jtzero::Runtime& self) {
            return thread_stats_to_list(self);
        }, "Get thread statistics as list of dicts")
        
        .def("get_engines", [](const jtzero::Runtime& self) {
            return engine_stats_to_dict(self);
        }, "Get engine statistics as dict")
        
        .def("get_camera", [](const jtzero::Runtime& self) {
            return camera_stats_to_dict(self);
        }, "Get camera pipeline stats as dict")
        
        .def("get_mavlink", [](const jtzero::Runtime& self) {
            return mavlink_stats_to_dict(self);
        }, "Get MAVLink interface stats as dict")
        
        .def("get_events", [](const jtzero::Runtime& self, size_t count) {
            return recent_events_to_list(self, count);
        }, py::arg("count") = 50, "Get recent events as list of dicts")
        
        .def("get_telemetry_history", [](const jtzero::Runtime& self, size_t count) {
            return telemetry_history_to_list(self, count);
        }, py::arg("count") = 100, "Get telemetry history as list of dicts")
        
        // Performance counters
        .def("get_performance", [](const jtzero::Runtime& self) {
            auto s = self.state();
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
            "telemetry_history"_a = jtzero::MemoryEngine::TELEMETRY_HISTORY,
            "event_history"_a = jtzero::MemoryEngine::EVENT_HISTORY
        );
    }, "Get build information and compile-time constants");
}
