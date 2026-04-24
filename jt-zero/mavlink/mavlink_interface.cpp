/**
 * JT-Zero MAVLink Interface Implementation
 * 
 * Transport auto-detection:
 *   1. Try serial /dev/ttyAMA0 (Pi hardware UART) or /dev/serial0
 *   2. Try UDP 127.0.0.1:14550 (SITL / MissionPlanner / QGC)
 *   3. Fall back to simulated (in-memory)
 * 
 * MAVLink v2 message framing is used on real transports.
 */

#include "jt_zero/mavlink_interface.h"
#include <cstdio>
#include <cstring>
#include <cmath>

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#endif

namespace jtzero {

// Forward declaration — used by auto-baud detection and frame parser
static uint8_t get_crc_extra(uint32_t msg_id);

MAVLinkInterface::MAVLinkInterface() = default;

// ═══════════════════════════════════════════════════════════
// Transport Auto-Detection
// ═══════════════════════════════════════════════════════════

MAVTransport MAVLinkInterface::auto_detect_transport() {
#ifdef __linux__
    // Try serial ports common on Raspberry Pi
    const char* serial_devices[] = {"/dev/ttyAMA0", "/dev/serial0", "/dev/ttyS0"};
    for (auto dev : serial_devices) {
        struct stat st;
        if (stat(dev, &st) == 0 && S_ISCHR(st.st_mode)) {
            int fd = ::open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
            if (fd >= 0) {
                ::close(fd);
                std::snprintf(detected_serial_, sizeof(detected_serial_), "%s", dev);
                std::printf("[MAVLink] Serial port detected: %s\n", dev);
                return MAVTransport::SERIAL;
            }
        }
    }
    
    // Try UDP (check if a MAVLink endpoint is reachable)
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp >= 0) {
        ::close(udp);
        // We can always create a UDP socket, but only use it 
        // if explicitly configured (not auto-detected)
    }
#endif
    
    return MAVTransport::SIMULATED;
}

// ═══════════════════════════════════════════════════════════
// Initialization
// ═══════════════════════════════════════════════════════════

bool MAVLinkInterface::initialize(bool simulated) {
    simulated_ = simulated;
    
    if (simulated) {
        transport_ = MAVTransport::SIMULATED;
        state_ = MAVLinkState::CONNECTED;
        last_heartbeat_us_ = now_us();
        std::snprintf(transport_info_, sizeof(transport_info_), "simulated");
        std::printf("[MAVLink] Simulated connection established\n");
    } else {
        // Auto-detect real transport
        transport_ = auto_detect_transport();
        
        if (transport_ == MAVTransport::SERIAL) {
            // Use auto-detected device (not default /dev/ttyAMA0)
            const char* dev = detected_serial_[0] ? detected_serial_ : "/dev/ttyAMA0";
            // Auto-detect baud rate: try common rates, pick one with valid MAVLink bytes
            return initialize_serial_auto_baud(dev);
        } else if (transport_ == MAVTransport::UDP) {
            return initialize_udp();
        } else {
            // Fallback to simulation
            transport_ = MAVTransport::SIMULATED;
            state_ = MAVLinkState::CONNECTED;
            last_heartbeat_us_ = now_us();
            std::snprintf(transport_info_, sizeof(transport_info_), "simulated (no hw)");
            std::printf("[MAVLink] No transport detected — using simulation\n");
        }
    }
    
    return true;
}

bool MAVLinkInterface::initialize_serial_auto_baud(const char* device) {
#ifdef __linux__
    // Try baud rates in order: most common ArduPilot configs first
    const int baud_rates[] = {115200, 921600, 57600, 230400, 460800};
    const int num_rates = 5;
    
    std::printf("[MAVLink] Auto-detecting baud rate on %s (CRC-validated)...\n", device);
    std::fflush(stdout);
    
    for (int bi = 0; bi < num_rates; bi++) {
        int baud = baud_rates[bi];
        
        int fd = ::open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) {
            std::printf("[MAVLink] Cannot open %s: %s\n", device, strerror(errno));
            return false;
        }
        
        // Configure UART
        struct termios tty;
        if (tcgetattr(fd, &tty) != 0) {
            ::close(fd);
            continue;
        }
        
        speed_t spd;
        switch (baud) {
            case 57600:   spd = B57600;   break;
            case 115200:  spd = B115200;  break;
            case 230400:  spd = B230400;  break;
            case 460800:  spd = B460800;  break;
            case 921600:  spd = B921600;  break;
            default:      spd = B115200;  break;
        }
        cfsetospeed(&tty, spd);
        cfsetispeed(&tty, spd);
        
        // 8N1, no flow control, raw mode
        tty.c_cflag &= ~PARENB;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CSIZE;
        tty.c_cflag |= CS8;
        tty.c_cflag &= ~CRTSCTS;
        tty.c_cflag |= CREAD | CLOCAL;
        tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
        tty.c_iflag &= ~(IXON | IXOFF | IXANY | IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
        tty.c_oflag &= ~OPOST;
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 1;
        
        tcsetattr(fd, TCSANOW, &tty);
        tcflush(fd, TCIOFLUSH);
        
        // Read data for ~1.5 seconds into buffer
        uint8_t buf[4096];
        size_t total = 0;
        for (int attempt = 0; attempt < 30; attempt++) {  // 30 * 50ms = 1.5s
            usleep(50000);
            if (total < sizeof(buf)) {
                int n = ::read(fd, buf + total, sizeof(buf) - total);
                if (n > 0) total += static_cast<size_t>(n);
            }
        }
        
        ::close(fd);
        
        // Try to find valid MAVLink frames with CRC validation
        // Only count frames where we KNOW the CRC extra (prevents false positives)
        int valid_frames = 0;
        for (size_t pos = 0; pos + 8 <= total; pos++) {
            if (buf[pos] == 0xFD && pos + 12 <= total) {
                // MAVLink v2 candidate
                uint8_t plen = buf[pos + 1];
                size_t flen = 12 + plen;
                if (pos + flen <= total && plen <= 253) {
                    uint32_t mid = buf[pos + 7]
                                 | (static_cast<uint32_t>(buf[pos + 8]) << 8)
                                 | (static_cast<uint32_t>(buf[pos + 9]) << 16);
                    uint8_t crc_extra = get_crc_extra(mid);
                    if (crc_extra != 0) {  // Only validate known messages
                        // Compute CRC over header(9 bytes) + payload
                        uint16_t crc = 0xFFFF;
                        for (size_t i = 0; i < 9u + plen; i++) {
                            uint8_t tmp = buf[pos + 1 + i] ^ static_cast<uint8_t>(crc & 0xFF);
                            tmp ^= (tmp << 4);
                            crc = (crc >> 8) ^ (static_cast<uint16_t>(tmp) << 8)
                                  ^ (static_cast<uint16_t>(tmp) << 3) ^ (tmp >> 4);
                        }
                        // Accumulate CRC extra
                        {
                            uint8_t tmp = crc_extra ^ static_cast<uint8_t>(crc & 0xFF);
                            tmp ^= (tmp << 4);
                            crc = (crc >> 8) ^ (static_cast<uint16_t>(tmp) << 8)
                                  ^ (static_cast<uint16_t>(tmp) << 3) ^ (tmp >> 4);
                        }
                        uint16_t frame_crc = buf[pos + 10 + plen]
                                           | (static_cast<uint16_t>(buf[pos + 11 + plen]) << 8);
                        if (crc == frame_crc) {
                            valid_frames++;
                        }
                    }
                }
            } else if (buf[pos] == 0xFE && pos + 8 <= total) {
                // MAVLink v1 candidate
                uint8_t plen = buf[pos + 1];
                size_t flen = 8 + plen;
                if (pos + flen <= total && plen <= 253) {
                    uint32_t mid = buf[pos + 5];
                    uint8_t crc_extra = get_crc_extra(mid);
                    if (crc_extra != 0) {  // Only validate known messages
                        // Compute CRC over header(5 bytes) + payload
                        uint16_t crc = 0xFFFF;
                        for (size_t i = 0; i < 5u + plen; i++) {
                            uint8_t tmp = buf[pos + 1 + i] ^ static_cast<uint8_t>(crc & 0xFF);
                            tmp ^= (tmp << 4);
                            crc = (crc >> 8) ^ (static_cast<uint16_t>(tmp) << 8)
                                  ^ (static_cast<uint16_t>(tmp) << 3) ^ (tmp >> 4);
                        }
                        // Accumulate CRC extra
                        {
                            uint8_t tmp = crc_extra ^ static_cast<uint8_t>(crc & 0xFF);
                            tmp ^= (tmp << 4);
                            crc = (crc >> 8) ^ (static_cast<uint16_t>(tmp) << 8)
                                  ^ (static_cast<uint16_t>(tmp) << 3) ^ (tmp >> 4);
                        }
                        uint16_t frame_crc = buf[pos + 6 + plen]
                                           | (static_cast<uint16_t>(buf[pos + 7 + plen]) << 8);
                        if (crc == frame_crc) {
                            valid_frames++;
                        }
                    }
                }
            }
        }
        
        std::printf("[MAVLink] Baud %d: %zu bytes, %d CRC-valid frames\n", baud, total, valid_frames);
        std::fflush(stdout);
        
        if (valid_frames >= 1) {
            std::printf("[MAVLink] === Auto-detected baud: %d ===\n", baud);
            std::fflush(stdout);
            return initialize_serial(device, baud);
        }
    }
    
    // No baud rate matched — fallback to 115200 (ArduPilot default)
    std::printf("[MAVLink] No CRC-valid frames found — defaulting to 115200\n");
    std::fflush(stdout);
    return initialize_serial(device, 115200);
#else
    return false;
#endif
}

