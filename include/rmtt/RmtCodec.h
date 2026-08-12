#pragma once

// RMTT wire protocol codec for ESP8266.
// Header-only (protocol layer is platform independent). Message framing,
// variable-length remaining length and the typed CONNECT / CONNACK / PUSH /
// DISCONNECT builders & parsers.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rmtt {

// Message types.
enum : uint8_t {
    MT_CONNECT = 1,
    MT_CONNACK = 2,
    MT_PUSH = 3,
    MT_PINGREQ = 5,
    MT_PINGRESP = 6,
    MT_DISCONNECT = 14,
};

// CONNACK return codes.
enum : uint8_t {
    RC_ACCEPTED = 0x00,
    RC_BAD_PROTOCOL_VERSION = 0x01,
    RC_SERVER_UNAVAILABLE = 0x02,
    RC_NOT_AUTHORISED = 0x03,
    RC_UNKNOWN_ERROR = 0xFE,
    RC_RESERVED = 0xFF,
};

// DISCONNECT return codes.
enum : uint8_t {
    DC_NORMAL = 0x00,
    DC_CREDENTIAL_EXPIRED = 0x01,
    DC_SESSION_TAKEN_OVER = 0x02,
    DC_SERVER_SHUTDOWN = 0x03,
    DC_PROTOCOL_VIOLATION = 0x04,
    DC_KEEPALIVE_TIMEOUT = 0x05,
    DC_KICKED_BY_ADMIN = 0x06,
    DC_RATE_LIMITED = 0x07,
    DC_CREDENTIAL_REJECTED = 0x08,
    DC_UNKNOWN = 0xFE,
};

// CONNECT magic number, big-endian on the wire ("czqu").
constexpr uint32_t kMagic = 0x637A7175U;
constexpr uint8_t kProtocolVersion = 1;

// A decoded RMTT packet. body holds the bytes after the fixed header and the
// variable-length RemainingLength field.
struct Packet {
    uint8_t type = 0;
    std::vector<uint8_t> body;
};

// MQTT-style variable-length integer for RemainingLength.
inline size_t encodeLength(uint32_t length, uint8_t* out) {
    size_t n = 0;
    for (;;) {
        uint8_t digit = static_cast<uint8_t>(length % 128);
        length /= 128;
        if (length > 0) digit |= 0x80;
        out[n++] = digit;
        if (length == 0) break;
    }
    return n;
}

inline bool decodeLength(const uint8_t* data, size_t size, size_t* pos, uint32_t* out) {
    uint32_t value = 0;
    uint32_t multiplier = 0;
    while (multiplier < 27) {
        if (*pos >= size) return false;
        uint8_t digit = data[(*pos)++];
        value |= static_cast<uint32_t>(digit & 127) << multiplier;
        if ((digit & 128) == 0) {
            *out = value;
            return true;
        }
        multiplier += 7;
    }
    return false;
}

// Serialize a packet: fixed header (type<<4 | 0) + varint remaining length + body.
inline std::vector<uint8_t> serializePacket(uint8_t type, const uint8_t* body, size_t bodyLen) {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + bodyLen);
    out.push_back(static_cast<uint8_t>((type << 4) | 0x00));
    uint8_t lenbuf[4];
    size_t n = encodeLength(static_cast<uint32_t>(bodyLen), lenbuf);
    out.insert(out.end(), lenbuf, lenbuf + n);
    if (bodyLen > 0) out.insert(out.end(), body, body + bodyLen);
    return out;
}

inline std::vector<uint8_t> serializePacket(uint8_t type, const std::vector<uint8_t>& body) {
    return serializePacket(type, body.data(), body.size());
}

// Try to decode one full packet from a byte buffer (streaming safe: on a
// "need more bytes" outcome *pos is restored to its original value).
// Returns 1 = packet decoded (+*pos advanced, *out set),
//         0 = need more bytes (no progress),
//        -1 = malformed fixed header / flags / length (protocol violation).
inline int tryDecodePacket(const uint8_t* data, size_t size, size_t* pos, Packet* out) {
    if (*pos >= size) return 0;
    size_t start = *pos;
    uint8_t typeAndFlags = data[(*pos)++];
    if ((typeAndFlags & 0x0F) != 0) {  // fixed header flags must be 0
        *pos = start;
        return -1;
    }
    uint32_t remaining = 0;
    if (!decodeLength(data, size, pos, &remaining)) {
        *pos = start;
        return 0;
    }
    if (remaining > size - *pos) {
        *pos = start;
        return 0;
    }
    Packet p;
    p.type = typeAndFlags >> 4;
    if (remaining > 0) p.body.assign(data + *pos, data + *pos + remaining);
    *pos += remaining;
    *out = std::move(p);
    return 1;
}

