#include "portal_auth.h"
#include "pin_lock.h"
#include "config.h"
#include "deck_ui.h"
#include "wifi_manager.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <time.h>

static constexpr int MAX_SESSIONS = 6;
static constexpr uint32_t SESSION_RAM_MS = 24UL * 3600000;  // "until the browser closes"
static constexpr size_t TOKEN_LEN = 32;
static const char COOKIE[] = "tdsess=";

struct Session {
  char tok[TOKEN_LEN + 1];  // empty = free slot
  uint32_t exp;             // unix seconds; 0 = in RAM only, 24 h from bornMs
  uint32_t bornMs;
};

static WebServer* s_srv = nullptr;
static Session s_sess[MAX_SESSIONS];
static bool s_ok = false;  // the gate's answer for the request being handled

static bool clockOk() { return time(nullptr) > 1700000000; }

// --- Sessions ----------------------------------------------------------------

static void saveSessions() {
  Session keep[MAX_SESSIONS];
  size_t n = 0;
  for (const Session& s : s_sess)
    if (s.tok[0] && s.exp) keep[n++] = s;
  Preferences p;
  if (!p.begin(PIN_NAMESPACE, false)) return;
  if (n) p.putBytes("sess", keep, n * sizeof(Session));
  else p.remove("sess");
  p.end();
}

static void loadSessions() {
  Preferences p;
  if (!p.begin(PIN_NAMESPACE, true)) return;
  const size_t len = p.getBytesLength("sess");
  if (len && len <= sizeof(s_sess) && len % sizeof(Session) == 0) p.getBytes("sess", s_sess, len);
  p.end();
  for (Session& s : s_sess) s.tok[TOKEN_LEN] = 0;
}

static void clearSessions() {
  memset(s_sess, 0, sizeof(s_sess));
  saveSessions();
}

static bool expired(const Session& s) {
  return s.exp ? clockOk() && (uint32_t)time(nullptr) >= s.exp : millis() - s.bornMs >= SESSION_RAM_MS;
}

// A free or expired slot, else the oldest RAM session, else the one that ends first.
static Session& sessionSlot() {
  Session* pick = nullptr;
  for (Session& s : s_sess) {
    if (!s.tok[0] || expired(s)) return s;
    if (!pick || (pick->exp && !s.exp)) pick = &s;
    else if (!pick->exp && !s.exp) { if (s.bornMs - pick->bornMs > UINT32_MAX / 2) pick = &s; }
    else if (pick->exp && s.exp && s.exp < pick->exp) pick = &s;
  }
  return *pick;
}

static void startSession(uint32_t days) {
  Session& s = sessionSlot();
  const bool stored = s.tok[0] && s.exp;
  memset(&s, 0, sizeof(s));
  uint8_t raw[TOKEN_LEN / 2];
  esp_fill_random(raw, sizeof(raw));
  for (size_t i = 0; i < sizeof(raw); i++) snprintf(s.tok + i * 2, 3, "%02x", raw[i]);
  s.bornMs = millis();
  // Without a set clock an end date means nothing: that session stays in RAM.
  if (days && clockOk()) s.exp = (uint32_t)time(nullptr) + days * 86400UL;
  if (s.exp || stored) saveSessions();
  String v = String(COOKIE) + s.tok + "; Path=/; HttpOnly; SameSite=Lax";
  if (days) v += "; Max-Age=" + String(days * 86400UL);
  s_srv->sendHeader("Set-Cookie", v);
}

static Session* requestSession() {
  const String c = s_srv->header("Cookie");
  int at = 0;
  while ((at = c.indexOf(COOKIE, at)) >= 0) {
    const int start = at + sizeof(COOKIE) - 1;
    const bool whole = at == 0 || c[at - 1] == ' ' || c[at - 1] == ';';
    at = start;
    if (!whole || c.length() < start + TOKEN_LEN) continue;
    for (Session& s : s_sess) {
      if (!s.tok[0]) continue;
      uint8_t diff = 0;
      for (size_t i = 0; i < TOKEN_LEN; i++) diff |= s.tok[i] ^ c[start + i];
      if (!diff && !expired(s)) return &s;
    }
  }
  return nullptr;
}

// --- The gate ----------------------------------------------------------------

// HTTP Basic with the PIN as the password, any user name. A wrong one counts as a try.
static bool basicOk() {
  const String h = s_srv->header("Authorization");
  if (!h.startsWith("Basic ")) return false;
  uint8_t plain[96];
  size_t n = 0;
  if (mbedtls_base64_decode(plain, sizeof(plain) - 1, &n, (const uint8_t*)h.c_str() + 6, h.length() - 6)) n = 0;
  plain[n] = 0;
  const char* colon = strchr((const char*)plain, ':');
  uint32_t left;
  return pinTry(PIN_BASIC, colon ? colon + 1 : "", left) == PIN_OK;
}

static bool signedIn() { return !pinIsSet() || requestSession() || basicOk(); }

// Open without a sign-in: the sign-in page and its files, the captive portal,
// asking for PC pairing, and reading states that control nothing. Several of
// those states share their URI with a POST that does, so they are GET only.
static bool openRequest(HTTPMethod method, const String& uri) {
  static const char* const kGet[] = {
    "/", "/portal.css", "/portal-layout.css", "/portal.js", "/deck.js", "/deck",
    "/generate_204", "/gen_204", "/connecttest.txt", "/hotspot-detect.html", "/canonical.html",
    "/api/status", "/api/ha/status", "/api/pc/status", "/api/printers", "/api/audio", "/api/deck/timer",
    "/api/deck/frame", "/api/deck/daily", "/api/deck/wallpaper", "/api/deck/icons", "/api/deck/brand",
    "/api/deck/perf",
  };
  static const char* const kPost[] = {
    "/auth/login", "/auth/logout", "/api/pc/pair/confirm", "/api/pc/pair/cancel",
  };
  if (method == HTTP_GET) {
    for (const char* o : kGet)
      if (uri == o) return true;
  } else if (method == HTTP_POST) {
    // Pairing is answered on the screen, so a locked screen needs the PIN before it.
    if (uri == "/api/pc/pair") return pinEditLock() == EDIT_ALLOWED;
    for (const char* o : kPost)
      if (uri == o) return true;
  }
  return false;
}

