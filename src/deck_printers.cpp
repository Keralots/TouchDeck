// Guition deck: printer secrets and the plug power gate. See deck_printers.h.
#include "deck_printers.h"
#include <Preferences.h>
#include "deck_bambu.h"
#include "deck_ha.h"
#include "settings.h"

namespace deck {
namespace {

constexpr const char* NVS_NS = "printers";
// Without a reachable HA the plug state is unknown: after this long the
// printer is tried anyway rather than staying dark.
constexpr uint32_t HA_GRACE_MS = 60000;

struct Power {
  String id;
  bool on = false;
};
std::vector<Power> s_power;  // printers handed to deck_bambu (they have secrets)

void readSecret(const String& id, String& serial, String& code) {
  Preferences p;
  if (!p.begin(NVS_NS, true)) return;
  serial = p.getString(("s_" + id).c_str(), "");
  code = p.getString(("c_" + id).c_str(), "");
  p.end();
}

}  // namespace

void printersApply(const DeckConfig& cfg) {
  std::vector<PrinterConn> list;
  if (advSettings.printers) {  // else switched off on the Advanced page
    for (const PrinterDef& d : cfg.printers) {
      PrinterConn c;
      c.id = d.id;
      c.ip = d.ip;
      readSecret(d.id, c.serial, c.code);
      if (c.serial.length() && c.code.length()) list.push_back(c);
    }
  }
  printersSet(list);
  std::vector<Power> next;
  for (const PrinterConn& c : list) {
    Power p{c.id, false};
    for (const Power& old : s_power)
      if (old.id == c.id) p.on = old.on && printerPowered(c.id);
    next.push_back(p);
  }
  s_power = next;
}

bool printerSecretSet(const String& id, const String& serial, const String& code) {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return false;
  bool ok = true;
  if (serial.length()) ok &= p.putString(("s_" + id).c_str(), serial) > 0;
  if (code.length()) ok &= p.putString(("c_" + id).c_str(), code) > 0;
  p.end();
  return ok;
}

bool printerHasSecret(const String& id) {
  String serial, code;
  readSecret(id, serial, code);
  return serial.length() && code.length();
}

bool printersPowerSync(const DeckConfig& cfg) {
  const bool haUp = haLink() == HA_CONNECTED;
  const bool haMissing = !haUrl().length() || !haHasToken();
  bool changed = false;
  for (const PrinterDef& d : cfg.printers) {
    Power* pw = nullptr;
    for (Power& p : s_power)
      if (p.id == d.id) pw = &p;
    if (!pw) continue;  // no serial / access code yet
    bool on = pw->on;
    HaEntity e;
    if (!d.plug.length() || haMissing) on = true;
    else if (haUp && haGetEntity(d.plug, e) && e.known) on = e.state == "on";
    else if (!haUp && millis() > HA_GRACE_MS) on = true;
    if (on != pw->on) {
      pw->on = on;
      printerPower(d.id, on);
      changed = true;
    }
  }
  return changed;
}

}  // namespace deck