// Typed builders / parsers.
inline std::vector<uint8_t> buildConnect(uint16_t keepalive, const std::string& credential) {
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>(kMagic >> 24));
    body.push_back(static_cast<uint8_t>(kMagic >> 16));
    body.push_back(static_cast<uint8_t>(kMagic >> 8));
    body.push_back(static_cast<uint8_t>(kMagic));
    body.push_back(kProtocolVersion);
    body.push_back(0x00);  // reserved flags
    body.push_back(static_cast<uint8_t>(keepalive >> 8));
    body.push_back(static_cast<uint8_t>(keepalive & 0xFF));
    size_t clen = credential.size();
    if (clen > 0xFFFF) clen = 0xFFFF;  // credential ≤ 65535 bytes
    body.push_back(static_cast<uint8_t>(clen >> 8));
    body.push_back(static_cast<uint8_t>(clen & 0xFF));
    if (clen > 0) body.insert(body.end(), credential.begin(), credential.begin() + clen);
    return serializePacket(MT_CONNECT, body);
}

inline bool parseConnack(const Packet& p, uint8_t* rc, uint16_t* serverKp) {
    if (p.type != MT_CONNACK || p.body.size() < 3) return false;
    *rc = p.body[0];
    *serverKp = static_cast<uint16_t>((static_cast<uint16_t>(p.body[1]) << 8) | p.body[2]);
    return true;
}

inline std::vector<uint8_t> buildPush(const uint8_t* payload, size_t len) {
    std::vector<uint8_t> body;
    body.reserve(1 + len);
    body.push_back(0x00);  // reserved
    if (len > 0) body.insert(body.end(), payload, payload + len);
    return serializePacket(MT_PUSH, body);
}

inline std::vector<uint8_t> buildPush(const std::string& payload) {
    return buildPush(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
}

inline bool parsePush(const Packet& p, std::vector<uint8_t>* payload) {
    if (p.type != MT_PUSH || p.body.size() < 1) return false;
    payload->assign(p.body.begin() + 1, p.body.end());
    return true;
}

inline std::vector<uint8_t> buildDisconnect(uint8_t rc) {
    std::vector<uint8_t> body(1, rc);
    return serializePacket(MT_DISCONNECT, body);
}

inline bool parseDisconnect(const Packet& p, uint8_t* rc) {
    if (p.type != MT_DISCONNECT || p.body.size() < 1) return false;
    *rc = p.body[0];
    return true;
}

inline const char* connackReason(uint8_t rc) {
    switch (rc) {
        case RC_ACCEPTED: return "Connection Accepted";
        case RC_BAD_PROTOCOL_VERSION: return "Refused: Bad Protocol Version";
        case RC_SERVER_UNAVAILABLE: return "Refused: Server Unavailable";
        case RC_NOT_AUTHORISED: return "Refused: Not Authorised";
        case RC_UNKNOWN_ERROR: return "Connection Error (unknown)";
        default: return "Unknown";
    }
}

inline const char* disconnectReason(uint8_t rc) {
    switch (rc) {
        case DC_NORMAL: return "NormalDisconnect";
        case DC_CREDENTIAL_EXPIRED: return "CredentialExpired";
        case DC_SESSION_TAKEN_OVER: return "SessionTakenOver";
        case DC_SERVER_SHUTDOWN: return "ServerShutdown";
        case DC_PROTOCOL_VIOLATION: return "ProtocolViolation";
        case DC_KEEPALIVE_TIMEOUT: return "KeepaliveTimeout";
        case DC_KICKED_BY_ADMIN: return "KickedByAdmin";
        case DC_RATE_LIMITED: return "RateLimited";
        case DC_CREDENTIAL_REJECTED: return "CredentialRejected";
        case DC_UNKNOWN: return "UnknownError";
        default: return "Unknown";
    }
}

}  // namespace rmtt