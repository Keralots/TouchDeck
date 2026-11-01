#include "deck_sd.h"
#include "deck_wall.h"
#include "deck_config.h"
#include "settings.h"
#include "config.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <vfs_api.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include <driver/sdspi_host.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <diskio_impl.h>
#include <diskio_sdmmc.h>
#include <ff.h>
#include <ctype.h>
#include <algorithm>

namespace deck {
namespace {

bool s_mounted = false;
const char* s_error = "";  // why an enabled card is not mounted
const char* s_mode = "";   // bus the card mounted on
const char* s_sdmmcError = "";  // why SD mode failed before the SPI fallback
const char* s_format = "";      // result of a format requested before this boot
uint32_t s_indexMs = 0;
fs::FSImplPtr s_impl(new VFSImpl());
fs::FS s_fs(s_impl);
StaticSemaphore_t s_mutexBuffer;
SemaphoreHandle_t s_mutex = nullptr;
WallFiles s_index[2][2];
uint64_t s_protected[2][8];

struct Lock {
  Lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_mutex); }
};

void sortFiles(WallFiles& files) {
  std::sort(files.begin(), files.end(), [](const WallFile& a, const WallFile& b) { return a.id < b.id; });
}

bool metadata(bool p, bool d, const String& name, WallFile& file) {
  const String path = String("/sd") + wallDir(p, d) + "/" + name;
  struct stat st;
  if (stat(path.c_str(), &st) || !S_ISREG(st.st_mode)) return false;
  file = {wallId(name) | (uint64_t)d << 32, (size_t)st.st_size, (uint32_t)st.st_mtime};
  return true;
}

void indexErase(bool p, bool d, uint64_t id) {
  auto& files = s_index[p][d];
  files.erase(std::remove_if(files.begin(), files.end(),
                            [id](const WallFile& f) { return f.id == id; }), files.end());
}

void indexChanged(bool p, bool d, const String& name) {
  indexErase(p, d, wallId(name) | (uint64_t)d << 32);
  WallFile file;
  if (metadata(p, d, name, file)) {
    s_index[p][d].push_back(file);
    sortFiles(s_index[p][d]);
  }
}

// A card left mid-transfer by a warm reset can miss the first init.
constexpr int SD_MOUNT_TRIES = 3;

// Slot wiring: CLK 12, CMD/MOSI 11, DAT0/MISO 13, DAT3/CS 10.
esp_vfs_fat_mount_config_t mountConfig() {
  esp_vfs_fat_mount_config_t mc = {};
  mc.format_if_mount_failed = false;
  mc.max_files = 8;  // brand icon sizes hold files open next to the gallery and portal ones
  mc.allocation_unit_size = 16 * 1024;
  return mc;
}

// 1-bit SD mode, set up as the vendor's SD_MMC.setPins(12, 11, 13) demo:
// DAT3 and the pull-ups are left to the board.
void sdmmcConfig(sdmmc_host_t& host, sdmmc_slot_config_t& slot) {
  host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_1BIT;
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;
  slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;
  slot.clk = GPIO_NUM_12;
  slot.cmd = GPIO_NUM_11;
  slot.d0 = GPIO_NUM_13;
  // S3 defaults put D2/D3 on GPIO 12/13, this slot's CLK and D0.
  slot.d1 = slot.d2 = slot.d3 = GPIO_NUM_NC;
  slot.d4 = slot.d5 = slot.d6 = slot.d7 = GPIO_NUM_NC;
}

esp_err_t mountSdmmc(sdmmc_card_t** card) {
  sdmmc_host_t host;
  sdmmc_slot_config_t slot;
  sdmmcConfig(host, slot);
  const esp_vfs_fat_mount_config_t mc = mountConfig();
  return esp_vfs_fat_sdmmc_mount("/sd", &host, &slot, &mc, card);
}

