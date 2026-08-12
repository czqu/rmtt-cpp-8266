# rmtt-cpp-8266

RMTT (Remote Message Telemetry Transport, MQTT-like) protocol client library for the **ESP8266** (Arduino / PlatformIO).

<details open>
<summary><b>English</b></summary>

An ESP8266 implementation of the RMTT protocol. Designed specifically for the ESP8266:

- **Threadless**: single-core cooperative state machine, driven by `client.loop()` in the Arduino `loop()`
- **Transport**: TCP (`WiFiClient`, always available) + **TLS (`WiFiClientSecure`/BearSSL, conditional compilation)**
- **TCP-only builds compile no TLS code**: just leave the `RMTT_USE_TLS` macro undefined (the linker never pulls in BearSSL)

## Protocol support

- CONNECT / CONNACK (incl. keepalive negotiation; `ServerKeepalive` is the final heartbeat interval)
- PUSH bidirectional push, PINGREQ / PINGRESP heartbeat, DISCONNECT (10+ return codes)
- Heartbeat dead-detection: `ServerKeepalive==0` disables keepalive judgement entirely; otherwise no inbound traffic for > 1.5× marks the connection dead
- Auto-reconnect (fixed backoff `reconnectDelayMs`); a server `DISCONNECT(0x00)` does not reconnect
- Protocol violation (fixed-header Flags non-zero / invalid length / RX overrun) → reply `DISCONNECT(0x04)` and reconnect

## Usage (PlatformIO)

```ini
[env:esp8266]
platform = espressif8266
framework = arduino
board = nodemcuv2

; TCP-only build: no macro defined
; TCP+TLS build: add the line below
build_flags = -DRMTT_USE_TLS

lib_deps = https://github.com/czqu/rmtt-cpp-8266
```

```cpp
#include <rmtt/RmtClient.h>

rmtt::RmtClientOptions opts;
opts.scheme = "tcp";          // "tcp" | "tls" (tls requires -DRMTT_USE_TLS)
opts.host = "192.168.1.100";
opts.port = 18883;
opts.credential = "device-01";
opts.keepalive = 30;          // CONNECT proposal; finalized by CONNACK.ServerKeepalive
#if defined(RMTT_USE_TLS)
opts.tlsInsecure = true;      // for test self-signed certs; use opts.tlsCaPem for production CA
#endif

rmtt::RmtClient client(opts);
client.onMessage = [](const std::vector<uint8_t>& payload) { /* received PUSH */ };
client.onConnectionLost = [](const std::string& reason) { /* pre-disconnect / pre-reconnect callback */ };

void setup() { client.begin(); }
void loop() {
    client.loop();                      // call frequently (connect is blocking, up to connectTimeoutMs)
    if (client.isConnected()) client.publish("hello");
}
```

## TLS conditional compilation

| Build mode   | Macro                | TLS code included |
|--------------|----------------------|-------------------|
| TCP-only     | (none)               | ❌ none; `scheme="tls"` fails in `begin()` |
| TCP+TLS      | `-DRMTT_USE_TLS`     | ✅ BearSSL (`WiFiClientSecure`) |

Without TLS compiled in, requesting the `tls` scheme → `onConnectionLost("TLS not compiled in (define RMTT_USE_TLS to enable)")`.

## Why QUIC / KCP / WS are not supported

- **QUIC**: no QUIC library runs on the ESP8266 (xtensa-lx106, no TLS 1.3, ~160KB heap);
  ngtcp2 / quiche / lsquic do not support this architecture. Use an ESP32 if you need QUIC.
- **KCP / WS / WSS**: KCP needs concurrent UDP send/receive, WS needs handshake + framing —
  neither is a typical 8266 shape; currently TCP/TLS only.

## Differences from the desktop implementation

- Threading: desktop multi-threaded + blocking IO → 8266 single-threaded cooperative state machine (`loop()`)
- Heartbeat: desktop adaptive heartbeat state machine → 8266 fixed keepalive only (extension point reserved)
- Logging: desktop pluggable Logger → 8266 zero-dependency, app prints via `Serial` itself
- API: `begin()/loop()` replaces `connect()/wait()`; `std::string` + `std::function`, C++11

## Layout

```
rmtt-cpp-8266/
├── library.json            # PlatformIO library manifest
├── include/rmtt/RmtCodec.h # protocol codec (header-only)
├── include/rmtt/RmtClient.h
├── src/RmtClient.cpp       # state machine + WiFiClient/WiFiClientSecure transport adapters
└── examples/
    └── demo/              # buildable/flashable example app (builds the library from ../..)
        ├── platformio.ini
        └── src/main.cpp
```

Build & flash the example:

```sh
pio run -d examples/demo -e esp8266-tcp          # build (TCP)
pio run -d examples/demo -e esp8266-tls          # build (TLS)
pio run -d examples/demo -e esp8266-tcp -t upload  # flash
```

</details>

