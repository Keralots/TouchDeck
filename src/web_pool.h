// Web server that holds several waiting connections, so one that sends nothing
// (a browser's spare socket) or half a request does not block the others.
#pragma once
#include <WebServer.h>
#include <lwip/sockets.h>

class PooledWebServer : public WebServer {
public:
  using WebServer::WebServer;

  void handleClient() override {
    if (_currentStatus == HC_NONE) {
      fill();
      const int i = ready();
      if (i < 0) {
        if (_nullDelay) delay(1);
        return;
      }
      _currentClient = _wait[i].client;
      _wait[i] = Slot();
      _currentStatus = HC_WAIT_READ;
      _statusChange = millis();
    }
    WebServer::handleClient();
  }

private:
  static constexpr int SLOTS = 6;
  static constexpr uint32_t WAIT_MS = 5000;
  struct Slot {
    NetworkClient client;
    uint32_t since = 0;
    bool used = false;
  };
  Slot _wait[SLOTS];

  // A free slot, else the oldest one that has sent nothing (its browser reconnects).
  int slot() {
    int oldest = -1;
    for (int i = 0; i < SLOTS; i++) {
      if (!_wait[i].used) return i;
      if (!_wait[i].client.available() && (oldest < 0 || (int32_t)(_wait[i].since - _wait[oldest].since) < 0)) oldest = i;
    }
    return oldest;
  }

  void fill() {
    for (int n = 0; n < SLOTS && _server.hasClient(); n++) {
      const int i = slot();
      if (i < 0) return;
      _wait[i] = Slot();
      _wait[i].client = _server.accept();
      _wait[i].since = millis();
      _wait[i].used = (bool)_wait[i].client;
    }
  }

  // Oldest connection whose request headers have arrived.
  int ready() {
    int best = -1;
    for (int i = 0; i < SLOTS; i++) {
      Slot& s = _wait[i];
      if (!s.used) continue;
      const int n = s.client.available();
      if (n <= 0 && !s.client.connected()) {
        s = Slot();
        continue;
      }
      if (n > 0 && complete(s.client, n)) {
        if (best < 0 || (int32_t)(s.since - _wait[best].since) < 0) best = i;
      } else if (millis() - s.since > WAIT_MS) {
        s = Slot();
      }
    }
    return best;
  }

  // lwIP peeks only the first segment: more bytes than that = a long header, let the parser wait for it.
  bool complete(NetworkClient& c, int n) {
    static char buf[1460];
    const int got = lwip_recv(c.fd(), buf, sizeof(buf), MSG_PEEK | MSG_DONTWAIT);
    if (got <= 0) return false;
    return n > got || memmem(buf, got, "\r\n\r\n", 4) != nullptr;
  }
};