bool MAVLinkInterface::initialize_serial(const char* device, int baudrate) {
#ifdef __linux__
    serial_fd_ = ::open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (serial_fd_ < 0) {
        std::printf("[MAVLink] Failed to open %s: %s\n", device, strerror(errno));
        return false;
    }
    
    // Configure UART
    struct termios tty;
    if (tcgetattr(serial_fd_, &tty) != 0) {
        ::close(serial_fd_);
        serial_fd_ = -1;
        return false;
    }
    
    // Map baudrate
    speed_t baud;
    switch (baudrate) {
        case 57600:   baud = B57600;   break;
        case 115200:  baud = B115200;  break;
        case 230400:  baud = B230400;  break;
        case 460800:  baud = B460800;  break;
        case 921600:  baud = B921600;  break;
        default:      baud = B921600;  break;
    }
    
    cfsetospeed(&tty, baud);
    cfsetispeed(&tty, baud);
    
    // 8N1, no flow control
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;
    
    // Raw mode
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_oflag &= ~OPOST;
    
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1; // 100ms read timeout
    
    if (tcsetattr(serial_fd_, TCSANOW, &tty) != 0) {
        ::close(serial_fd_);
        serial_fd_ = -1;
        return false;
    }
    
    tcflush(serial_fd_, TCIOFLUSH);
    
    transport_ = MAVTransport::SERIAL;
    serial_baud_ = baudrate;
    state_ = MAVLinkState::CONNECTING;
    std::snprintf(transport_info_, sizeof(transport_info_), "%s@%d", device, baudrate);
    std::printf("[MAVLink] Serial opened: %s @ %d baud\n", device, baudrate);
    return true;
#else
    return false;
#endif
}

bool MAVLinkInterface::initialize_udp(const char* host, int port) {
#ifdef __linux__
    udp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd_ < 0) return false;
    
    // Set non-blocking
    int flags = fcntl(udp_fd_, F_GETFL, 0);
    fcntl(udp_fd_, F_SETFL, flags | O_NONBLOCK);
    
    // Bind to receive
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(udp_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        // Port in use — try connecting instead
        addr.sin_addr.s_addr = inet_addr(host);
        addr.sin_port = htons(port);
        if (connect(udp_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(udp_fd_);
            udp_fd_ = -1;
            return false;
        }
    }
    
    std::strncpy(udp_host_, host, sizeof(udp_host_) - 1);
    udp_port_ = port;
    transport_ = MAVTransport::UDP;
    state_ = MAVLinkState::CONNECTING;
    std::snprintf(transport_info_, sizeof(transport_info_), "%s:%d", host, port);
    std::printf("[MAVLink] UDP opened: %s:%d\n", host, port);
    return true;
#else
    return false;
#endif
}

// ═══════════════════════════════════════════════════════════
// Raw Transport I/O
// ═══════════════════════════════════════════════════════════

bool MAVLinkInterface::send_raw(const uint8_t* data, size_t len) {
#ifdef __linux__
    if (transport_ == MAVTransport::SERIAL && serial_fd_ >= 0) {
        ssize_t n = ::write(serial_fd_, data, len);
        if (n > 0) bytes_sent_ += static_cast<size_t>(n);
        return n == static_cast<ssize_t>(len);
    }
    if (transport_ == MAVTransport::UDP && udp_fd_ >= 0) {
        ssize_t n = ::send(udp_fd_, data, len, MSG_DONTWAIT);
        if (n > 0) bytes_sent_ += static_cast<size_t>(n);
        return n == static_cast<ssize_t>(len);
    }
#endif
    return false;
}

int MAVLinkInterface::recv_raw(uint8_t* buf, size_t max_len) {
#ifdef __linux__
    if (transport_ == MAVTransport::SERIAL && serial_fd_ >= 0) {
        return static_cast<int>(::read(serial_fd_, buf, max_len));
    }
    if (transport_ == MAVTransport::UDP && udp_fd_ >= 0) {
        return static_cast<int>(::recv(udp_fd_, buf, max_len, MSG_DONTWAIT));
    }
#endif
    return -1;
}

// ═══════════════════════════════════════════════════════════
// Shutdown
// ═══════════════════════════════════════════════════════════

void MAVLinkInterface::shutdown() {
#ifdef __linux__
    if (serial_fd_ >= 0) { ::close(serial_fd_); serial_fd_ = -1; }
    if (udp_fd_ >= 0)    { ::close(udp_fd_);    udp_fd_ = -1; }
#endif
    state_ = MAVLinkState::DISCONNECTED;
    std::printf("[MAVLink] Disconnected\n");
}

// ═══════════════════════════════════════════════════════════
// Message Sending
// ═══════════════════════════════════════════════════════════