<details>
<summary><b>中文</b></summary>

RMTT（Remote Message Telemetry Transport，MQTT-like）协议的 **ESP8266 客户端库**（Arduino / PlatformIO）。

RMTT 协议的 ESP8266 客户端实现。针对 ESP8266 专门设计：

- **无线程**：单核协作式状态机，由 Arduino `loop()` 中的 `client.loop()` 驱动
- **传输**：TCP（`WiFiClient`，恒有）+ **TLS（`WiFiClientSecure`/BearSSL，条件编译）**
- **TCP-only 构建不编译任何 TLS 代码**：不定义 `RMTT_USE_TLS` 宏即可（链接器不会带入 BearSSL）

## 协议支持

- CONNECT / CONNACK（含心跳协商，`ServerKeepalive` 即最终心跳间隔）
- PUSH 双向推送、PINGREQ / PINGRESP 心跳、DISCONNECT（10+ ReturnCode）
- 心跳判死：`ServerKeepalive==0` 时完全禁用心跳判定；非 0 时无入站超 1.5× 判死
- 自动重连（固定退避 `reconnectDelayMs`）；服务端 `DISCONNECT(0x00)` 不重连
- 协议违规（固定头 Flags 非 0 / 长度非法 / RX 超限）→ 回 `DISCONNECT(0x04)` 并重连

## 使用（PlatformIO）

```ini
[env:esp8266]
platform = espressif8266
framework = arduino
board = nodemcuv2

; TCP-only 构建：不定义任何宏
; TCP+TLS 构建：追加下面一行
build_flags = -DRMTT_USE_TLS

lib_deps = https://github.com/czqu/rmtt-cpp-8266
```

```cpp
#include <rmtt/RmtClient.h>

rmtt::RmtClientOptions opts;
opts.scheme = "tcp";          // "tcp" | "tls"（tls 需 -DRMTT_USE_TLS 编译）
opts.host = "192.168.1.100";
opts.port = 18883;
opts.credential = "device-01";
opts.keepalive = 30;          // CONNECT 提议值，最终以 CONNACK.ServerKeepalive 为准
#if defined(RMTT_USE_TLS)
opts.tlsInsecure = true;      // 测试自签证书；生产用 opts.tlsCaPem 指定 CA
#endif

rmtt::RmtClient client(opts);
client.onMessage = [](const std::vector<uint8_t>& payload) { /* 收到 PUSH */ };
client.onConnectionLost = [](const std::string& reason) { /* 断连/重连前回调 */ };

void setup() { client.begin(); }
void loop() {
    client.loop();                      // 必须频繁调用（含阻塞式 connect，最长 connectTimeoutMs）
    if (client.isConnected()) client.publish("hello");
}
```

## TLS 条件编译

| 构建方式 | 宏 | 包含 TLS 代码 |
|---------|----|--------------|
| TCP-only | （无） | ❌ 完全不含，`scheme="tls"` 在 `begin()` 报错 |
| TCP+TLS | `-DRMTT_USE_TLS` | ✅ BearSSL（WiFiClientSecure） |

未编译 TLS 时请求 `tls` scheme → `onConnectionLost("TLS not compiled in (define RMTT_USE_TLS to enable)")`。

## 为什么不支持 QUIC / KCP / WS

- **QUIC**：ESP8266（xtensa-lx106，无 TLS 1.3、堆 ~160KB）上不存在可用的 QUIC 库
  （ngtcp2 / quiche / lsquic 均不支持该架构），故未实现。需要 QUIC 请使用 ESP32。
- **KCP / WS / WSS**：KCP 需 UDP 并发收发，WS 需握手/分帧，均非 8266 常见形态；当前仅 TCP/TLS。

## 与桌面版实现的差异

- 线程模型：桌面版多线程 + 阻塞 IO → 8266 版单线程协作式状态机（`loop()`）
- 心跳：桌面版含自适应心跳状态机 → 8266 版仅固定 keepalive（预留扩展）
- 日志：桌面版 pluggable Logger → 8266 版零依赖，由应用自行 Serial 输出
- API：`begin()/loop()` 代替 `connect()/wait()`；`std::string` + `std::function`，C++11

## 目录

```
rmtt-cpp-8266/
├── library.json            # PlatformIO 库清单
├── include/rmtt/RmtCodec.h # 协议编解码（header-only）
├── include/rmtt/RmtClient.h
├── src/RmtClient.cpp       # 状态机 + WiFiClient/WiFiClientSecure 传输适配
└── examples/
    └── demo/             # 可编译/可烧录的示例应用（从 ../.. 构建库）
        ├── platformio.ini
        └── src/main.cpp
```

构建并烧录示例：

```sh
pio run -d examples/demo -e esp8266-tcp          # 构建（TCP）
pio run -d examples/demo -e esp8266-tls          # 构建（TLS）
pio run -d examples/demo -e esp8266-tcp -t upload  # 烧录
```

</details>