esp_err_t mountSpi(sdmmc_card_t** card) {
  spi_bus_config_t bus = {};
  bus.mosi_io_num = 11;
  bus.miso_io_num = 13;
  bus.sclk_io_num = 12;
  bus.quadwp_io_num = bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 4096;
  esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) return err;
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SPI3_HOST;
  host.max_freq_khz = 20000;
  sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
  dev.gpio_cs = GPIO_NUM_10;
  dev.host_id = SPI3_HOST;
  const esp_vfs_fat_mount_config_t mc = mountConfig();
  err = esp_vfs_fat_sdspi_mount("/sd", &host, &dev, &mc, card);
  if (err != ESP_OK) spi_bus_free(SPI3_HOST);
  return err;
}

// Driver error lines logged while mounting, for the portal (#2).
char* s_log = nullptr;
size_t s_logLen = 0;
constexpr size_t SD_LOG_SIZE = 1024;
size_t s_logSection = 0;  // dedupe only within the current "SD mode:" / "SPI:" block
portMUX_TYPE s_logMux = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t s_prevLog = nullptr;

void logLine(const char* text, bool section = false) {
  const size_t len = strlen(text);
  portENTER_CRITICAL(&s_logMux);
  if (section) s_logSection = s_logLen;
  if ((section || !strstr(s_log + s_logSection, text)) && s_logLen + len + 2 < SD_LOG_SIZE) {
    memcpy(s_log + s_logLen, text, len);
    s_logLen += len;
    s_log[s_logLen++] = '\n';
    s_log[s_logLen] = 0;
  }
  portEXIT_CRITICAL(&s_logMux);
}

// Keeps each distinct line once, without the "E (352) " prefix.
int captureLog(const char* fmt, va_list args) {
  char line[160];
  va_list copy;
  va_copy(copy, args);
  int n = vsnprintf(line, sizeof(line), fmt, copy);
  va_end(copy);
  n = std::min<int>(n, sizeof(line) - 1);
  while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
  line[std::max(n, 0)] = 0;
  const char* text = strstr(line, ") ");
  text = text && text - line < 16 ? text + 2 : line;
  if (*text && (strstr(text, "sdmmc") || strstr(text, "sdspi") || strstr(text, "vfs_fat"))) logLine(text);
  return s_prevLog ? s_prevLog(fmt, args) : vprintf(fmt, args);
}