bool MAVLinkInterface::send_vision_position(const MAVVisionPositionEstimate& msg) {
    if (state_ != MAVLinkState::CONNECTED) return false;
    
    if (!simulated_ && (serial_fd_ >= 0 || udp_fd_ >= 0)) {
        // VISION_POSITION_ESTIMATE (msg_id=102, CRC_EXTRA=158)
        // Wire order: usec(8), x(4), y(4), z(4), roll(4), pitch(4), yaw(4) = 32 bytes
        uint8_t payload[32];
        std::memcpy(payload + 0,  &msg.usec,  8);
        std::memcpy(payload + 8,  &msg.x,     4);
        std::memcpy(payload + 12, &msg.y,     4);
        std::memcpy(payload + 16, &msg.z,     4);
        std::memcpy(payload + 20, &msg.roll,  4);
        std::memcpy(payload + 24, &msg.pitch, 4);
        std::memcpy(payload + 28, &msg.yaw,   4);
        send_mavlink_v2(102, payload, 32, 158);
    }
    
    msgs_sent_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool MAVLinkInterface::send_odometry(const MAVOdometry& msg) {
    if (state_ != MAVLinkState::CONNECTED) return false;
    
    if (!simulated_ && (serial_fd_ >= 0 || udp_fd_ >= 0)) {
        // ODOMETRY (msg_id=331, CRC_EXTRA=91)
        // Wire order: time_usec(8), x(4),y(4),z(4), q[4](16), vx(4),vy(4),vz(4),
        //   rollspeed(4),pitchspeed(4),yawspeed(4), pose_cov[21](84), vel_cov[21](84),
        //   frame_id(1), child_frame_id(1) = 230 bytes base
        //   + extensions: reset_counter(1), estimator_type(1), quality(1) = 233 total
        uint8_t payload[233];
        std::memset(payload, 0, sizeof(payload));
        
        std::memcpy(payload + 0, &msg.time_usec, 8);
        std::memcpy(payload + 8, &msg.x, 4);
        std::memcpy(payload + 12, &msg.y, 4);
        std::memcpy(payload + 16, &msg.z, 4);
        std::memcpy(payload + 20, msg.q, 16);   // float[4]
        std::memcpy(payload + 36, &msg.vx, 4);
        std::memcpy(payload + 40, &msg.vy, 4);
        std::memcpy(payload + 44, &msg.vz, 4);
        std::memcpy(payload + 48, &msg.rollspeed, 4);
        std::memcpy(payload + 52, &msg.pitchspeed, 4);
        std::memcpy(payload + 56, &msg.yawspeed, 4);
        
        // pose_covariance[21] at offset 60 (upper triangle of 6x6 matrix)
        // Use VO confidence to set covariance: high confidence → low covariance
        float pos_var = 0.01f; // 10cm base uncertainty
        if (msg.quality < 80) {
            pos_var = 0.1f + (80.0f - msg.quality) * 0.05f; // up to 4.1m uncertainty
        }
        if (msg.quality < 40) {
            pos_var = 10.0f; // low confidence → 10m uncertainty (EKF ignores, prevents cycling)
        }
        float rot_var = 0.01f; // rotation variance (fixed, yaw from gyro)
        // Diagonal elements: xx, yy, zz, roll, pitch, yaw
        // Index mapping for upper triangle: [0]=xx, [6]=yy, [11]=zz, [15]=rr, [18]=pp, [20]=yy_rot
        float zero_f = 0.0f;
        for (int i = 0; i < 21; i++) {
            std::memcpy(payload + 60 + i * 4, &zero_f, 4);
        }
        std::memcpy(payload + 60 + 0 * 4, &pos_var, 4);  // xx
        std::memcpy(payload + 60 + 6 * 4, &pos_var, 4);  // yy
        std::memcpy(payload + 60 + 11 * 4, &pos_var, 4); // zz
        std::memcpy(payload + 60 + 15 * 4, &rot_var, 4); // roll
        std::memcpy(payload + 60 + 18 * 4, &rot_var, 4); // pitch
        std::memcpy(payload + 60 + 20 * 4, &rot_var, 4); // yaw
        
        // velocity_covariance[21] at offset 144
        float vel_var = pos_var * 2.0f; // velocity variance scales with position
        for (int i = 0; i < 21; i++) {
            std::memcpy(payload + 144 + i * 4, &zero_f, 4);
        }
        std::memcpy(payload + 144 + 0 * 4, &vel_var, 4);  // vx
        std::memcpy(payload + 144 + 6 * 4, &vel_var, 4);  // vy
        std::memcpy(payload + 144 + 11 * 4, &vel_var, 4); // vz
        
        payload[228] = msg.frame_id;
        payload[229] = msg.child_frame_id;
        
        // Extensions
        payload[230] = 0;  // reset_counter
        payload[231] = 2;  // estimator_type = MAV_ESTIMATOR_TYPE_VIO
        payload[232] = static_cast<uint8_t>(std::max(0.0f, std::min(100.0f, msg.quality)));
        
        send_mavlink_v2(331, payload, 233, 91);
    }
    
    msgs_sent_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool MAVLinkInterface::send_optical_flow_rad(const MAVOpticalFlowRad& msg) {
    if (state_ != MAVLinkState::CONNECTED) return false;
    
    if (!simulated_ && (serial_fd_ >= 0 || udp_fd_ >= 0)) {
        // OPTICAL_FLOW_RAD (msg_id=106, CRC_EXTRA=138)
        // Wire order (sorted by size): time_usec(8), integration_time_us(4),
        //   integrated_x(4), integrated_y(4), integrated_xgyro(4), integrated_ygyro(4),
        //   integrated_zgyro(4), time_delta_distance_us(4), distance(4),
        //   temperature(2), sensor_id(1), quality(1) = 44 bytes
        uint8_t payload[44];
        std::memset(payload, 0, sizeof(payload));
        
        std::memcpy(payload + 0,  &msg.time_usec, 8);
        std::memcpy(payload + 8,  &msg.integration_time_us, 4);
        std::memcpy(payload + 12, &msg.integrated_x, 4);
        std::memcpy(payload + 16, &msg.integrated_y, 4);
        std::memcpy(payload + 20, &msg.integrated_xgyro, 4);
        std::memcpy(payload + 24, &msg.integrated_ygyro, 4);
        std::memcpy(payload + 28, &msg.integrated_zgyro, 4);
        std::memcpy(payload + 32, &msg.time_delta_distance_us, 4);
        std::memcpy(payload + 36, &msg.distance, 4);
        int16_t temp = msg.temperature;
        std::memcpy(payload + 40, &temp, 2);
        payload[42] = 0;  // sensor_id
        payload[43] = msg.quality;
        
        send_mavlink_v2(106, payload, 44, 175);
    }
    
    msgs_sent_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool MAVLinkInterface::send_heartbeat() {
    if (state_ != MAVLinkState::CONNECTED && state_ != MAVLinkState::CONNECTING) return false;
    
    // Build and send our heartbeat to FC
    if (!simulated_ && (serial_fd_ >= 0 || udp_fd_ >= 0)) {
        // MAVLink v2 HEARTBEAT (msg_id=0, CRC_EXTRA=50)
        uint8_t payload[9] = {0};
        // custom_mode = 0
        payload[4] = 18;  // MAV_TYPE_ONBOARD_CONTROLLER
        payload[5] = 0;   // MAV_AUTOPILOT_GENERIC
        payload[6] = 0;   // base_mode
        payload[7] = 0;   // system_status = uninit
        payload[8] = 3;   // mavlink_version = 2
        send_mavlink_v2(0, payload, 9, 50);
    }
    
    last_heartbeat_us_ = now_us();
    msgs_sent_.fetch_add(1, std::memory_order_relaxed);
    
    if (simulated_) {
        msgs_received_.fetch_add(1, std::memory_order_relaxed);
        fc_armed_ = false;
    } else {
        process_incoming();
    }
    
    return true;
}

bool MAVLinkInterface::send_statustext(uint8_t severity, const char* text) {
    if (state_ != MAVLinkState::CONNECTED && state_ != MAVLinkState::CONNECTING) return false;
    if (simulated_) return true;  // no-op in simulation
    
    // STATUSTEXT (msg_id=253, CRC_EXTRA=83)
    // Payload: severity(1) + text(50) + id(2) + chunk_seq(1) = 54 bytes
    uint8_t payload[54] = {0};
    payload[0] = severity;  // MAV_SEVERITY (0=EMERGENCY, 4=WARNING, 6=INFO)
    size_t len = 0;
    while (text[len] && len < 50) {
        payload[1 + len] = static_cast<uint8_t>(text[len]);
        ++len;
    }
    // id=0, chunk_seq=0 (single message)
    send_mavlink_v2(253, payload, 54, 83);
    msgs_sent_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ═══════════════════════════════════════════════════════════
// MAVLink v2 Frame Serializer
// ═══════════════════════════════════════════════════════════

// CRC-16/MCRF4XX (X.25)
static uint16_t mavlink_crc(const uint8_t* buf, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        uint8_t tmp = buf[i] ^ static_cast<uint8_t>(crc & 0xFF);
        tmp ^= (tmp << 4);
        crc = (crc >> 8) ^ (static_cast<uint16_t>(tmp) << 8) 
              ^ (static_cast<uint16_t>(tmp) << 3) ^ (tmp >> 4);
    }
    return crc;
}

static void mavlink_crc_accumulate(uint16_t& crc, uint8_t byte) {
    uint8_t tmp = byte ^ static_cast<uint8_t>(crc & 0xFF);
    tmp ^= (tmp << 4);
    crc = (crc >> 8) ^ (static_cast<uint16_t>(tmp) << 8)
          ^ (static_cast<uint16_t>(tmp) << 3) ^ (tmp >> 4);
}

bool MAVLinkInterface::send_mavlink_v2(uint32_t msg_id, const uint8_t* payload, uint8_t len, uint8_t crc_extra) {
    uint8_t frame[280];
    frame[0] = 0xFD;           // MAVLink v2 STX
    frame[1] = len;            // payload length
    frame[2] = 0;              // incompat_flags
    frame[3] = 0;              // compat_flags
    frame[4] = seq_++;         // sequence
    frame[5] = 1;              // system_id (us)
    frame[6] = 191;            // component_id (MAV_COMP_ID_ONBOARD_COMPUTER)
    frame[7] = msg_id & 0xFF;
    frame[8] = (msg_id >> 8) & 0xFF;
    frame[9] = (msg_id >> 16) & 0xFF;
    
    std::memcpy(frame + 10, payload, len);
    
    // CRC over bytes 1..end_of_payload, then accumulate crc_extra
    uint16_t crc = mavlink_crc(frame + 1, 9 + len);
    mavlink_crc_accumulate(crc, crc_extra);
    
    frame[10 + len] = crc & 0xFF;
    frame[11 + len] = (crc >> 8) & 0xFF;
    
    return send_raw(frame, 12 + len);
}

void MAVLinkInterface::request_data_streams() {
    if (simulated_) return;
    
    // Method 1: Legacy REQUEST_DATA_STREAM (msg_id=66, CRC_EXTRA=148)
    struct { uint8_t stream_id; uint16_t rate_hz; } streams[] = {
        { 1,  2 },  // RAW_SENSORS (SCALED_IMU, SCALED_PRESSURE)
        { 2,  2 },  // EXTENDED_STATUS (SYS_STATUS)
        { 6,  2 },  // POSITION (GLOBAL_POSITION_INT, GPS_RAW_INT)
        { 10, 25 },  // EXTRA1 (ATTITUDE) — 25 Hz for LK IMU hints (Fix 45b)
        { 11, 2 },  // EXTRA2 (VFR_HUD)
    };
    
    for (auto& s : streams) {
        uint8_t payload[6];
        payload[0] = s.rate_hz & 0xFF;
        payload[1] = (s.rate_hz >> 8) & 0xFF;
        payload[2] = fc_system_id_;
        payload[3] = 1;  // MAV_COMP_ID_AUTOPILOT1
        payload[4] = s.stream_id;
        payload[5] = 1;  // start
        send_mavlink_v2(66, payload, 6, 148);
        msgs_sent_.fetch_add(1, std::memory_order_relaxed);
    }
    
    // Method 2: Modern COMMAND_LONG / MAV_CMD_SET_MESSAGE_INTERVAL (cmd=511)
    // msg_id=76, CRC_EXTRA=152
    // More reliable: requests specific message IDs at specific intervals
    struct { uint32_t mavlink_msg_id; int32_t interval_us; } intervals[] = {
        { 30,   40000 },  // ATTITUDE @ 25 Hz (Fix 45b: gyro hints for LK tracker)
        { 29,  500000 },  // SCALED_PRESSURE @ 2 Hz
        { 27,  500000 },  // RAW_IMU / SCALED_IMU @ 2 Hz
        { 24,  500000 },  // GPS_RAW_INT @ 2 Hz
        { 33,  500000 },  // GLOBAL_POSITION_INT @ 2 Hz
        { 74,  500000 },  // VFR_HUD @ 2 Hz
        { 1,   500000 },  // SYS_STATUS @ 2 Hz
    };
    
    for (auto& m : intervals) {
        uint8_t payload[33] = {};
        // COMMAND_LONG fields (wire order: largest first)
        // param1 (float): MAVLink message ID
        float p1;
        std::memcpy(&p1, &(m.mavlink_msg_id), 0);  // cast to float
        p1 = static_cast<float>(m.mavlink_msg_id);
        std::memcpy(payload + 0, &p1, 4);
        // param2 (float): interval in microseconds (-1 = default, 0 = disable)
        float p2 = static_cast<float>(m.interval_us);
        std::memcpy(payload + 4, &p2, 4);
        // param3-param7: unused (0)
        // command (uint16_t) at offset 28
        uint16_t cmd = 511;  // MAV_CMD_SET_MESSAGE_INTERVAL
        std::memcpy(payload + 28, &cmd, 2);
        // target_system
        payload[30] = fc_system_id_;
        // target_component
        payload[31] = 1;  // MAV_COMP_ID_AUTOPILOT1
        // confirmation
        payload[32] = 0;
        
        send_mavlink_v2(76, payload, 33, 152);
        msgs_sent_.fetch_add(1, std::memory_order_relaxed);
    }
    
    std::printf("[MAVLink] Requested data streams + message intervals from FC sysid=%d\n", fc_system_id_);
    std::fflush(stdout);
}

// ═══════════════════════════════════════════════════════════
// Diagnostic: message ID tracking
// ═══════════════════════════════════════════════════════════

void MAVLinkInterface::log_msg_id(uint32_t msg_id) {
    // Log first 20 unique message IDs for debugging
    if (diag_unique_count_ >= 32) return;
    for (size_t i = 0; i < diag_unique_count_; i++) {
        if (diag_msg_ids_[i] == msg_id) return;  // already logged
    }
    diag_msg_ids_[diag_unique_count_++] = msg_id;
    std::printf("[MAVLink] New msg_id=%u (total unique: %zu)\n", msg_id, diag_unique_count_);
    std::fflush(stdout);
}

// ═══════════════════════════════════════════════════════════
// MAVLink Frame Parser
// ═══════════════════════════════════════════════════════════

// CRC Extra bytes for known MAVLink messages (for CRC validation)
static uint8_t get_crc_extra(uint32_t msg_id) {
    switch (msg_id) {
        case 0:   return 50;   // HEARTBEAT
        case 1:   return 124;  // SYS_STATUS
        case 2:   return 137;  // SYSTEM_TIME
        case 24:  return 24;   // GPS_RAW_INT
        case 26:  return 170;  // SCALED_IMU
        case 27:  return 144;  // RAW_IMU
        case 29:  return 115;  // SCALED_PRESSURE
        case 30:  return 39;   // ATTITUDE
        case 33:  return 104;  // GLOBAL_POSITION_INT
        case 65:  return 118;  // RC_CHANNELS
        case 74:  return 20;   // VFR_HUD
        case 76:  return 152;  // COMMAND_LONG
        case 77:  return 143;  // COMMAND_ACK
        case 102: return 158;  // VISION_POSITION_ESTIMATE
        case 106: return 175;  // OPTICAL_FLOW_RAD
        case 253: return 83;   // STATUSTEXT
        case 331: return 91;   // ODOMETRY
        default:  return 0;    // Unknown — skip CRC validation
    }
}

// Validate MAVLink v2 frame CRC
static bool validate_v2_crc(const uint8_t* frame, uint8_t payload_len, uint32_t msg_id) {
    uint8_t crc_extra = get_crc_extra(msg_id);
    if (crc_extra == 0) return true; // Unknown msg — accept without CRC check
    
    // CRC over bytes 1 .. 9+payload_len (header without STX + payload)
    uint16_t crc = 0xFFFF;
    size_t crc_len = 9 + payload_len;
    for (size_t i = 0; i < crc_len; i++) {
        uint8_t tmp = frame[1 + i] ^ static_cast<uint8_t>(crc & 0xFF);
        tmp ^= (tmp << 4);
        crc = (crc >> 8) ^ (static_cast<uint16_t>(tmp) << 8)
              ^ (static_cast<uint16_t>(tmp) << 3) ^ (tmp >> 4);
    }
    // Accumulate CRC extra
    {
        uint8_t tmp = crc_extra ^ static_cast<uint8_t>(crc & 0xFF);
        tmp ^= (tmp << 4);
        crc = (crc >> 8) ^ (static_cast<uint16_t>(tmp) << 8)
              ^ (static_cast<uint16_t>(tmp) << 3) ^ (tmp >> 4);
    }
    
    uint16_t frame_crc = frame[10 + payload_len]
                       | (static_cast<uint16_t>(frame[11 + payload_len]) << 8);
    return crc == frame_crc;
}

// Validate MAVLink v1 frame CRC
static bool validate_v1_crc(const uint8_t* frame, uint8_t payload_len, uint32_t msg_id) {
    uint8_t crc_extra = get_crc_extra(msg_id);
    if (crc_extra == 0) return true; // Unknown msg — accept without CRC check
    
    // CRC over bytes 1 .. 5+payload_len (header without STX + payload)
    uint16_t crc = 0xFFFF;
    size_t crc_len = 5 + payload_len;
    for (size_t i = 0; i < crc_len; i++) {
        uint8_t tmp = frame[1 + i] ^ static_cast<uint8_t>(crc & 0xFF);
        tmp ^= (tmp << 4);
        crc = (crc >> 8) ^ (static_cast<uint16_t>(tmp) << 8)
              ^ (static_cast<uint16_t>(tmp) << 3) ^ (tmp >> 4);
    }
    // Accumulate CRC extra
    {
        uint8_t tmp = crc_extra ^ static_cast<uint8_t>(crc & 0xFF);
        tmp ^= (tmp << 4);
        crc = (crc >> 8) ^ (static_cast<uint16_t>(tmp) << 8)
              ^ (static_cast<uint16_t>(tmp) << 3) ^ (tmp >> 4);
    }
    
    uint16_t frame_crc = frame[6 + payload_len]
                       | (static_cast<uint16_t>(frame[7 + payload_len]) << 8);
    return crc == frame_crc;
}

void MAVLinkInterface::process_incoming() {
    // Read all available bytes into ring buffer
    while (true) {
        size_t space = RX_BUF_SIZE - rx_head_;
        if (space == 0) {
            // Buffer full — discard oldest data
            rx_tail_ = RX_BUF_SIZE / 2;
            std::memmove(rx_buf_, rx_buf_ + rx_tail_, rx_head_ - rx_tail_);
            rx_head_ -= rx_tail_;
            rx_tail_ = 0;
            space = RX_BUF_SIZE - rx_head_;
        }
        int n = recv_raw(rx_buf_ + rx_head_, space);
        if (n <= 0) break;
        bytes_received_ += static_cast<size_t>(n);
        rx_head_ += static_cast<size_t>(n);
        
        // Diagnostic: log first 32 raw bytes once
        if (!diag_raw_logged_ && bytes_received_ >= 16) {
            std::printf("[MAVLink] First %zu raw RX bytes: ", rx_head_);
            for (size_t i = 0; i < rx_head_ && i < 32; i++) {
                std::printf("%02X ", rx_buf_[i]);
            }
            std::printf("\n");
            std::fflush(stdout);
            diag_raw_logged_ = true;
        }
    }
    
    // Parse frames from buffer
    while (rx_tail_ + 8 <= rx_head_) {  // Minimum v1 frame: STX(1)+len(1)+seq+sys+comp+msg+crc(2) = 8
        // Find start-of-frame
        uint8_t stx = rx_buf_[rx_tail_];
        
        if (stx == 0xFD) {
            // MAVLink v2 frame
            uint8_t payload_len = rx_buf_[rx_tail_ + 1];
            size_t frame_len = 12 + payload_len;  // STX+len+incompat+compat+seq+sysid+compid+msgid(3)+payload+crc(2)
            
            // Check for signing flag — adds 13 bytes signature
            uint8_t incompat_flags = rx_buf_[rx_tail_ + 2];
            if (incompat_flags & 0x01) {
                frame_len += 13;
            }
            
            if (rx_tail_ + frame_len > rx_head_) break;  // Incomplete frame
            
            uint8_t sysid = rx_buf_[rx_tail_ + 5];
            uint32_t msg_id = rx_buf_[rx_tail_ + 7]
                           | (static_cast<uint32_t>(rx_buf_[rx_tail_ + 8]) << 8)
                           | (static_cast<uint32_t>(rx_buf_[rx_tail_ + 9]) << 16);
            const uint8_t* payload = rx_buf_ + rx_tail_ + 10;
            
            // CRC validation — reject corrupt frames
            if (!validate_v2_crc(rx_buf_ + rx_tail_, payload_len, msg_id)) {
                crc_errors_++;
                if (crc_errors_ <= 5) {
                    std::printf("[MAVLink] v2 CRC FAIL: msg_id=%u len=%u sysid=%u (err #%zu)\n",
                                msg_id, payload_len, sysid, crc_errors_);
                    std::fflush(stdout);
                }
                rx_tail_++;  // Skip this byte and resync
                continue;
            }
            
            handle_message(msg_id, payload, payload_len, sysid);
            msgs_received_.fetch_add(1, std::memory_order_relaxed);
            
            if (state_ == MAVLinkState::CONNECTING) {
                state_ = MAVLinkState::CONNECTED;
                std::printf("[MAVLink] Connected to FC via v2 (sysid=%d, first msg=%u)\n", sysid, msg_id);
                std::fflush(stdout);
            }
            last_heartbeat_us_ = now_us();
            
            rx_tail_ += frame_len;
            
        } else if (stx == 0xFE) {
            // MAVLink v1 frame
            uint8_t payload_len = rx_buf_[rx_tail_ + 1];
            size_t frame_len = 8 + payload_len;  // STX+len+seq+sysid+compid+msgid+payload+crc(2)
            
            if (rx_tail_ + frame_len > rx_head_) break;
            
            uint8_t sysid = rx_buf_[rx_tail_ + 3];
            uint32_t msg_id = rx_buf_[rx_tail_ + 5];
            const uint8_t* payload = rx_buf_ + rx_tail_ + 6;
            
            // CRC validation — reject corrupt frames
            if (!validate_v1_crc(rx_buf_ + rx_tail_, payload_len, msg_id)) {
                crc_errors_++;
                if (crc_errors_ <= 5) {
                    std::printf("[MAVLink] v1 CRC FAIL: msg_id=%u len=%u sysid=%u (err #%zu)\n",
                                msg_id, payload_len, sysid, crc_errors_);
                    std::fflush(stdout);
                }
                rx_tail_++;  // Skip this byte and resync
                continue;
            }
            
            handle_message(msg_id, payload, payload_len, sysid);
            msgs_received_.fetch_add(1, std::memory_order_relaxed);
            
            if (state_ == MAVLinkState::CONNECTING) {
                state_ = MAVLinkState::CONNECTED;
                std::printf("[MAVLink] Connected via v1 (sysid=%d, msg=%u)\n", sysid, msg_id);
                std::fflush(stdout);
            }
            last_heartbeat_us_ = now_us();
            
            rx_tail_ += frame_len;
            
        } else {
            // Not a valid start byte — skip
            rx_tail_++;
        }
    }
    
    // Compact buffer if we've consumed a lot
    if (rx_tail_ > RX_BUF_SIZE / 2) {
        size_t remaining = rx_head_ - rx_tail_;
        if (remaining > 0) {
            std::memmove(rx_buf_, rx_buf_ + rx_tail_, remaining);
        }
        rx_head_ = remaining;
        rx_tail_ = 0;
    }
}

// Helper: read little-endian values from buffer
static inline float    read_f32(const uint8_t* p) { float v; std::memcpy(&v, p, 4); return v; }
static inline uint32_t read_u32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
static inline int32_t  read_i32(const uint8_t* p) { int32_t v; std::memcpy(&v, p, 4); return v; }
static inline uint16_t read_u16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
static inline int16_t  read_i16(const uint8_t* p) { int16_t v; std::memcpy(&v, p, 2); return v; }
static inline uint64_t read_u64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

void MAVLinkInterface::handle_message(uint32_t msg_id, const uint8_t* p, uint8_t len, uint8_t sysid) {
    // RAII spinlock — automatically released when function exits (any path)
    ScopedSpinLock guard(telem_lock_);
    
    fc_system_id_ = sysid;
    fc_telem_.last_update_us = now_us();
    fc_telem_.msg_count++;
    log_msg_id(msg_id);
    
    // Helper: safe read — returns 0 for bytes beyond payload (MAVLink v2 trims trailing zeros)
    auto safe_f32 = [&](size_t off) -> float { 
        if (off + 4 > len) return 0.0f;
        float v; std::memcpy(&v, p + off, 4); return v;
    };
    auto safe_u32 = [&](size_t off) -> uint32_t {
        if (off + 4 > len) return 0;
        uint32_t v; std::memcpy(&v, p + off, 4); return v;
    };
    auto safe_i32 = [&](size_t off) -> int32_t {
        if (off + 4 > len) return 0;
        int32_t v; std::memcpy(&v, p + off, 4); return v;
    };
    auto safe_u16 = [&](size_t off) -> uint16_t {
        if (off + 2 > len) return 0;
        uint16_t v; std::memcpy(&v, p + off, 2); return v;
    };
    auto safe_i16 = [&](size_t off) -> int16_t {
        if (off + 2 > len) return 0;
        int16_t v; std::memcpy(&v, p + off, 2); return v;
    };
    auto safe_u8 = [&](size_t off) -> uint8_t {
        if (off >= len) return 0;
        return p[off];
    };
    
    switch (msg_id) {
        
    case 0: {  // HEARTBEAT
        // MAVLink v2 zero truncation: payload may be < 9 bytes if trailing fields are 0
        // We need at least 5 bytes to read type (offset 4)
        if (len < 5) {
            std::printf("[MAVLink] HEARTBEAT too short: len=%u (need >=5), sysid=%u\n", len, sysid);
            std::fflush(stdout);
            break;
        }
        uint8_t hb_type      = safe_u8(4);
        uint8_t hb_autopilot = safe_u8(5);
        uint8_t hb_base_mode = safe_u8(6);
        
        // Log every heartbeat for debugging (first 10 only)
        if (heartbeats_seen_ < 10) {
            std::printf("[MAVLink] HEARTBEAT rx: sysid=%u type=%u autopilot=%u base_mode=0x%02X len=%u\n",
                        sysid, hb_type, hb_autopilot, hb_base_mode, len);
            std::fflush(stdout);
        }
        heartbeats_seen_++;
        
        // Skip our own echoed heartbeats (sysid=1, compid=191, type=18 ONBOARD_CONTROLLER)
        if (sysid == 1 && hb_type == 18) break;
        // Skip GCS heartbeats (MAV_TYPE_GCS=6)
        if (hb_type == 6) break;
        // Skip ADSB (type=27)
        if (hb_type == 27) break;
        
        // Accept ALL other heartbeats including GENERIC (type=0) and vehicle types
        fc_telem_.custom_mode  = safe_u32(0);
        fc_telem_.fc_type      = hb_type;
        fc_telem_.fc_autopilot = hb_autopilot;
        fc_telem_.base_mode    = hb_base_mode;
        fc_telem_.system_status = safe_u8(7);
        fc_telem_.armed        = (hb_base_mode & 0x80) != 0;
        fc_telem_.heartbeat_valid = true;
        fc_armed_ = fc_telem_.armed;
        heartbeats_received_.fetch_add(1, std::memory_order_relaxed);
        
        if (heartbeats_received_.load() == 1) {
            std::printf("[MAVLink] First FC heartbeat! type=%u(%s) autopilot=%u armed=%d\n",
                        hb_type,
                        hb_type == 2 ? "QUADROTOR" : hb_type == 1 ? "FIXED_WING" : "OTHER",
                        hb_autopilot, fc_telem_.armed ? 1 : 0);
            std::fflush(stdout);
        }
        break;
    }
    
    case 1: {  // SYS_STATUS — need at least bytes 14-17 for voltage/current
        if (len < 18) break;
        fc_telem_.battery_voltage   = safe_u16(14) * 0.001f;
        fc_telem_.battery_current   = safe_i16(16) * 0.01f;
        fc_telem_.battery_remaining = static_cast<int8_t>(safe_u8(30));
        fc_telem_.status_valid = true;
        break;
    }
    
    case 24: {  // GPS_RAW_INT — need at least offset 16 for lat/lon/alt
        if (len < 18) break;
        fc_telem_.gps_lat   = safe_i32(8) * 1.0e-7;
        fc_telem_.gps_lon   = safe_i32(12) * 1.0e-7;
        fc_telem_.gps_alt   = safe_i32(16) * 0.001f;
        fc_telem_.gps_speed = safe_u16(24) * 0.01f;
        fc_telem_.gps_fix   = safe_u8(28);
        fc_telem_.gps_sats  = safe_u8(29);
        fc_telem_.gps_valid = (fc_telem_.gps_fix >= 2);
        break;
    }
    
    case 26: {  // SCALED_IMU — need at least offset 4 for acc data
        if (len < 10) break;
        fc_telem_.acc_x  = safe_i16(4) * 0.00981f;
        fc_telem_.acc_y  = safe_i16(6) * 0.00981f;
        fc_telem_.acc_z  = safe_i16(8) * 0.00981f;
        fc_telem_.gyro_x = safe_i16(10) * 0.001f;
        fc_telem_.gyro_y = safe_i16(12) * 0.001f;
        fc_telem_.gyro_z = safe_i16(14) * 0.001f;
        fc_telem_.mag_x  = safe_i16(16) * 0.001f;
        fc_telem_.mag_y  = safe_i16(18) * 0.001f;
        fc_telem_.mag_z  = safe_i16(20) * 0.001f;
        fc_telem_.imu_valid = true;
        break;
    }
    
    case 27: {  // RAW_IMU — same as SCALED_IMU but uint64_t time (offset +4)
        if (len < 14) break;
        // uint64_t time_usec at p+0 (8 bytes, vs 4 in SCALED_IMU)
        fc_telem_.acc_x  = safe_i16(8) * 0.00981f;    // mG → m/s²
        fc_telem_.acc_y  = safe_i16(10) * 0.00981f;
        fc_telem_.acc_z  = safe_i16(12) * 0.00981f;
        fc_telem_.gyro_x = safe_i16(14) * 0.001f;     // mrad/s → rad/s
        fc_telem_.gyro_y = safe_i16(16) * 0.001f;
        fc_telem_.gyro_z = safe_i16(18) * 0.001f;
        fc_telem_.mag_x  = safe_i16(20) * 0.001f;     // mgauss → gauss
        fc_telem_.mag_y  = safe_i16(22) * 0.001f;
        fc_telem_.mag_z  = safe_i16(24) * 0.001f;
        fc_telem_.imu_valid = true;
        break;
    }
    
    case 29: {  // SCALED_PRESSURE — need press_abs at offset 4
        if (len < 8) break;
        fc_telem_.pressure    = safe_f32(4);
        fc_telem_.temperature = safe_i16(12) * 0.01f;
        fc_telem_.baro_valid = true;
        break;
    }
    
    case 30: {  // ATTITUDE — need at least roll/pitch/yaw at offset 4-16
        if (len < 16) break;
        fc_telem_.roll       = safe_f32(4);
        fc_telem_.pitch      = safe_f32(8);
        fc_telem_.yaw        = safe_f32(12);
        fc_telem_.rollspeed  = safe_f32(16);
        fc_telem_.pitchspeed = safe_f32(20);
        fc_telem_.yawspeed   = safe_f32(24);
        fc_telem_.attitude_valid = true;
        // Fix 45b: ATTITUDE angular rates (rad/s) are the same physical quantity as
        // SCALED_IMU gyro but arrive at 25 Hz vs 2 Hz. Use them for LK IMU hints.
        // Only update gyro if len covers the rollspeed fields (offset 16-28 = 28 bytes).
        if (len >= 28) {
            fc_telem_.gyro_x   = fc_telem_.rollspeed;
            fc_telem_.gyro_y   = fc_telem_.pitchspeed;
            fc_telem_.gyro_z   = fc_telem_.yawspeed;
            fc_telem_.imu_valid = true;
        }
        break;
    }
    
    case 33: {  // GLOBAL_POSITION_INT — fused GPS+INS position
        if (len < 20) break;
        // uint32_t time_boot_ms at p+0
        double lat = safe_i32(4) * 1.0e-7;
        double lon = safe_i32(8) * 1.0e-7;
        if (lat != 0.0 || lon != 0.0) {
            fc_telem_.gps_lat = lat;
            fc_telem_.gps_lon = lon;
            fc_telem_.gps_alt = safe_i32(12) * 0.001f;  // mm → m (AMSL)
            // relative_alt at offset 16 (mm AGL)
            float rel_alt = safe_i32(16) * 0.001f;
            if (rel_alt != 0.0f) fc_telem_.alt = rel_alt;
            fc_telem_.gps_valid = true;
        }
        break;
    }
    
    case 74: {  // VFR_HUD — need at least offset 0-12
        if (len < 12) break;
        fc_telem_.airspeed    = safe_f32(0);
        fc_telem_.groundspeed = safe_f32(4);
        fc_telem_.heading     = safe_i16(8);
        fc_telem_.throttle    = safe_u16(10);
        fc_telem_.alt         = safe_f32(12);
        fc_telem_.climb       = safe_f32(16);
        fc_telem_.hud_valid = true;
        break;
    }
    
    case 65: {  // RC_CHANNELS — 18 channels + rssi
        if (len < 6) break;
        // time_boot_ms at offset 0 (4 bytes)
        // chan1_raw..chan18_raw at offsets 4,6,8,...38 (uint16_t each)
        for (int i = 0; i < 18; i++) {
            fc_telem_.rc_channels[i] = safe_u16(4 + i * 2);
        }
        fc_telem_.rc_chancount = safe_u8(40);
        fc_telem_.rc_rssi = safe_u8(41);
        fc_telem_.rc_valid = true;
        break;
    }
    
    default:
        break;
    }
}

// ═══════════════════════════════════════════════════════════
// Message Building (unchanged logic from previous version)
// ═══════════════════════════════════════════════════════════

MAVVisionPositionEstimate MAVLinkInterface::build_vision_position(
    const SystemState& state, const VOResult& vo) {
    
    MAVVisionPositionEstimate msg;
    msg.usec = now_us();
    msg.x = vo_pose_x_;
    msg.y = vo_pose_y_;
    msg.z = -state.altitude_agl;
    msg.roll  = state.roll * 0.0174533f;
    msg.pitch = state.pitch * 0.0174533f;
    msg.yaw   = state.yaw * 0.0174533f;
    // Note: vo_pose_x_/y_ accumulation is done in tick() — not here.
    return msg;
}

MAVOdometry MAVLinkInterface::build_odometry(
    const SystemState& state, const VOResult& vo) {
    
    MAVOdometry msg;
    msg.time_usec = now_us();
    msg.x = vo_pose_x_;
    msg.y = vo_pose_y_;
    msg.z = -state.altitude_agl;
    msg.vx = vo.vx;
    msg.vy = vo.vy;
    msg.vz = state.vz;
    msg.rollspeed  = state.imu.gyro_x;
    msg.pitchspeed = state.imu.gyro_y;
    msg.yawspeed   = state.imu.gyro_z;
    // Set quality 0-100 based on VO confidence (not just tracking_quality)
    // quality=0 signals EKF3 to ignore this measurement entirely (ArduPilot convention)
    msg.quality = vo.valid ? static_cast<uint8_t>(vo.confidence * 100.0f) : 0;
    msg.frame_id = 0;       // MAV_FRAME_LOCAL_NED
    msg.child_frame_id = 1; // MAV_FRAME_BODY_FRD
    
    // Yaw-only quaternion: do NOT send roll/pitch back to FC.
    // FC already knows its own roll/pitch from its IMU (far more accurate than our estimate).
    // Feeding roll/pitch back via ODOMETRY creates a feedback loop:
    // tiny conversion errors cause EKF3 to "correct" attitude → horizon tilt in MP during yaw.
    // EK3_SRC1_YAW is typically not 6 (ext nav), so yaw here is informational only.
    float cy = std::cos(state.yaw * 0.0087266f);  // half-angle: deg * π/360
    float sy = std::sin(state.yaw * 0.0087266f);
    msg.q[0] = cy;   // w
    msg.q[1] = 0.0f; // x (no roll component)
    msg.q[2] = 0.0f; // y (no pitch component)
    msg.q[3] = sy;   // z (yaw only)
    
    return msg;
}

MAVOpticalFlowRad MAVLinkInterface::build_optical_flow_rad(
    const OpticalFlowData& flow, const VOResult& vo) {
    
    MAVOpticalFlowRad msg;
    msg.time_usec = now_us();
    msg.integrated_x = flow.flow_x;
    msg.integrated_y = flow.flow_y;
    msg.integrated_xgyro = 0;
    msg.integrated_ygyro = 0;
    msg.integrated_zgyro = 0;
    msg.integration_time_us = 20000;
    msg.distance = flow.ground_distance;
    msg.temperature = 2200;
    msg.quality = flow.quality;
    msg.time_delta_distance_us = 20000;
    
    return msg;
}

// ═══════════════════════════════════════════════════════════
// Tick & Stats
// ═══════════════════════════════════════════════════════════

void MAVLinkInterface::tick(const SystemState& state, const VOResult& vo) {
    if (state_ == MAVLinkState::DISCONNECTED) return;
    
    // Always process incoming data (parse MAVLink frames)
    if (!simulated_) {
        process_incoming();
        
        // Request data streams with retry (up to 3 times, every 5 seconds)
        if (state_ == MAVLinkState::CONNECTED && fc_telem_.heartbeat_valid) {
            if (!streams_requested_) {
                request_data_streams();
                streams_requested_ = true;
                stream_request_count_ = 1;
                last_stream_request_us_ = now_us();
            } else if (stream_request_count_ < 3) {
                uint64_t elapsed = now_us() - last_stream_request_us_;
                if (elapsed > 5000000) {  // 5 seconds
                    request_data_streams();
                    stream_request_count_++;
                    last_stream_request_us_ = now_us();
                }
            }
        }
    }
    
    check_connection();
    
    if (heartbeat_count_ % 50 == 0) {
        send_heartbeat();
    }
    heartbeat_count_++;
    
    uint64_t current = now_us();
    if (current - last_vision_us_ >= 33333) {
        // Fix 46: Only send when VO is valid. Sending at 1Hz with quality=0 when invalid
        // CAUSES EKF3 cycling (receives → fails innovation check → "stopped aiding" → repeat).
        // Stopping completely lets EKF3 time out once and cleanly fall back to IMU-only.
        // When VO becomes valid again, EKF3 re-acquires on the first valid message.
        bool should_send = vo.valid;
        if (should_send) {
            // Fix: send ONLY ODOMETRY (#331), NOT VISION_POSITION_ESTIMATE (#102).
            // Sending both causes ArduPilot EKF3 to fuse the same position twice per cycle:
            // effective measurement variance halved → innovation gate too tight →
            // any VO noise triggers rejection → rapid "stopped aiding" cycling.
            // ODOMETRY already contains position + velocity + covariance + quality.
            // VISION_POSITION_ESTIMATE is redundant and harmful when sent simultaneously.
            // Also: accumulate vo_pose internally (was done inside build_vision_position).
            if (vo.valid) {
                vo_pose_x_ += vo.dx;
                vo_pose_y_ += vo.dy;
            }

            auto odom_msg = build_odometry(state, vo);
            send_odometry(odom_msg);

            if (state.flow.valid) {
                auto flow_msg = build_optical_flow_rad(state.flow, vo);
                send_optical_flow_rad(flow_msg);
            }

            last_vision_us_ = current;
        }
    }
}

MAVLinkStats MAVLinkInterface::get_stats() const {
    MAVLinkStats stats;
    stats.state = state_;
    stats.transport = transport_;
    stats.messages_sent = msgs_sent_.load(std::memory_order_relaxed);
    stats.messages_received = msgs_received_.load(std::memory_order_relaxed);
    stats.heartbeats_received = heartbeats_received_.load(std::memory_order_relaxed);
    stats.bytes_received = bytes_received_;
    stats.bytes_sent = bytes_sent_;
    stats.crc_errors = crc_errors_;
    stats.errors = errors_.load(std::memory_order_relaxed);
    stats.last_heartbeat_us = last_heartbeat_us_;
    stats.system_id = 1;
    stats.component_id = 191;
    stats.fc_system_id = fc_system_id_;
    stats.fc_armed = fc_armed_;
    
    // Link quality based on message rate
    if (state_ == MAVLinkState::CONNECTED && fc_telem_.msg_count > 0) {
        stats.link_quality = 0.95f;
        stats.fc_autopilot = fc_telem_.fc_autopilot;
        stats.fc_type = fc_telem_.fc_type;
    } else if (state_ == MAVLinkState::CONNECTED) {
        stats.link_quality = 0.5f;
    } else {
        stats.link_quality = 0;
    }
    
    // FC firmware string based on detected autopilot type
    if (fc_telem_.heartbeat_valid) {
        const char* ap = (fc_telem_.fc_autopilot == 3) ? "ArduPilot" : 
                         (fc_telem_.fc_autopilot == 12) ? "PX4" :
                         (fc_telem_.fc_autopilot == 8) ? "MAVPilot" : "Autopilot";
        const char* tp = (fc_telem_.fc_type == 2) ? "QUADROTOR" :
                         (fc_telem_.fc_type == 1) ? "FIXED_WING" :
                         (fc_telem_.fc_type == 13) ? "HEXAROTOR" :
                         (fc_telem_.fc_type == 14) ? "OCTOROTOR" :
                         (fc_telem_.fc_type == 15) ? "TRICOPTER" :
                         (fc_telem_.fc_type == 4) ? "HELICOPTER" : "VEHICLE";
        std::snprintf(stats.fc_firmware, sizeof(stats.fc_firmware), "%s %s", ap, tp);
    } else {
        std::strncpy(stats.fc_firmware, simulated_ ? "Simulated" : "Waiting...", sizeof(stats.fc_firmware) - 1);
    }
    
    std::strncpy(stats.transport_info, transport_info_, sizeof(stats.transport_info) - 1);
    
    return stats;
}

void MAVLinkInterface::check_connection() {
    if (simulated_) {
        state_ = MAVLinkState::CONNECTED;
        return;
    }
    
    uint64_t current = now_us();
    if (state_ == MAVLinkState::CONNECTED) {
        if (current - last_heartbeat_us_ > HEARTBEAT_TIMEOUT_US) {
            state_ = MAVLinkState::LOST;
            std::printf("[MAVLink] Connection lost (heartbeat timeout)\n");
        }
    }
}

} // namespace jtzero
