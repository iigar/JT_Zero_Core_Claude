#!/usr/bin/env python3
"""
JT-Zero Ground Station Backend API Testing
Tests all REST endpoints and functionality
"""

import requests
import sys
import json
import time
from datetime import datetime

class JTZeroTester:
    def __init__(self, base_url="https://pi-reflex-core.preview.emergentagent.com"):
        self.base_url = base_url
        self.tests_run = 0
        self.tests_passed = 0
        self.failed_tests = []
        self.session = requests.Session()

    def log(self, message):
        print(f"[{datetime.now().strftime('%H:%M:%S')}] {message}")

    def run_test(self, name, method, endpoint, expected_status, data=None, validate_response=None):
        """Run a single API test with validation"""
        url = f"{self.base_url}{endpoint}"
        headers = {'Content-Type': 'application/json'}

        self.tests_run += 1
        self.log(f"🔍 Testing {name}...")
        
        try:
            if method == 'GET':
                response = self.session.get(url, headers=headers, timeout=10)
            elif method == 'POST':
                response = self.session.post(url, json=data, headers=headers, timeout=10)
            
            # Check status code
            status_ok = response.status_code == expected_status
            if not status_ok:
                self.log(f"❌ {name} - Status Failed - Expected {expected_status}, got {response.status_code}")
                self.failed_tests.append(f"{name}: Status {response.status_code} != {expected_status}")
                return False, {}
            
            # Try to parse JSON
            try:
                response_data = response.json()
            except:
                self.log(f"❌ {name} - Invalid JSON response")
                self.failed_tests.append(f"{name}: Invalid JSON response")
                return False, {}
            
            # Custom validation
            if validate_response:
                validation_result = validate_response(response_data)
                if validation_result is not True:
                    self.log(f"❌ {name} - Validation Failed: {validation_result}")
                    self.failed_tests.append(f"{name}: {validation_result}")
                    return False, response_data
            
            self.tests_passed += 1
            self.log(f"✅ {name} - Passed")
            return True, response_data

        except requests.RequestException as e:
            self.log(f"❌ {name} - Network Error: {str(e)}")
            self.failed_tests.append(f"{name}: Network error - {str(e)}")
            return False, {}
        except Exception as e:
            self.log(f"❌ {name} - Error: {str(e)}")
            self.failed_tests.append(f"{name}: Exception - {str(e)}")
            return False, {}

    def validate_health(self, data):
        required_fields = ['status', 'runtime', 'version', 'mode', 'native']
        for field in required_fields:
            if field not in data:
                return f"Missing field: {field}"
        if data['status'] != 'ok':
            return f"Status not ok: {data['status']}"
        if data['runtime'] != 'jt-zero':
            return f"Invalid runtime: {data['runtime']}"
        
        # Phase 7: Check native runtime is active
        if data['mode'] != 'native':
            return f"Expected native mode, got: {data['mode']}"
        if not data['native']:
            return f"Expected native=true, got: {data['native']}"
        
        # Check build info exists
        if 'build_info' not in data:
            return "Missing build_info for native runtime"
        
        build_info = data['build_info']
        required_build_fields = ['compiler', 'cpp_standard', 'platform']
        for field in required_build_fields:
            if field not in build_info:
                return f"Missing build_info field: {field}"
        
        return True

    def validate_state(self, data):
        required_fields = ['flight_mode', 'armed', 'battery_voltage', 'imu', 'baro', 'gps', 'rangefinder', 'optical_flow']
        for field in required_fields:
            if field not in data:
                return f"Missing field: {field}"
        
        # Check sensor data structure
        sensor_checks = [
            ('imu', ['gyro_x', 'gyro_y', 'gyro_z', 'acc_x', 'acc_y', 'acc_z', 'valid']),
            ('baro', ['pressure', 'altitude', 'temperature', 'valid']),
            ('gps', ['lat', 'lon', 'alt', 'speed', 'satellites', 'fix_type', 'valid']),
            ('rangefinder', ['distance', 'signal_quality', 'valid']),
            ('optical_flow', ['flow_x', 'flow_y', 'quality', 'ground_distance', 'valid'])
        ]
        
        for sensor_name, required_sensor_fields in sensor_checks:
            if sensor_name not in data:
                return f"Missing sensor: {sensor_name}"
            for field in required_sensor_fields:
                if field not in data[sensor_name]:
                    return f"Missing {sensor_name} field: {field}"
        
        return True

    def validate_events(self, data):
        if not isinstance(data, list):
            return "Events should be a list"
        if len(data) > 0:
            event = data[0]
            required_fields = ['timestamp', 'type', 'priority', 'message']
            for field in required_fields:
                if field not in event:
                    return f"Event missing field: {field}"
        return True

    def validate_telemetry(self, data):
        required_fields = ['state', 'threads', 'engines']
        for field in required_fields:
            if field not in data:
                return f"Missing field: {field}"
        return True

    def validate_threads(self, data):
        if not isinstance(data, list):
            return "Threads should be a list"
        if len(data) > 0:
            thread = data[0]
            required_fields = ['name', 'target_hz', 'actual_hz', 'running']
            for field in required_fields:
                if field not in thread:
                    return f"Thread missing field: {field}"
        return True

    def validate_engines(self, data):
        if not isinstance(data, dict):
            return "Engines should be a dict"
        required_keys = ['events', 'reflexes', 'rules', 'memory', 'output']
        for key in required_keys:
            if key not in data:
                return f"Missing engine: {key}"
        return True

    def validate_command_response(self, data):
        required_fields = ['success', 'message', 'command']
        for field in required_fields:
            if field not in data:
                return f"Missing field: {field}"
        return True

    def validate_camera_stats(self, data):
        """Validate camera/visual odometry stats structure"""
        required_fields = ['camera_type', 'fps_actual', 'frame_count', 'width', 'height', 
                          'vo_features_detected', 'vo_features_tracked', 'vo_tracking_quality',
                          'vo_dx', 'vo_dy', 'vo_valid']
        for field in required_fields:
            if field not in data:
                return f"Missing camera field: {field}"
        
        # Validate data types and ranges
        if not isinstance(data['fps_actual'], (int, float)):
            return "fps_actual should be numeric"
        if not isinstance(data['frame_count'], int):
            return "frame_count should be integer"
        if data['vo_tracking_quality'] < 0 or data['vo_tracking_quality'] > 1:
            return "vo_tracking_quality should be between 0 and 1"
        
        return True

    def validate_mavlink_stats(self, data):
        """Validate MAVLink stats structure"""
        required_fields = ['state', 'messages_sent', 'messages_received', 'link_quality',
                          'fc_autopilot', 'fc_firmware', 'vision_pos_sent', 'odometry_sent', 
                          'optical_flow_sent']
        for field in required_fields:
            if field not in data:
                return f"Missing mavlink field: {field}"
        
        # Validate state values
        valid_states = ['CONNECTED', 'CONNECTING', 'DISCONNECTED', 'LOST']
        if data['state'] not in valid_states:
            return f"Invalid state: {data['state']} (expected one of {valid_states})"
        
        # Validate counters are non-negative integers
        counters = ['messages_sent', 'messages_received', 'vision_pos_sent', 'odometry_sent', 'optical_flow_sent']
        for counter in counters:
            if not isinstance(data[counter], int) or data[counter] < 0:
                return f"{counter} should be non-negative integer"
        
        # Validate link quality is between 0 and 1
        if data['link_quality'] < 0 or data['link_quality'] > 1:
            return "link_quality should be between 0 and 1"
        
        return True

    def validate_performance(self, data):
        """Validate performance endpoint (Phase 10)"""
        if 'error' in data:
            return f"Performance error: {data['error']}"
        
        required_sections = ['threads', 'memory', 'latency', 'throughput', 'total_cpu_percent']
        for section in required_sections:
            if section not in data:
                return f"Missing performance section: {section}"
        
        # Validate threads (should have 7 running)
        threads = data.get('threads', [])
        if not isinstance(threads, list):
            return "threads should be a list"
        
        running_threads = len([t for t in threads if t.get('running')])
        if running_threads != 7:
            return f"Expected 7 running threads, got {running_threads}"
        
        # Validate memory usage is reasonable (< 5MB total)
        memory = data.get('memory', {})
        total_mb = memory.get('total_mb', 0)
        if total_mb >= 5:
            return f"Memory usage too high: {total_mb:.2f}MB (expected < 5MB)"
        
        # Validate throughput has zero dropped events
        throughput = data.get('throughput', {})
        events_dropped = throughput.get('events_dropped', 1)  # Default to 1 to fail if missing
        drop_rate = throughput.get('drop_rate', 1.0)
        
        if events_dropped != 0:
            return f"Events dropped: {events_dropped} (expected 0)"
        if drop_rate != 0:
            return f"Drop rate: {drop_rate:.3f} (expected 0)"
        
        return True

    def test_all_endpoints(self):
        """Test all JT-Zero API endpoints"""
        self.log("🚀 Starting JT-Zero Backend API Tests (Iteration 3 - Native C++ Runtime + Performance)")
        
        # Test health endpoint (must show native mode)
        self.run_test(
            "Health Check (Native Runtime)", 
            "GET", 
            "/api/health", 
            200, 
            validate_response=self.validate_health
        )
        
        # Test state endpoint
        success, state_data = self.run_test(
            "System State", 
            "GET", 
            "/api/state", 
            200, 
            validate_response=self.validate_state
        )
        
        # Test NEW performance endpoint (Phase 10)
        self.run_test(
            "Performance Metrics", 
            "GET", 
            "/api/performance", 
            200, 
            validate_response=self.validate_performance
        )
        
        # Test events endpoint
        self.run_test(
            "Events List", 
            "GET", 
            "/api/events", 
            200, 
            validate_response=self.validate_events
        )
        
        # Test telemetry endpoint
        self.run_test(
            "Telemetry Data", 
            "GET", 
            "/api/telemetry", 
            200, 
            validate_response=self.validate_telemetry
        )
        
        # Test telemetry history
        self.run_test(
            "Telemetry History", 
            "GET", 
            "/api/telemetry/history", 
            200,
            validate_response=lambda data: True if isinstance(data, list) else "Should be a list"
        )
        
        # Test threads endpoint
        self.run_test(
            "Thread Stats", 
            "GET", 
            "/api/threads", 
            200, 
            validate_response=self.validate_threads
        )
        
        # Test engines endpoint
        self.run_test(
            "Engine Stats", 
            "GET", 
            "/api/engines", 
            200, 
            validate_response=self.validate_engines
        )
        
        # Test Camera endpoint (C++ pipeline)
        self.run_test(
            "Camera Stats (C++ Pipeline)", 
            "GET", 
            "/api/camera", 
            200, 
            validate_response=self.validate_camera_stats
        )
        
        # Test MAVLink endpoint (C++ MAVLink)
        self.run_test(
            "MAVLink Stats (C++ MAVLink)", 
            "GET", 
            "/api/mavlink", 
            200, 
            validate_response=self.validate_mavlink_stats
        )
        
        # Test command endpoints (through C++ runtime)
        self.test_commands(state_data)

    def test_commands(self, initial_state):
        """Test command functionality with state validation"""
        self.log("🎮 Testing Command Functionality")
        
        # Test ARM command
        success, response = self.run_test(
            "ARM Command", 
            "POST", 
            "/api/command", 
            200, 
            data={"command": "arm", "param1": 0, "param2": 0},
            validate_response=self.validate_command_response
        )
        
        if success and response.get('success'):
            # Verify state changed to ARMED
            time.sleep(0.5)  # Wait for state update
            success, new_state = self.run_test(
                "Verify ARM State", 
                "GET", 
                "/api/state", 
                200
            )
            if success and new_state.get('armed') and new_state.get('flight_mode') == 'ARMED':
                self.log("✅ ARM command changed state correctly")
                self.tests_passed += 1
            else:
                self.log("❌ ARM command did not change state")
                self.failed_tests.append("ARM command: State not updated")
        
        # Test TAKEOFF command (should work when armed)
        success, response = self.run_test(
            "TAKEOFF Command", 
            "POST", 
            "/api/command", 
            200, 
            data={"command": "takeoff", "param1": 10, "param2": 0},
            validate_response=self.validate_command_response
        )
        
        if success and response.get('success'):
            time.sleep(0.5)
            success, new_state = self.run_test(
                "Verify TAKEOFF State", 
                "GET", 
                "/api/state", 
                200
            )
            if success and new_state.get('flight_mode') in ['TAKEOFF', 'HOVER']:
                self.log("✅ TAKEOFF command changed state correctly")
                self.tests_passed += 1
            else:
                self.log("❌ TAKEOFF command did not change state")
                self.failed_tests.append("TAKEOFF command: State not updated")
        
        # Test LAND command
        self.run_test(
            "LAND Command", 
            "POST", 
            "/api/command", 
            200, 
            data={"command": "land"},
            validate_response=self.validate_command_response
        )
        
        # Test RTL command
        self.run_test(
            "RTL Command", 
            "POST", 
            "/api/command", 
            200, 
            data={"command": "rtl"},
            validate_response=self.validate_command_response
        )
        
        # Test HOLD command
        self.run_test(
            "HOLD Command", 
            "POST", 
            "/api/command", 
            200, 
            data={"command": "hold"},
            validate_response=self.validate_command_response
        )
        
        # Test DISARM command
        success, response = self.run_test(
            "DISARM Command", 
            "POST", 
            "/api/command", 
            200, 
            data={"command": "disarm"},
            validate_response=self.validate_command_response
        )
        
        if success and response.get('success'):
            time.sleep(0.5)
            success, new_state = self.run_test(
                "Verify DISARM State", 
                "GET", 
                "/api/state", 
                200
            )
            if success and not new_state.get('armed') and new_state.get('flight_mode') == 'IDLE':
                self.log("✅ DISARM command changed state correctly")
                self.tests_passed += 1
            else:
                self.log("❌ DISARM command did not change state")
                self.failed_tests.append("DISARM command: State not updated")
        
        # Test EMERGENCY command
        self.run_test(
            "EMERGENCY Command", 
            "POST", 
            "/api/command", 
            200, 
            data={"command": "emergency"},
            validate_response=self.validate_command_response
        )

    def print_summary(self):
        """Print test summary"""
        self.log("=" * 60)
        self.log(f"📊 Test Summary: {self.tests_passed}/{self.tests_run} passed")
        
        if self.failed_tests:
            self.log("❌ Failed Tests:")
            for failure in self.failed_tests:
                self.log(f"  - {failure}")
        else:
            self.log("✅ All tests passed!")
        
        return len(self.failed_tests) == 0

def main():
    """Main test execution"""
    tester = JTZeroTester()
    
    # Run all tests
    tester.test_all_endpoints()
    
    # Print results
    success = tester.print_summary()
    
    return 0 if success else 1

if __name__ == "__main__":
    sys.exit(main())