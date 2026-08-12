#include <Arduino.h>
#include <rmtt/RmtClient.h>

static rmtt::RmtClientOptions opts;
static rmtt::RmtClient* client = nullptr;

void setup() {
    opts.scheme = "tcp";
    opts.host = "192.0.2.1";
    opts.port = 18883;
    opts.credential = "device-01";
#if defined(RMTT_USE_TLS)
    opts.tlsInsecure = true;
#endif

    client = new rmtt::RmtClient(opts);
    client->begin();
}

void loop() {
    if (client) {
        client->loop();
    }
}