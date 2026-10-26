// Guition deck: Bambu printers from Home Assistant. See deck_bambu_ha.h.
#include "deck_bambu_ha.h"
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <new>
#include "deck_ha.h"

namespace deck {
namespace {

constexpr uint32_t RETRY_MS = 30000;  // a failed lookup is tried again after this

enum Field : uint8_t {
  F_STATUS, F_STAGE, F_PROGRESS, F_REMAIN, F_LAYER, F_LAYERS, F_TASK, F_SPEED,
  F_NOZZLE, F_NOZZLE_T, F_LNOZ, F_LNOZ_T, F_RNOZ, F_RNOZ_T, F_BED, F_BED_T, F_CHAMBER,
  F_ONLINE, F_LIGHT, F_WEIGHT,
  F_HUM, F_ATEMP, F_DRY, F_TRAY,  // per AMS unit
  F_EXT,
};

struct KeyField {
  const char* key;
  const char* domain;
  Field field;
};
const KeyField kPrinterKeys[] = {
    {"print_status", "sensor", F_STATUS},       {"stage", "sensor", F_STAGE},
    {"print_progress", "sensor", F_PROGRESS},   {"remaining_time", "sensor", F_REMAIN},
    {"current_layer", "sensor", F_LAYER},       {"total_layers", "sensor", F_LAYERS},
    {"subtask_name", "sensor", F_TASK},         {"speed_profile", "sensor", F_SPEED},
    {"nozzle_temp", "sensor", F_NOZZLE},        {"target_nozzle_temp", "sensor", F_NOZZLE_T},
    {"left_nozzle_temp", "sensor", F_LNOZ},     {"left_target_nozzle_temp", "sensor", F_LNOZ_T},
    {"right_nozzle_temp", "sensor", F_RNOZ},    {"right_target_nozzle_temp", "sensor", F_RNOZ_T},
    {"bed_temp", "sensor", F_BED},              {"target_bed_temp", "sensor", F_BED_T},
    {"chamber_temp", "sensor", F_CHAMBER},      {"online", "binary_sensor", F_ONLINE},
    {"chamber_light", "light", F_LIGHT},        {"print_weight", "sensor", F_WEIGHT},
};
const KeyField kAmsKeys[] = {
    {"humidity", "sensor", F_HUM},
    {"temperature", "sensor", F_ATEMP},
    {"remaining_drying_time", "sensor", F_DRY},
};

// pybambu's CURRENT_STAGE_IDS, index = stg_cur.
const char* const kStages[] = {
    "printing", "auto_bed_leveling", "heatbed_preheating", "sweeping_xy_mech_mode",
    "changing_filament", "m400_pause", "paused_filament_runout", "heating_hotend",
    "calibrating_extrusion", "scanning_bed_surface", "inspecting_first_layer",
    "identifying_build_plate_type", "calibrating_micro_lidar", "homing_toolhead",
    "cleaning_nozzle_tip", "checking_extruder_temperature", "paused_user",
    "paused_front_cover_falling", "calibrating_micro_lidar", "calibrating_extrusion_flow",
    "paused_nozzle_temperature_malfunction", "paused_heat_bed_temperature_malfunction",
    "filament_unloading", "paused_skipped_step", "filament_loading", "calibrating_motor_noise",
    "paused_ams_lost", "paused_low_fan_speed_heat_break", "paused_chamber_temperature_control_error",
    "cooling_chamber", "paused_user_gcode", "motor_noise_showoff",
    "paused_nozzle_filament_covered_detected", "paused_cutter_error", "paused_first_layer_error",
    "paused_nozzle_clog", "check_absolute_accuracy_before_calibration",
    "absolute_accuracy_calibration", "check_absolute_accuracy_after_calibration",
    "calibrate_nozzle_offset", "bed_level_high_temperature", "check_quick_release",
    "check_door_and_cover", "laser_calibration", "check_plaform", "check_birdeye_camera_position",
    "calibrate_birdeye_camera", "bed_level_phase_1", "bed_level_phase_2", "heating_chamber",
    "heated_bedcooling", "print_calibration_lines", "check_material",
    "calibrating_live_view_camera", "waiting_for_heatbed_temperature", "check_material_position",
    "calibrating_cutter_model_offset", "measuring_surface", "thermal_preconditioning",
    "homing_blade_holder", "calibrating_camera_offset", "calibrating_blade_holder_position",
    "hotend_pick_place_test", "waiting_chamber_temperature_equalize", "preparing_hotend",
    "calibrating_detection_nozzle_clumping", "purifying_chamber_air", "measuring_rotary_attachment",
    "moving_toolhead_above_purge_chute", "cooling_nozzle", "moving_toolhead_to_center_of_heatbed",
    "active_arc_fitting", "hotend_type_detection", "build_plate_alignment_detection",
    "heatbed_surface_foreign_object_detection", "heatbed_underside_foreign_object_detection",
    "pre_extrusion_before_printing", "preparing_ams",
};

struct Bind {
  char entity[HA_RAW_ID];
  Field field;
  uint8_t unit, slot;  // ams[] index and tray, F_HUM..F_TRAY
  float scale;         // to minutes, F_REMAIN / F_DRY
};

// What the lookup found; a fresh state starts from it.
struct Layout {
  uint8_t amsCount = 0;
  uint8_t id[PRN_AMS_UNITS] = {};
  uint8_t slots[PRN_AMS_UNITS] = {};
  bool dual = false;
};

enum Disc : uint8_t { D_NONE, D_QUEUED, D_DONE, D_FAILED };

struct Printer {
  String id, serial;
  bool powered = false;
  Disc disc = D_NONE;
  uint32_t discAt = 0;
  char err[48] = "";
  Bind* binds = nullptr;  // PSRAM
  uint16_t nBinds = 0;
  Layout layout;
  char light[72] = "";
  PrinterState* st = nullptr;  // PSRAM, while powered
  // Fields the state has no room for.
  bool trayActive[PRN_AMS_UNITS][PRN_AMS_SLOTS] = {};
  bool extActive = false;
  bool online = false;
  float actTemp = NAN, actTarget = NAN;
};

Printer s_prn[PRN_MAX];
uint8_t s_count = 0;
uint32_t s_version = 1, s_gen = 1;

SemaphoreHandle_t mutex() {
  static StaticSemaphore_t storage;
  static SemaphoreHandle_t handle = xSemaphoreCreateMutexStatic(&storage);
  return handle;
}
struct Lock {
  Lock() { xSemaphoreTake(mutex(), portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mutex()); }
};

Printer* find(const String& id) {
  for (uint8_t i = 0; i < s_count; i++)
    if (id.length() && s_prn[i].id == id) return &s_prn[i];
  return nullptr;
}

void resetState(Printer& p) {
  if (!p.st) {
    void* mem = heap_caps_malloc(sizeof(PrinterState), MALLOC_CAP_SPIRAM);
    if (!mem) return;
    p.st = new (mem) PrinterState();
  }
  PrinterState& s = *p.st;
  s = PrinterState{};
  s.link = PRN_CONNECTING;
  s.dualNozzle = p.layout.dual;
  s.amsCount = p.layout.amsCount;
  for (uint8_t u = 0; u < s.amsCount; u++) {
    s.ams[u].present = true;
    s.ams[u].id = p.layout.id[u];
    s.ams[u].slots = p.layout.slots[u];
  }
  memset(p.trayActive, 0, sizeof(p.trayActive));
  p.extActive = false;
  p.online = false;
  p.actTemp = p.actTarget = NAN;
}

void freeState(Printer& p) {
  if (!p.st) return;
  p.st->~PrinterState();
  heap_caps_free(p.st);
  p.st = nullptr;
}

void hook(const char* id, JsonObjectConst ev, bool full);

// Entities of powered printers whose lookup is done. Under the lock: the ids
// point into the binds (the HA lock is only ever taken after this one).
void syncRaw() {
  Lock g;
  std::vector<const char*> ids;
  for (uint8_t i = 0; i < s_count; i++) {
    const Printer& p = s_prn[i];
    if (!p.powered || p.disc != D_DONE) continue;
    for (uint16_t b = 0; b < p.nBinds; b++) ids.push_back(p.binds[b].entity);
  }
  haSetRawEntities(ids.data(), ids.size(), hook);
}

// --- Lookup -------------------------------------------------------------------

const KeyField* keyOf(const KeyField* table, size_t n, const char* key, const char* entity) {
  for (size_t i = 0; i < n; i++) {
    const size_t dl = strlen(table[i].domain);
    if (!strcmp(table[i].key, key) && !strncmp(entity, table[i].domain, dl) && entity[dl] == '.')
      return &table[i];
  }
  return nullptr;
}

int unitIndex(Layout& l, uint8_t raw) {
  for (uint8_t u = 0; u < l.amsCount; u++)
    if (l.id[u] == raw) return u;
  return -1;
}

// unique_ids: "<serial>_<key>", "<model>_<serial>_AMS_<ams serial>_<key>",
// "<model>_<serial>_ExternalSpool[2]_<key>". The AMS number comes from the
// AMS device's own name ("<model>_<serial>_AMS_1", 128+ = AMS HT).
void lookupDone(uint32_t tag, bool ok, const HaRegEntry* list, size_t count, const char* err) {
  {
    Lock g;
    const uint8_t idx = tag & 0xF;
    if ((tag >> 4) != (s_gen & 0x0FFFFFFF) || idx >= s_count) return;  // config changed meanwhile
    Printer& p = s_prn[idx];
    p.discAt = millis();
    if (!ok) {
      p.disc = D_FAILED;
      strlcpy(p.err, err && *err ? err : "Home Assistant lookup failed", sizeof(p.err));
      ++s_version;
      return;
    }
    const String serial = p.serial;
    const String amsTag = "_" + serial + "_AMS_";
    const String extTag = "_" + serial + "_ExternalSpool_";
    Bind* binds = (Bind*)heap_caps_calloc(count ? count : 1, sizeof(Bind), MALLOC_CAP_SPIRAM);
    if (!binds) {
      p.disc = D_FAILED;
      strlcpy(p.err, "Out of memory", sizeof(p.err));
      ++s_version;
      return;
    }
    Layout l;
    char light[72] = "";
    uint16_t n = 0;
    bool status = false;
    // Units first, so they can be sorted before trays point into them.
    uint8_t raws[PRN_AMS_UNITS];
    uint8_t nRaw = 0;
    for (size_t i = 0; i < count; i++) {
      if (!strstr(list[i].uid, amsTag.c_str())) continue;
      const char* a = strstr(list[i].extra, "_AMS_");
      if (!a) continue;
      const int num = atoi(a + 5);
      const uint8_t raw = num >= 128 ? num : num - 1;
      bool seen = false;
      for (uint8_t k = 0; k < nRaw; k++) seen |= raws[k] == raw;
      if (!seen && nRaw < PRN_AMS_UNITS && num > 0) raws[nRaw++] = raw;
    }
    std::sort(raws, raws + nRaw);
    l.amsCount = nRaw;
    memcpy(l.id, raws, nRaw);
    for (size_t i = 0; i < count; i++) {
      const HaRegEntry& e = list[i];
      Bind b{};
      strlcpy(b.entity, e.entity, sizeof(b.entity));
      b.scale = 1;
      const char* uid = e.uid;
      const char* ams = strstr(uid, amsTag.c_str());
      const char* ext = strstr(uid, extTag.c_str());
      if (!strncmp(uid, serial.c_str(), serial.length()) && uid[serial.length()] == '_') {
        const char* key = uid + serial.length() + 1;
        const KeyField* k = keyOf(kPrinterKeys, sizeof(kPrinterKeys) / sizeof(kPrinterKeys[0]), key, e.entity);
        if (!k) continue;
        if (k->field == F_LIGHT) strlcpy(light, e.entity, sizeof(light));
        if (k->field == F_LNOZ) l.dual = true;
        if (k->field == F_STATUS) status = true;
        b.field = k->field;
      } else if (ams) {
        const char* a = strstr(e.extra, "_AMS_");
        const int num = a ? atoi(a + 5) : 0;
        const int u = num > 0 ? unitIndex(l, num >= 128 ? num : num - 1) : -1;
        const char* key = strchr(ams + amsTag.length(), '_');
        if (u < 0 || !key) continue;
        key++;
        b.unit = u;
        if (!strncmp(key, "tray_", 5) && !strncmp(e.entity, "sensor.", 7)) {
          const int t = atoi(key + 5);
          if (t < 1 || t > PRN_AMS_SLOTS) continue;
          b.field = F_TRAY;
          b.slot = t - 1;
          l.slots[u] = max<uint8_t>(l.slots[u], t);
        } else {
          const KeyField* k = keyOf(kAmsKeys, sizeof(kAmsKeys) / sizeof(kAmsKeys[0]), key, e.entity);
          if (!k) continue;
          b.field = k->field;
        }
      } else if (ext) {
        if (strcmp(ext + extTag.length(), "external_spool") || strncmp(e.entity, "sensor.", 7)) continue;
        b.field = F_EXT;
      } else {
        continue;
      }
      if (b.field == F_DRY) b.scale = 60;  // hours unless the unit says otherwise
      binds[n++] = b;
    }
    heap_caps_free(p.binds);
    p.binds = binds;
    p.nBinds = n;
    p.layout = l;
    strlcpy(p.light, light, sizeof(p.light));
    if (!status) {
      p.disc = D_FAILED;
      snprintf(p.err, sizeof(p.err), "%s not in Home Assistant", serial.c_str());
    } else {
      p.disc = D_DONE;
      p.err[0] = 0;
      if (p.powered) resetState(p);
    }
    Serial.printf("Printer %s: HA lookup %u entities, %u used, %u AMS\n", p.id.c_str(),
                  (unsigned)count, (unsigned)n, (unsigned)l.amsCount);
    ++s_version;
  }
  syncRaw();
}

// --- Live state -----------------------------------------------------------------

float num(const char* s) {
  char* end = nullptr;
  const float v = s ? strtof(s, &end) : NAN;
  return s && end != s && !*end && isfinite(v) ? v : NAN;
}

bool unusable(const char* s) { return !s || !strcmp(s, "unavailable") || !strcmp(s, "unknown"); }

// "Bambu PLA Basic" -> "PLA Basic", "Generic PETG" -> "PETG", like tray_sub_brands.
void trayName(char* out, size_t n, const char* name) {
  if (!strncmp(name, "Bambu ", 6)) name += 6;
  else if (!strncmp(name, "Generic ", 8)) name += 8;
  strlcpy(out, name, n);
}

void applyTray(PrinterTray& t, bool& active, const char* s, JsonObjectConst a) {
  if (s && !unusable(s)) {
    t.present = strcmp(s, "Empty") != 0;
    if (t.present) trayName(t.type, sizeof(t.type), s);
  }
  if (a.isNull()) return;
  if (a["empty"].is<bool>()) t.present = !a["empty"].as<bool>();
  if (a["active"].is<bool>()) active = a["active"].as<bool>();
  const char* c = a["color"] | "";
  if (*c == '#' && strlen(c) >= 7) {
    char hex[9] = "";
    strlcpy(hex, c + 1, sizeof(hex));
    const uint32_t v = strtoul(hex, nullptr, 16);
    t.rgb = strlen(hex) >= 8 ? v >> 8 : v;
    t.alpha = strlen(hex) >= 8 ? v & 0xFF : 255;
  }
  if (a["remain"].is<int>()) {
    const int r = a["remain"].as<int>();
    t.remain = r >= 0 && r <= 100 ? r : -1;
  }
  if (a["remain_enabled"].is<bool>() && !a["remain_enabled"].as<bool>()) t.remain = -1;
}

void activeTray(Printer& p) {
  PrinterState& s = *p.st;
  s.activeUnit = s.activeSlot = 255;
  if (p.extActive) { s.activeUnit = 254; s.activeSlot = 0; return; }
  for (uint8_t u = 0; u < s.amsCount; u++)
    for (uint8_t t = 0; t < PRN_AMS_SLOTS; t++)
      if (p.trayActive[u][t]) { s.activeUnit = u; s.activeSlot = t; return; }
}

// The integration has no active-nozzle entity: the active nozzle is the one
// whose target (else temperature) the plain nozzle sensors repeat.
void activeNozzle(Printer& p) {
  PrinterState& s = *p.st;
  if (!s.dualNozzle) {
    s.nozzle[0] = p.actTemp;
    s.nozzleTarget[0] = p.actTarget;
    return;
  }
  auto same = [](float a, float b) { return isfinite(a) && isfinite(b) && fabsf(a - b) < 0.5f; };
  const bool l = same(p.actTarget, s.nozzleTarget[1]), r = same(p.actTarget, s.nozzleTarget[0]);
  if (l != r) { s.activeNozzle = l ? 1 : 0; return; }
  const bool lt = same(p.actTemp, s.nozzle[1]), rt = same(p.actTemp, s.nozzle[0]);
  if (lt != rt) s.activeNozzle = lt ? 1 : 0;
}

// print_weight attributes name the AMS slots the job uses ("AMS 1 Tray 2").
void weightMap(PrinterState& s, JsonObjectConst a, bool full, JsonArrayConst removed) {
  auto code = [](const char* k, uint16_t& out) {
    int unit, tray;
    if (sscanf(k, "AMS %d Tray %d", &unit, &tray) != 2 || unit < 1 || tray < 1) return false;
    out = (uint16_t)(((unit - 1) << 8) | (tray - 1));
    return true;
  };
  auto drop = [&](uint16_t c) {
    for (uint8_t i = 0; i < s.mapCount; i++)
      if (s.map[i] == c) { memmove(&s.map[i], &s.map[i + 1], (s.mapCount - i - 1) * sizeof(s.map[0])); s.mapCount--; return; }
  };
  if (full) s.mapCount = 0;
  for (JsonVariantConst k : removed) {
    uint16_t c;
    if (code(k | "", c)) drop(c);
  }
  for (JsonPairConst kv : a) {
    uint16_t c;
    if (!code(kv.key().c_str(), c)) continue;
    drop(c);
    if (kv.value().as<float>() > 0 && s.mapCount < PRN_MAP_MAX) s.map[s.mapCount++] = c;
  }
}

void apply(Printer& p, Bind& b, JsonObjectConst ev, bool full) {
  PrinterState& s = *p.st;
  const char* st = nullptr;
  JsonObjectConst a;
  JsonArrayConst removed;
  if (full) {
    st = ev.isNull() ? "unavailable" : (ev["s"] | "");
    a = ev["a"];
  } else {
    st = ev["+"]["s"];
    a = ev["+"]["a"];
    removed = ev["-"]["a"];
  }
  if (!a.isNull() && (b.field == F_REMAIN || b.field == F_DRY)) {
    const char* u = a["unit_of_measurement"] | "";
    if (!strcmp(u, "h")) b.scale = 60;
    else if (!strcmp(u, "min")) b.scale = 1;
    else if (!strcmp(u, "s")) b.scale = 1.0f / 60;
    else if (!strcmp(u, "d")) b.scale = 1440;
  }
  const float v = num(st);
  switch (b.field) {
    case F_STATUS:
      if (!st) break;
      if (unusable(st) || !strcmp(st, "offline")) s.gcodeState[0] = 0;
      else {
        size_t i = 0;
        for (; st[i] && i + 1 < sizeof(s.gcodeState); i++) s.gcodeState[i] = toupper((unsigned char)st[i]);
        s.gcodeState[i] = 0;
      }
      break;
    case F_STAGE:
      if (!st) break;
      s.stage = -1;
      for (size_t i = 0; i < sizeof(kStages) / sizeof(kStages[0]); i++)
        if (!strcmp(st, kStages[i])) { s.stage = i; break; }
      break;
    case F_PROGRESS: if (st) s.progress = isfinite(v) ? constrain((int)v, 0, 100) : 0; break;
    case F_REMAIN: if (st) s.remainingMin = isfinite(v) && v > 0 ? (uint16_t)lroundf(v * b.scale) : 0; break;
    case F_LAYER: if (st) s.layer = isfinite(v) && v > 0 ? (uint16_t)v : 0; break;
    case F_LAYERS: if (st) s.totalLayers = isfinite(v) && v > 0 ? (uint16_t)v : 0; break;
    case F_TASK: if (st) strlcpy(s.job, unusable(st) ? "" : st, sizeof(s.job)); break;
    case F_SPEED:
      if (!st) break;
      s.speedLevel = !strcmp(st, "silent") ? 1 : !strcmp(st, "standard") ? 2 : !strcmp(st, "sport") ? 3
                   : !strcmp(st, "ludicrous") ? 4 : 0;
      break;
    case F_NOZZLE: if (st) { p.actTemp = v; activeNozzle(p); } break;
    case F_NOZZLE_T: if (st) { p.actTarget = v; activeNozzle(p); } break;
    case F_LNOZ: if (st) { s.nozzle[1] = v; activeNozzle(p); } break;
    case F_LNOZ_T: if (st) { s.nozzleTarget[1] = v; activeNozzle(p); } break;
    case F_RNOZ: if (st) { s.nozzle[0] = v; activeNozzle(p); } break;
    case F_RNOZ_T: if (st) { s.nozzleTarget[0] = v; activeNozzle(p); } break;
    case F_BED: if (st) s.bed = v; break;
    case F_BED_T: if (st) s.bedTarget = v; break;
    case F_CHAMBER: if (st) s.chamber = v; break;
    case F_ONLINE:
      if (!st) break;
      p.online = !strcmp(st, "on");
      s.link = p.online ? PRN_ONLINE : PRN_FAILED;
      strlcpy(s.err, p.online ? "" : "Printer offline", sizeof(s.err));
      break;
    case F_LIGHT: if (st) s.light = !strcmp(st, "on") ? 1 : !strcmp(st, "off") ? 0 : -1; break;
    case F_WEIGHT: weightMap(s, a, full, removed); break;
    case F_HUM: if (st) s.ams[b.unit].humidity = isfinite(v) && v >= 0 && v <= 100 ? (uint8_t)v : 255; break;
    case F_ATEMP: if (st) s.ams[b.unit].temp = v; break;
    case F_DRY: if (st) s.ams[b.unit].dryRemainMin = isfinite(v) && v > 0 ? (uint16_t)lroundf(v * b.scale) : 0; break;
    case F_TRAY:
      applyTray(s.ams[b.unit].tray[b.slot], p.trayActive[b.unit][b.slot], st, a);
      activeTray(p);
      break;
    case F_EXT:
      applyTray(s.ext, p.extActive, st, a);
      s.extPresent = s.ext.present;
      activeTray(p);
      break;
  }
  if (p.online) s.lastReportMs = millis();
}

void hook(const char* id, JsonObjectConst ev, bool full) {
  Lock g;
  for (uint8_t i = 0; i < s_count; i++) {
    Printer& p = s_prn[i];
    if (!p.powered || !p.st || p.disc != D_DONE) continue;
    for (uint16_t b = 0; b < p.nBinds; b++) {
      if (strcmp(p.binds[b].entity, id)) continue;
      apply(p, p.binds[b], ev, full);
      ++s_version;
    }
  }
}

}  // namespace

void printersHaSet(const std::vector<PrinterHaDef>& list) {
  {
    Lock g;
    bool changed = list.size() != s_count;
    for (size_t i = 0; !changed && i < list.size(); i++)
      changed = s_prn[i].id != list[i].id || !s_prn[i].serial.equalsIgnoreCase(list[i].serial);
    if (!changed) return;
    Printer old[PRN_MAX];
    for (uint8_t i = 0; i < s_count; i++) {
      heap_caps_free(s_prn[i].binds);
      freeState(s_prn[i]);
      old[i].id = s_prn[i].id;
      old[i].powered = s_prn[i].powered;
      s_prn[i] = Printer();
    }
    const uint8_t oldCount = s_count;
    s_count = min(list.size(), (size_t)PRN_MAX);
    for (uint8_t i = 0; i < s_count; i++) {
      s_prn[i].id = list[i].id;
      s_prn[i].serial = list[i].serial;
      s_prn[i].serial.toUpperCase();
      for (uint8_t k = 0; k < oldCount; k++)
        if (old[k].id == list[i].id) s_prn[i].powered = old[k].powered;
      if (s_prn[i].powered) resetState(s_prn[i]);
    }
    s_gen++;
    ++s_version;
  }
  syncRaw();
}

void printerHaPower(const String& id, bool on) {
  {
    Lock g;
    Printer* p = find(id);
    if (!p || p->powered == on) return;
    p->powered = on;
    if (on) resetState(*p);
    else freeState(*p);
    ++s_version;
  }
  syncRaw();
}

bool printerHaKnown(const String& id) {
  Lock g;
  return find(id) != nullptr;
}

bool printerHaPowered(const String& id) {
  Lock g;
  const Printer* p = find(id);
  return p && p->powered;
}

bool printerHaRead(const String& id, PrinterState& out) {
  const HaLink link = haLink();
  Lock g;
  const Printer* p = find(id);
  if (!p || !p->powered) return false;
  if (p->st) out = *p->st;
  else out = PrinterState{};
  if (p->disc == D_FAILED) {
    out.link = PRN_FAILED;
    strlcpy(out.err, p->err, sizeof(out.err));
  } else if (p->disc != D_DONE || link != HA_CONNECTED) {
    out.link = PRN_CONNECTING;
    out.err[0] = 0;
  }
  return true;
}

bool printerHaLight(const String& id, bool on) {
  String entity;
  {
    Lock g;
    const Printer* p = find(id);
    if (!p || !p->powered || !p->online || !p->light[0]) return false;
    entity = p->light;
  }
  return haCallService("light", on ? "turn_on" : "turn_off", entity, "");
}

uint32_t printersHaVersion() {
  Lock g;
  return s_version;
}

bool printersHaFind(String& json, String& err) {
  // Printer devices are the integration's devices without a parent (AMS,
  // spools and the hot end rack hang below the printer).
  static const char kFind[] =
      "{% for d in integration_entities('bambu_lab') | map('device_id') | reject('none') | unique %}"
      "{% if not device_attr(d, 'via_device_id') %}"
      "{{ (device_attr(d, 'identifiers') | list | first)[1] }}|"
      "{{ device_attr(d, 'name_by_user') or device_attr(d, 'name') }}|{{ device_attr(d, 'model') }}\n"
      "{% endif %}{% endfor %}";
  String text;
  if (!haRenderTemplate(kFind, text, err)) return false;
  JsonDocument doc;
  JsonArray arr = doc["printers"].to<JsonArray>();
  int at = 0;
  while (at < (int)text.length()) {
    int end = text.indexOf('\n', at);
    if (end < 0) end = text.length();
    const String line = text.substring(at, end);
    at = end + 1;
    const int a = line.indexOf('|'), b = line.indexOf('|', a + 1);
    if (a <= 0 || b < 0) continue;
    JsonObject o = arr.add<JsonObject>();
    String serial = line.substring(0, a);
    serial.trim();
    o["serial"] = serial;
    o["name"] = line.substring(a + 1, b);
    o["model"] = line.substring(b + 1);
  }
  serializeJson(doc, json);
  return true;
}

void printersHaTick() {
  if (haLink() != HA_CONNECTED) return;
  String tmpl;
  uint32_t tag = 0;
  {
    Lock g;
    for (uint8_t i = 0; i < s_count && !tmpl.length(); i++) {
      Printer& p = s_prn[i];
      if (!p.powered || p.disc == D_QUEUED || p.disc == D_DONE) continue;
      if (p.disc == D_FAILED && millis() - p.discAt < RETRY_MS) continue;
      // Entities whose device name carries the serial; the hot end rack has
      // dozens of sensors the deck never shows.
      tmpl = "{% for e in integration_entities('bambu_lab') %}"
             "{% set n = device_attr(device_id(e), 'name') or '' %}"
             "{% if '" + p.serial + "' in n|upper and e.split('.')[0] in ['sensor','binary_sensor','light']"
             " and 'HotendRack' not in n %}{{ e }}|{{ n }}\n{% endif %}{% endfor %}";
      tag = ((s_gen & 0x0FFFFFFF) << 4) | i;
      p.disc = D_QUEUED;
    }
  }
  if (!tmpl.length()) return;
  if (!haRegistryLookup(tmpl, tag, lookupDone)) {
    Lock g;
    const uint8_t idx = tag & 0xF;
    if ((tag >> 4) == (s_gen & 0x0FFFFFFF) && idx < s_count) s_prn[idx].disc = D_NONE;  // busy, next tick
  }
}

}  // namespace deck