static void sendMessage(int code, bool ok, const String& message) {
  JsonDocument doc;
  doc["success"] = ok;
  doc["message"] = message;
  String out;
  serializeJson(doc, out);
  s_srv->sendHeader("Cache-Control", "no-store");
  s_srv->send(code, "application/json", out);
}

static bool gate(WebServer& server, Middleware::Callback next) {
  s_ok = signedIn();
  const String uri = server.uri();
  if (s_ok || openRequest(server.method(), uri)) return next();
  // A phone in setup mode asks for addresses of its own; they lead to the sign-in page.
  if (isAPMode() && server.method() == HTTP_GET && !uri.startsWith("/api/")) {
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302, "text/plain", "");
    return true;
  }
  sendMessage(401, false, "Sign-in required");
  return true;
}

WebServer::THandlerFunction portalAuthUpload(WebServer::THandlerFunction handler) {
  return [handler]() {
    static bool allowed = false;
    if (!s_srv->header("Content-Type").startsWith("multipart/")) return;  // raw body: there is no upload()
    const HTTPUploadStatus st = s_srv->upload().status;
    if (st == UPLOAD_FILE_START) allowed = signedIn();
    if (allowed) handler();
    if (st == UPLOAD_FILE_END || st == UPLOAD_FILE_ABORTED) allowed = false;
  };
}

bool portalAuthOk() { return s_ok; }

// --- Routes ------------------------------------------------------------------

static String lockedText(uint32_t secs) {
  return "Too many wrong tries. Try again in " +
         (secs >= 120 ? String((secs + 59) / 60) + " min." : String(secs) + " s.");
}

// Checks a PIN typed in the portal; answers the request itself when it is not right.
static bool typedPinOk(const String& pin) {
  uint32_t left;
  const PinResult r = pinTry(PIN_LOGIN, pin.c_str(), left);
  if (r == PIN_OK) return true;
  sendMessage(r == PIN_LOCKED ? 429 : 403, false, left ? lockedText(left) : String("Wrong PIN."));
  return false;
}

static uint32_t rememberDays() {
  const int d = s_srv->arg("remember").toInt();
  return d == 7 || d == 30 ? d : 0;
}

static void handleLogin() {
  if (!pinIsSet()) {
    sendMessage(200, true, "No PIN is set.");
    return;
  }
  if (!typedPinOk(s_srv->arg("pin"))) return;
  startSession(rememberDays());
  sendMessage(200, true, "Signed in.");
}

static void handleLogout() {
  if (Session* s = requestSession()) {
    const bool stored = s->exp;
    memset(s, 0, sizeof(*s));
    if (stored) saveSessions();
  }
  s_srv->sendHeader("Set-Cookie", String(COOKIE) + "; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
  sendMessage(200, true, "Signed out.");
}

// POST /save/security (pinCurrent, pinNew, pinRepeat, pinRemove, editLock). A set
// PIN is asked for again, so a browser left signed in cannot take it off.
static void handleSaveSecurity() {
  const String pin = s_srv->arg("pinNew");
  const bool remove = s_srv->hasArg("pinRemove");
  const String lock = s_srv->arg("editLock");
  const EditLock edit = lock == "allow" ? EDIT_ALLOWED : lock == "off" ? EDIT_OFF : lock == "ask" ? EDIT_ASK : pinEditLock();
  const bool keepPin = pinIsSet() && !pin.length();
  if (!remove && !keepPin) {
    if (!pin.length()) return sendMessage(400, false, "Enter a new PIN.");
    if (!pinValid(pin)) return sendMessage(400, false, "The PIN must be 4 to 8 digits.");
    if (pin != s_srv->arg("pinRepeat")) return sendMessage(400, false, "The two PINs differ.");
  }
  if (pinIsSet() && !typedPinOk(s_srv->arg("pinCurrent"))) return;
  if (remove) {
    if (!pinRemove()) return sendMessage(500, false, "Could not remove the PIN.");
    clearSessions();
    return sendMessage(200, true, "PIN removed.");
  }
  if (keepPin) {
    if (!pinSetEditLock(edit)) return sendMessage(500, false, "Could not store the setting.");
    return sendMessage(200, true, "Security settings saved.");
  }
  if (!pinStore(pin)) return sendMessage(500, false, "Could not store the PIN. No PIN is set now.");
  // Every other browser has to sign in with the new PIN; this one stays in.
  clearSessions();
  startSession(30);
  if (!pinSetEditLock(edit)) return sendMessage(500, false, "The PIN is saved, the other setting is not.");
  sendMessage(200, true, "PIN saved.");
}

void portalAuthTick() {
  if (!pinTakeClearRequest()) return;
  const bool ok = pinRemove();
  if (ok) clearSessions();
  Serial.printf("\n@TD {\"pin\":\"%s\"}\n", ok ? "cleared" : "failed");
  if (ok) deckToast("PIN removed over USB");
}

void portalAuthBegin(WebServer& server) {
  s_srv = &server;
  loadSessions();
  server.addMiddleware(gate);
  server.on("/auth/login", HTTP_POST, handleLogin);
  server.on("/auth/logout", HTTP_POST, handleLogout);
  server.on("/save/security", HTTP_POST, handleSaveSecurity);
}