void logBegin() {
  if (!s_log) s_log = (char*)heap_caps_calloc(1, SD_LOG_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (s_log) s_prevLog = esp_log_set_vprintf(captureLog);
}

void logEnd() {
  if (s_log) esp_log_set_vprintf(s_prevLog);
}

void logf(const char* fmt, ...) {
  if (!s_log) return;
  char line[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  logLine(line);
  Serial.printf("Deck: SD %s\n", line);
}

// The card in SD mode with FatFs but without the VFS, for the probe and
// the format.
struct RawCard {
  sdmmc_card_t card = {};
  BYTE pdrv = 0xFF;
  bool host = false;
  char drv[3] = {0};

  esp_err_t open() {
    sdmmc_host_t h;
    sdmmc_slot_config_t slot;
    sdmmcConfig(h, slot);
    esp_err_t err = h.init();
    if (err != ESP_OK) return err;
    host = true;
    if ((err = sdmmc_host_init_slot(h.slot, &slot)) != ESP_OK) return err;
    if ((err = sdmmc_card_init(&h, &card)) != ESP_OK) return err;
    if ((err = ff_diskio_get_drive(&pdrv)) != ESP_OK) return err;
    ff_diskio_register_sdmmc(pdrv, &card);
    drv[0] = '0' + pdrv;
    drv[1] = ':';
    return ESP_OK;
  }
  ~RawCard() {
    if (pdrv != 0xFF) ff_diskio_unregister(pdrv);
    if (host) sdmmc_host_deinit();
  }
};

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t le16(const uint8_t* p) { return p[0] | p[1] << 8; }

// Why FatFs refused a card that answers: partition table, boot sector
// fields and the f_mount result (#2).
void probeCard() {
  logLine("Probe:", true);
  RawCard* rc = new RawCard();
  uint8_t* buf = (uint8_t*)heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  FATFS* fs = (FATFS*)heap_caps_calloc(1, sizeof(FATFS), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  esp_err_t err = buf && fs ? rc->open() : ESP_ERR_NO_MEM;
  if (err != ESP_OK) {
    logf("card open %s", esp_err_to_name(err));
  } else if ((err = sdmmc_read_sectors(&rc->card, buf, 0, 1)) != ESP_OK) {
    logf("read sector 0 %s", esp_err_to_name(err));
  } else {
    logf("card %llu sectors of %u B", (unsigned long long)rc->card.csd.capacity,
         (unsigned)rc->card.csd.sector_size);
    uint32_t vbr = 0;
    if (buf[0] == 0xEB || buf[0] == 0xE9 || buf[0] == 0xE8) {
      logf("sector 0: boot sector, no partition table");
    } else {
      logf("sector 0: signature %04X", le16(buf + 510));
      for (int i = 0; i < 4; i++) {
        const uint8_t* e = buf + 446 + 16 * i;
        if (!e[4]) continue;
        logf("partition %d: status %02X type %02X start %u size %u", i + 1, e[0], e[4],
             (unsigned)le32(e + 8), (unsigned)le32(e + 12));
        if (!vbr) vbr = le32(e + 8);
      }
    }
    if (vbr && (err = sdmmc_read_sectors(&rc->card, buf, vbr, 1)) != ESP_OK) {
      logf("read sector %u %s", (unsigned)vbr, esp_err_to_name(err));
    } else if (vbr || buf[0] == 0xEB || buf[0] == 0xE9 || buf[0] == 0xE8) {
      char oem[9] = {0}, type16[9] = {0}, type32[9] = {0};
      for (int i = 0; i < 8; i++) {
        oem[i] = isprint(buf[3 + i]) ? buf[3 + i] : '.';
        type16[i] = isprint(buf[54 + i]) ? buf[54 + i] : '.';
        type32[i] = isprint(buf[82 + i]) ? buf[82 + i] : '.';
      }
      logf("boot: jump %02X OEM '%s' bytes/sec %u sec/clus %u reserved %u FATs %u", buf[0], oem,
           le16(buf + 11), buf[13], le16(buf + 14), buf[16]);
      logf("boot: root %u total16 %u total32 %u FAT16 size %u FAT32 size %u", le16(buf + 17),
           le16(buf + 19), (unsigned)le32(buf + 32), le16(buf + 22), (unsigned)le32(buf + 36));
      logf("boot: type '%s' / '%s' signature %04X", type16, type32, le16(buf + 510));
    }
    logf("f_mount result %d", (int)f_mount(fs, rc->drv, 1));
    f_mount(nullptr, rc->drv, 0);
  }
  delete rc;
  free(buf);
  free(fs);
}

// One FAT partition over the whole card, as esp_vfs_fat formats it.
const char* formatCard() {
  static char result[48];
  logLine("Format:", true);
  RawCard* rc = new RawCard();
  void* work = heap_caps_malloc(FF_MAX_SS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  esp_err_t err = work ? rc->open() : ESP_ERR_NO_MEM;
  if (err != ESP_OK) {
    snprintf(result, sizeof(result), "card open %s", esp_err_to_name(err));
  } else {
    const LBA_t parts[] = {100, 0, 0, 0};
    FRESULT res = f_fdisk(rc->pdrv, parts, work);
    if (res != FR_OK) {
      snprintf(result, sizeof(result), "f_fdisk %d", (int)res);
    } else {
      const MKFS_PARM opt = {FM_ANY, 2, 0, 0, 16 * 1024};
      res = f_mkfs(rc->drv, &opt, work, FF_MAX_SS);
      if (res != FR_OK) snprintf(result, sizeof(result), "f_mkfs %d", (int)res);
      else strlcpy(result, "OK", sizeof(result));
    }
  }
  logf("%s", result);
  delete rc;
  free(work);
  return result;
}

// All SD mode tries first: once SPI has addressed the card, SD mode times
// out until a power cycle, so interleaving hid the real error.
sdmmc_card_t* mountCard() {
  static char error[96];
  sdmmc_card_t* card = nullptr;
  esp_err_t mmcErr = ESP_OK, spiErr = ESP_OK;
  if (s_log) logLine("SD mode:", true);
  for (int i = 0; i < SD_MOUNT_TRIES && !card; i++) {
    if (i) delay(250);
    const esp_err_t err = mountSdmmc(&card);
    if (err != ESP_OK && mmcErr == ESP_OK) mmcErr = err;
  }
  const bool sdMode = card;
  if (sdMode) {
    s_mode = "SD 1-bit";
  } else if (mmcErr == ESP_FAIL) {
    // The card answered but FatFs refused it; SPI cannot help a card
    // already in SD mode.
    if (s_log) probeCard();
  } else {
    if (s_log) logLine("SPI:", true);
    for (int i = 0; i < SD_MOUNT_TRIES && !card; i++) {
      if (i) delay(250);
      const esp_err_t err = mountSpi(&card);
      if (err != ESP_OK && spiErr == ESP_OK) spiErr = err;
    }
    if (card) s_mode = "SPI";
  }
  s_sdmmcError = sdMode ? "" : esp_err_to_name(mmcErr);
  if (!card && mmcErr == ESP_FAIL) {
    snprintf(error, sizeof(error), "SD %s: file system not accepted", esp_err_to_name(mmcErr));
    s_error = error;
    Serial.printf("Deck: SD mount failed: %s\n", s_error);
    return nullptr;
  }
  if (!card) {
    snprintf(error, sizeof(error), "SD %s, SPI %s", esp_err_to_name(mmcErr), esp_err_to_name(spiErr));
    s_error = error;
    Serial.printf("Deck: SD mount failed: %s\n", s_error);
    return nullptr;
  }
  s_error = "";
  Serial.printf("Deck: SD card %s, %s, %llu MB\n", card->cid.name, s_mode,
                (unsigned long long)card->csd.capacity * card->csd.sector_size / 1048576);
  return card;
}

}  // namespace

bool wallOnSd() { return s_mounted; }
const char* wallSdError() { return s_error; }
const char* wallSdMode() { return s_mode; }
const char* wallSdmmcError() { return s_sdmmcError; }
const char* wallSdLog() { return s_log ? s_log : ""; }
const char* wallSdFormatResult() { return s_format; }

bool wallSdFormatRequest() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  const bool ok = prefs.putBool("sdfmt", true);
  prefs.end();
  return ok;
}
uint32_t wallIndexMs() { return s_indexMs; }
fs::FS& wallFs() {
  if (s_mounted) return s_fs;
  deckFsReady();
  return LittleFS;
}
const char* wallRoot() { return s_mounted ? "/touchdeck/wall" : "/wall"; }
const char* wallTemp(bool daily) {
  return s_mounted ? (daily ? "/touchdeck/wall/daily.up" : "/touchdeck/wall/upload.up")
                   : (daily ? "/wall/daily.up" : "/wall.up");
}

const char* wallDir(bool portrait, bool daily) {
  if (s_mounted)
    return daily ? (portrait ? "/touchdeck/wall/dp" : "/touchdeck/wall/dl")
                 : (portrait ? "/touchdeck/wall/p" : "/touchdeck/wall/l");
  return daily ? (portrait ? "/wall/dp" : "/wall/dl") : (portrait ? "/wall/p" : "/wall/l");
}

uint64_t wallFreeBytes() {
  if (!s_mounted) return deckFsReady() ? LittleFS.totalBytes() - LittleFS.usedBytes() : 0;
  uint64_t total = 0, free = 0;
  return esp_vfs_fat_info("/sd", &total, &free) == ESP_OK ? free : 0;
}

uint64_t wallTotalBytes() {
  if (!s_mounted) return deckFsReady() ? LittleFS.totalBytes() : 0;
  uint64_t total = 0, free = 0;
  return esp_vfs_fat_info("/sd", &total, &free) == ESP_OK ? total : 0;
}

void wallStorageBegin() {
  s_mutex = xSemaphoreCreateMutexStatic(&s_mutexBuffer);
  for (auto& orientation : s_protected)
    for (auto& id : orientation) id = UINT64_MAX;
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    const char* keys[2][3] = {{"wpin_l", "wpin_dl", "wpin_bl"}, {"wpin_p", "wpin_dp", "wpin_bp"}};
    for (int p = 0; p < 2; p++)
      for (int source = 0; source < 3; source++) {
        String name = prefs.getString(keys[p][source], "");
        if (source == 1 && wallValidName(name)) name = "d/" + name;
        if (wallValidRef(name)) s_protected[p][4 + source] = wallId(name);
      }
    prefs.end();
  }
  if (!dispSettings.sdCard) return;
  bool format = false;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    format = prefs.getBool("sdfmt", false);
    if (format) prefs.remove("sdfmt");  // cleared first: a crash must not format again
    prefs.end();
  }
  logBegin();
  if (format) s_format = formatCard();
  sdmmc_card_t* card = mountCard();
  logEnd();
  if (!card) return;
  s_impl->mountpoint("/sd");
  s_mounted = true;
  bool ok = (s_fs.exists("/touchdeck") || s_fs.mkdir("/touchdeck")) &&
            (s_fs.exists(wallRoot()) || s_fs.mkdir(wallRoot()));
  for (int p = 0; p < 2; p++)
    for (int d = 0; d < 2; d++) ok &= s_fs.exists(wallDir(p, d)) || s_fs.mkdir(wallDir(p, d));
  if (!ok) {
    s_error = "gallery folders unavailable";
    Serial.println("Deck: SD gallery directories unavailable");
    s_mounted = false;
    s_impl->mountpoint(nullptr);
    esp_vfs_fat_sdcard_unmount("/sd", card);
    if (!strcmp(s_mode, "SPI")) spi_bus_free(SPI3_HOST);
    s_mode = "";
    return;
  }
  wallRebuildIndex();
  Serial.println("Deck: SD galleries mounted");
}

