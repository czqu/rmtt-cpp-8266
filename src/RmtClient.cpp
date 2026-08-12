#include "rmtt/RmtClient.h"

#include <Arduino.h>
#include <ESP8266WiFi.h>
#if defined(RMTT_USE_TLS)
#include <WiFiClientSecure.h>
#endif

namespace rmtt {

// Implementations live in the .cpp only; RmtTransport is declared in
// RmtClient.h as an opaque handle behind std::unique_ptr (so the header stays
// free of Arduino/WiFiClient types). The strategy interface lets future
// transports (ws/wss/kcp) be added as a new class + a factory branch without
// touching the RmtClient state machine, while retaining per-transport compile
// isolation: a TCP-only build links no TLS code and pays no flash cost.
class RmtTransport {
public:
    virtual ~RmtTransport() = default;

    virtual bool connect(const std::string& host, uint16_t port) = 0;
    virtual bool connected() = 0;
    virtual int available() = 0;
    virtual size_t read(uint8_t* buf, size_t n) = 0;
    virtual size_t write(const uint8_t* data, size_t n) = 0;
    virtual void stop() = 0;
};

namespace {

// Plain TCP over WiFiClient.
class TcpTransport : public RmtTransport {
public:
    explicit TcpTransport(const RmtClientOptions& o) {
        plain_.setTimeout(o.connectTimeoutMs);
    }
    bool connect(const std::string& host, uint16_t port) override {
        return plain_.connect(host.c_str(), port);
    }
    bool connected() override { return plain_.connected(); }
    int available() override { return plain_.available(); }
    size_t read(uint8_t* buf, size_t n) override { return plain_.read(buf, n); }
    size_t write(const uint8_t* data, size_t n) override { return plain_.write(data, n); }
    void stop() override { plain_.stop(); }

private:
    WiFiClient plain_;
};

#if defined(RMTT_USE_TLS)
// TLS transport over WiFiClientSecure (BearSSL). Not compiled at all when
// RMTT_USE_TLS is undefined, so a TCP-only link carries zero TLS symbols.
class TlsTransport : public RmtTransport {
public:
    explicit TlsTransport(const RmtClientOptions& o) {
        secure_.setTimeout(o.connectTimeoutMs);
        caPem_ = o.tlsCaPem;
        if (o.tlsInsecure) {
            secure_.setInsecure();
        } else if (!caPem_.empty()) {
            trustAnchors_.reset(new X509List(caPem_.c_str()));
            secure_.setTrustAnchors(trustAnchors_.get());
        } else {
            secure_.setInsecure();  // no CA bundle given
        }
    }
    bool connect(const std::string& host, uint16_t port) override {
        return secure_.connect(host.c_str(), port);
    }
    bool connected() override { return secure_.connected(); }
    int available() override { return secure_.available(); }
    size_t read(uint8_t* buf, size_t n) override { return secure_.read(buf, n); }
    size_t write(const uint8_t* data, size_t n) override { return secure_.write(data, n); }
    void stop() override { secure_.stop(); }

private:
    WiFiClientSecure secure_;
    std::string caPem_;                      // keeps the PEM alive
    std::unique_ptr<X509List> trustAnchors_;  // parsed from caPem_ (BearSSL)
};
#endif  // RMTT_USE_TLS

// Transport factory: dispatch on the requested scheme. begin() has already
// validated the scheme (rejecting "tls" when TLS is not compiled in), so the
// null case here is defensive only.
RmtTransport* createTransport(const RmtClientOptions& o) {
    if (o.scheme == "tcp") return new TcpTransport(o);
#if defined(RMTT_USE_TLS)
    if (o.scheme == "tls") return new TlsTransport(o);
#endif
    return nullptr;
}

}  // namespace

RmtClient::RmtClient(const RmtClientOptions& opts) : opts_(opts) {}

RmtClient::~RmtClient() { closeTransport(); }

void RmtClient::begin() {
    if (enabled_ || state_ != State::Idle) return;
    enabled_ = true;
    connected_ = false;
    serverKp_ = 0;
    if (loggingEnabled())
        note("INFO", "begin scheme=" + opts_.scheme + " host=" + opts_.host + ":" + std::to_string(opts_.port));

#if !defined(RMTT_USE_TLS)
    if (opts_.scheme == "tls") {
        failFatal("TLS not compiled in (define RMTT_USE_TLS to enable)");
        return;
    }
#endif
    if (opts_.scheme != "tcp" && opts_.scheme != "tls") {
        failFatal("unsupported scheme: " + opts_.scheme);
        return;
    }

    t_.reset(createTransport(opts_));
    if (!t_) {
        failFatal("unsupported scheme: " + opts_.scheme);
        return;
    }
    state_ = State::Connecting;
    stateSince_ = millis();
}

void RmtClient::loop() {
    if (!enabled_ || state_ == State::Idle || state_ == State::Stopped) return;
    const unsigned long now = millis();
    switch (state_) {
        case State::Connecting: {
            if (!t_) {
                startReconnect("internal error: no transport");
                return;
            }
            if (!t_->connect(opts_.host, opts_.port)) {
                startReconnect("TCP/TLS connect failed");
                if (loggingEnabled()) note("ERROR", "transport connect failed");
                return;
            }
            std::vector<uint8_t> pkt = buildConnect(opts_.keepalive, opts_.credential);
            if (t_->write(pkt.data(), pkt.size()) != pkt.size()) {
                startReconnect("CONNECT write failed");
                note("ERROR", "CONNECT write failed");
                return;
            }
            lastSentMs_ = millis();
            lastReceivedMs_ = millis();
            state_ = State::Handshake;
            stateSince_ = millis();
            if (loggingEnabled()) note("DEBUG", "CONNECT sent, awaiting CONNACK");
            break;
        }
        case State::Handshake: {
            if (now - stateSince_ >= opts_.connectTimeoutMs) {
                startReconnect("no CONNACK received (timeout)");
                return;
            }
            readAvailable();
            break;
        }
        case State::Connected: {
            if (!t_ || !t_->connected()) {
                startReconnect("connection closed by peer");
                return;
            }
            readAvailable();
            if (state_ != State::Connected) break;
            // Use a fresh clock reading: readAvailable() may have updated
            // lastReceivedMs_ past the `now` captured at loop() entry, which
            // would make the unsigned subtraction underflow and spuriously
            // fire the keepalive timeout.
            keepaliveTick(millis());
            break;
        }
        case State::Reconnecting: {
            if (now >= reconnectAt_) {
                state_ = State::Connecting;
                stateSince_ = now;
            }
            break;
        }
        default:
            break;
    }
}

bool RmtClient::publish(const uint8_t* data, size_t len) {
    if (state_ != State::Connected || !t_) return false;
    std::vector<uint8_t> pkt = buildPush(data, len);
    if (t_->write(pkt.data(), pkt.size()) != pkt.size()) {
        startReconnect("PUSH write failed");
        return false;
    }
    lastSentMs_ = millis();
    return true;
}

void RmtClient::disconnect(uint8_t rc) {
    if (!enabled_ || state_ == State::Idle || state_ == State::Stopped) return;
    if (state_ == State::Connected && t_) {
        std::vector<uint8_t> d = buildDisconnect(rc);
        t_->write(d.data(), d.size());
    }
    connected_ = false;
    state_ = State::Stopped;
    closeTransport();
}

void RmtClient::readAvailable() {
    if (!t_) return;
    uint8_t tmp[128];
    while (t_->available() > 0) {
        size_t n = t_->read(tmp, sizeof(tmp));
        if (n == 0) break;
        if (rx_.size() + n > opts_.rxBufferSize) {
            // inbound exceeds the configured cap ⇒ ProtocolViolation
            std::vector<uint8_t> d = buildDisconnect(DC_PROTOCOL_VIOLATION);
            t_->write(d.data(), d.size());
            startReconnect("rx buffer overflow (packet too large)");
            return;
        }
        rx_.insert(rx_.end(), tmp, tmp + n);
        parseRx();
        if (state_ != State::Connected && state_ != State::Handshake) {
            rx_.clear();
            return;
        }
    }
}

void RmtClient::parseRx() {
    while (!rx_.empty()) {
        size_t pos = 0;
        Packet p;
        int r = tryDecodePacket(rx_.data(), rx_.size(), &pos, &p);
        if (r == 0) return;  // need more bytes; keep the buffer
        if (r < 0) {
            // malformed fixed header / flags / length ⇒ ProtocolViolation
            std::vector<uint8_t> d = buildDisconnect(DC_PROTOCOL_VIOLATION);
            if (t_) t_->write(d.data(), d.size());
            startReconnect("protocol violation (bad fixed header)");
            return;
        }
        rx_.erase(rx_.begin(), rx_.begin() + pos);
        handlePacket(p);
        if (state_ != State::Connected && state_ != State::Handshake) {
            rx_.clear();
            return;
        }
    }
}

void RmtClient::handlePacket(const Packet& p) {
    lastReceivedMs_ = millis();
    switch (p.type) {
        case MT_CONNACK: {
            if (state_ != State::Handshake) break;  // unexpected late CONNACK, ignore
            uint8_t rc = 0;
            uint16_t kp = 0;
            if (!parseConnack(p, &rc, &kp)) {
                startReconnect("malformed CONNACK");
                return;
            }
            if (rc != RC_ACCEPTED) {
                std::string reason = "connect refused: ";
                reason += connackReason(rc);
                startReconnect(reason);
                return;
            }
            serverKp_ = kp;  // the server-negotiated heartbeat
            connected_ = true;
            state_ = State::Connected;
            stateSince_ = millis();
            if (loggingEnabled())
                note("INFO", "connected, serverKeepalive=" + std::to_string(kp));
            if (onConnected) onConnected();
            break;
        }
        case MT_PUSH: {
            std::vector<uint8_t> payload;
            if (parsePush(p, &payload) && onMessage) onMessage(payload);
            break;
        }
        case MT_PINGRESP:
            if (loggingEnabled()) note("DEBUG", "PINGRESP received");
            break;
        case MT_DISCONNECT: {
            uint8_t rc = 0;
            parseDisconnect(p, &rc);
            std::string reason = "server disconnect: ";
            reason += disconnectReason(rc);
            connected_ = false;
            if (rc == DC_NORMAL) {
                // 0x00 — do not reconnect unless the app wants to
                state_ = State::Stopped;
                closeTransport();
                if (onConnectionLost) onConnectionLost(reason);
            } else {
                startReconnect(reason);
            }
            break;
        }
        default:
            break;  // reserved / unknown types are ignored
    }
}

void RmtClient::keepaliveTick(unsigned long now) {
    if (serverKp_ == 0) return;  // heartbeat disabled, no keepalive judgement
    const uint32_t intervalMs = static_cast<uint32_t>(serverKp_) * 1000u;
    if (now - lastSentMs_ >= intervalMs) {
        sendPingReq();
    }
    if (now - lastReceivedMs_ > intervalMs + intervalMs / 2) {
        // nothing inbound for ~1.5×serverKp ⇒ connection dead
        if (loggingEnabled()) note("WARN", "keepalive timeout (no inbound for 1.5x serverKp)");
        startReconnect("keepalive timeout (no inbound for 1.5x serverKp)");
    }
}

void RmtClient::sendPingReq() {
    if (!t_) return;
    static const uint8_t ping[2] = {static_cast<uint8_t>(MT_PINGREQ << 4), 0x00};
    if (t_->write(ping, sizeof(ping)) == sizeof(ping)) {
        lastSentMs_ = millis();
        if (loggingEnabled()) note("DEBUG", "PINGREQ sent");
    } else {
        if (loggingEnabled()) note("ERROR", "PINGREQ write failed");
        startReconnect("PINGREQ write failed");
    }
}

void RmtClient::note(const std::string& level, const std::string& text) {
    if (onLog) onLog(level, text);
}

void RmtClient::startReconnect(const std::string& reason) {
    if (state_ == State::Stopped) return;
    const bool wasConnected = connected_;
    connected_ = false;
    if (t_) t_->stop();  // release the socket only; keep the transport object so
                         // TLS trust anchors are parsed once, not per reconnect
    rx_.clear();
    if (wasConnected && onConnectionLost) onConnectionLost(reason);
    if (!opts_.autoReconnect) {
        state_ = State::Stopped;
        return;
    }
    reconnectAt_ = millis() + opts_.reconnectDelayMs;
    state_ = State::Reconnecting;
}

void RmtClient::closeTransport() {
    if (t_) {
        t_->stop();
        t_.reset();
    }
}

void RmtClient::failFatal(const std::string& reason) {
    connected_ = false;
    closeTransport();
    state_ = State::Stopped;
    if (onConnectionLost) onConnectionLost(reason);
}

}  // namespace rmtt