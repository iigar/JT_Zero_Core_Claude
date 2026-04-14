/**
 * JT-Zero Main Entry Point
 * Standalone runtime for Raspberry Pi Zero 2 W
 * 
 * Usage: ./jt-zero [--simulator] [--duration <seconds>]
 */

#include "jt_zero/runtime.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>

static jtzero::Runtime* g_runtime = nullptr;

void signal_handler(int sig) {
    std::printf("\n[JT-Zero] Signal %d received, shutting down...\n", sig);
    if (g_runtime) {
        g_runtime->stop();
    }
}

void print_banner() {
    std::printf(R"(
     ╔══════════════════════════════════════════╗
     ║           JT-Zero Runtime v1.0           ║
     ║    Lightweight Drone Autonomy Engine     ║
     ║       Raspberry Pi Zero 2 W Ready        ║
     ╚══════════════════════════════════════════╝
)");
}

void print_status(const jtzero::Runtime& rt) {
    // state_snapshot() returns a copy under both mutexes — safe for main thread
    const auto s = rt.state_snapshot();
    
    std::printf("\n─── System Status ────────────────────────\n");
    std::printf("  Mode:    %-12s  Armed: %s\n", 
                jtzero::flight_mode_str(s.flight_mode),
                s.armed ? "YES" : "NO");
    std::printf("  Battery: %.1fV (%.0f%%)  CPU: %.1f°C\n",
                s.battery_voltage, s.battery_percent, s.cpu_temp);
    std::printf("  Uptime:  %us  Events: %u\n",
                s.uptime_sec, s.event_count);
    
    std::printf("\n─── Sensors ──────────────────────────────\n");
    std::printf("  IMU:   gyro(%.3f, %.3f, %.3f) acc(%.2f, %.2f, %.2f)\n",
                s.imu.gyro_x, s.imu.gyro_y, s.imu.gyro_z,
                s.imu.acc_x, s.imu.acc_y, s.imu.acc_z);
    std::printf("  Baro:  alt=%.2fm  press=%.1fhPa  temp=%.1f°C\n",
                s.baro.altitude, s.baro.pressure, s.baro.temperature);
    std::printf("  GPS:   %.6f, %.6f  alt=%.1fm  spd=%.1fm/s  sat=%d\n",
                s.gps.lat, s.gps.lon, s.gps.alt, s.gps.speed, s.gps.satellites);
    std::printf("  Range: %.2fm (q=%.0f%%)\n",
                s.range.distance, s.range.signal_quality * 100);
    std::printf("  Flow:  (%.3f, %.3f) q=%d\n",
                s.flow.flow_x, s.flow.flow_y, s.flow.quality);
    
    std::printf("\n─── Attitude ─────────────────────────────\n");
    std::printf("  Roll: %.2f°  Pitch: %.2f°  Yaw: %.2f°\n",
                s.roll, s.pitch, s.yaw);
    std::printf("  AGL:  %.2fm\n", s.altitude_agl);
    
    std::printf("\n─── Threads ──────────────────────────────\n");
    for (int i = 0; i < 5; ++i) {
        auto ts = rt.get_thread_stats(i);
        std::printf("  %s: %s  loops=%lu  max_lat=%luus\n",
                    ts.name, ts.running ? "RUN" : "---",
                    ts.loop_count, ts.max_latency_us);
    }
    
    std::printf("\n─── Engines ──────────────────────────────\n");
    std::printf("  Events:   total=%lu  dropped=%lu  pending=%zu\n",
                rt.events().total_events(), rt.events().dropped_events(),
                rt.events().pending_count());
    std::printf("  Reflexes: rules=%zu  fires=%lu  avg_lat=%.1fus\n",
                rt.reflexes().rule_count(), rt.reflexes().total_fires(),
                rt.reflexes().avg_latency_us());
    std::printf("  Rules:    count=%zu  evals=%lu\n",
                rt.rules().rule_count(), rt.rules().total_evaluations());
    std::printf("  Memory:   telem=%lu  events=%lu  usage=%zuB\n",
                rt.memory().total_telemetry_records(),
                rt.memory().total_event_records(),
                rt.memory().memory_usage_bytes());
}

int main(int argc, char* argv[]) {
    print_banner();
    
    bool simulator = true;
    int duration = 10;
    
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-simulator") == 0) {
            simulator = false;
        } else if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration = std::atoi(argv[++i]);
        }
    }
    
    // Setup signal handling
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // Create and initialize runtime
    jtzero::Runtime runtime;
    g_runtime = &runtime;
    
    runtime.set_simulator_mode(simulator);
    
    if (!runtime.initialize()) {
        std::printf("[JT-Zero] Initialization failed!\n");
        return 1;
    }
    
    // Start runtime
    runtime.start();
    
    std::printf("[JT-Zero] Running for %d seconds (Ctrl+C to stop)...\n", duration);
    
    // Status display loop
    for (int i = 0; i < duration && runtime.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        print_status(runtime);
    }
    
    // Stop
    runtime.stop();
    
    std::printf("\n[JT-Zero] Final status:\n");
    print_status(runtime);
    std::printf("\n[JT-Zero] Shutdown complete.\n");
    
    return 0;
}