void wallRebuildIndex() {
  if (!s_mounted) return;
  Lock lock;
  const uint32_t start = millis();
  for (int p = 0; p < 2; p++) {
    for (int d = 0; d < 2; d++) {
      WallFiles files;
      const String path = String("/sd") + wallDir(p, d);
      DIR* dir = opendir(path.c_str());
      if (!dir) continue;
      while (struct dirent* entry = readdir(dir)) {
        if (!wallValidName(entry->d_name)) continue;
        WallFile file;
        if (metadata(p, d, entry->d_name, file)) files.push_back(file);
      }
      closedir(dir);
      sortFiles(files);
      s_index[p][d].swap(files);
    }
  }
  s_indexMs = millis() - start;
}

WallFiles wallListDir(const char* path) {
  if (s_mounted) {
    Lock lock;
    for (int p = 0; p < 2; p++)
      for (int d = 0; d < 2; d++)
        if (!strcmp(path, wallDir(p, d))) return s_index[p][d];
    return {};
  }
  WallFiles files;
  if (!deckFsReady()) return files;
  File dir = LittleFS.open(path);
  if (!dir || !dir.isDirectory()) return files;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String name = f.name();
    name = name.substring(name.lastIndexOf('/') + 1);
    if (!f.isDirectory() && wallValidName(name))
      files.push_back({wallId(name), f.size(), (uint32_t)f.getLastWrite()});
  }
  sortFiles(files);
  return files;
}

void wallIndexChanged(bool p, bool d, const String& name) {
  if (!s_mounted) return;
  Lock lock;
  indexChanged(p, d, name);
}

void wallProtect(bool p, int slot, const String& name) {
  Lock lock;
  s_protected[p][slot] = wallValidRef(name) ? wallId(name) : UINT64_MAX;
}

bool wallRemoveFile(bool p, bool d, const String& name, bool protect) {
  Lock lock;
  const uint64_t id = wallId(name) | (uint64_t)d << 32;
  if (protect)
    for (uint64_t kept : s_protected[p]) if (kept == id) return false;
  const String path = String(wallDir(p, d)) + "/" + name;
  // Each FAT lookup scans the directory: skip the stat a re-index would do.
  if (!wallFs().remove(path)) return false;
  if (s_mounted) indexErase(p, d, id);
  return true;
}

}  // namespace deck
