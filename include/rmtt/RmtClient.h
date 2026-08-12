#pragma once

// Non-blocking, cooperative RMTT client for ESP8266 (Arduino).
//
// Design notes (vs. the desktop rmtt-cpp / Go / Java clients):
//   - No threads: the ESP8266 is single-core; the connect/handshake/read/
//     keepalive machine is driven by calling loop() from Arduino loop().
//   - Transport is WiFiClient (TCP) and, when RMTT_USE_TLS is defined,
//     WiFiClientSecure (TLS). A TCP-only build compiles out every TLS symbol.
//   - Time base is millis() with wraparound-safe unsigned arithmetic.
//
// Flow: TCP/TLS → CONNECT(keepalive, credential) →
// CONNACK(ReturnCode, ServerKeepalive). The ServerKeepalive from CONNACK is
// the final heartbeat interval. Heartbeat: PINGREQ when nothing
// has been sent for serverKeepalive seconds; the connection is judged dead
// when nothing has been received for 1.5×serverKeepalive. serverKeepalive==0
// disables heartbeats and all keepalive-based judgements.
//
// Scheme "tls" is rejected at begin() with a clear message when the library
// was built without RMTT_USE_TLS.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "rmtt/RmtCodec.h"

namespace rmtt {

class RmtTransport;  // transport strategy interface (tcp/tls implemented;
                     // ws/wss/kcp are future extensions); impl in the .cpp

struct RmtClientOptions {
    std::string scheme = "tcp";  // "tcp" | "tls" ("tls" needs RMTT_USE_TLS); new transports plug in via the factory
    std::string host = "192.168.1.100";
    uint16_t port = 18883;
    std::string credential;     // opaque credential, any format
    uint16_t keepalive = 30;    // CONNECT proposal in seconds; 0 = leave to server
    uint32_t connectTimeoutMs = 5000;  // TCP/TLS connect + CONNACK wait budgets
    size_t rxBufferSize = 2048;  // max inbound bytes buffered; overflow ⇒ ProtocolViolation
    bool autoReconnect = true;    // reconnect on any non-normal drop
    uint32_t reconnectDelayMs = 5000;  // fixed backoff between attempts

#if defined(RMTT_USE_TLS)
    bool tlsInsecure = false;    // skip certificate verification (testing only)
    std::string tlsCaPem;        // PEM root CA bundle; empty + insecure=false ⇒ setInsecure
#endif
};

class RmtClient {
public:
    explicit RmtClient(const RmtClientOptions& opts);
    ~RmtClient();

    RmtClient(const RmtClient&) = delete;
    RmtClient& operator=(const RmtClient&) = delete;

    // Start the connect state machine (non-blocking). Call once after WiFi up.
    void begin();

    // Drive the state machine. Call frequently from Arduino loop(); may block
    // up to connectTimeoutMs while the TCP/TLS connection attempt runs.
    void loop();

    bool isConnected() const { return connected_; }
    bool isConnecting() const {
        return state_ == State::Connecting || state_ == State::Handshake;
    }
    bool isEnabled() const { return enabled_; }

    // Publish a PUSH payload. Returns false when not connected.
    bool publish(const uint8_t* data, size_t len);
    bool publish(const std::string& s) {
        return publish(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    // Graceful DISCONNECT(rc) then stop the state machine.
    void disconnect(uint8_t rc = DC_NORMAL);

    // Server-negotiated heartbeat in seconds (0 = disabled).
    uint16_t serverKeepalive() const { return serverKp_; }

    // Callbacks, invoked from loop().
    std::function<void(const std::vector<uint8_t>&)> onMessage;
    std::function<void()> onConnected;
    std::function<void(const std::string& reason)> onConnectionLost;

    // Optional diagnostic hook: (level, text). Invoked from loop() for
    // connect/keepalive/reconnect/protocol events. Empty std::function is a
    // no-op, so builds that do not set it carry no call overhead.
    std::function<void(const std::string& level, const std::string& text)> onLog;

private:
    enum class State : uint8_t {
        Idle, Connecting, Handshake, Connected, Reconnecting, Stopped,
    };

    void readAvailable();
    void parseRx();
    void handlePacket(const Packet& p);
    void keepaliveTick(unsigned long now);
    void sendPingReq();
    void startReconnect(const std::string& reason);
    void closeTransport();
    void failFatal(const std::string& reason);
    void note(const std::string& level, const std::string& text);
    bool loggingEnabled() const { return static_cast<bool>(onLog); }

    RmtClientOptions opts_;
    std::unique_ptr<RmtTransport> t_;

    State state_ = State::Idle;
    bool enabled_ = false;
    bool connected_ = false;

    unsigned long lastSentMs_ = 0;
    unsigned long lastReceivedMs_ = 0;
    unsigned long stateSince_ = 0;
    unsigned long reconnectAt_ = 0;

    uint16_t serverKp_ = 0;
    std::vector<uint8_t> rx_;
};

}  // namespace rmtt