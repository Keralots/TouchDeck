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
#include <algorithm>

namespace deck {
namespace {

bool s_mounted = false;
const char* s_error = "";  // why an enabled card is not mounted
const char* s_mode = "";   // bus the card mounted on
const char* s_sdmmcError = "";  // why SD mode failed before the SPI fallback
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

// 1-bit SD mode, as the board's factory firmware uses.
esp_err_t mountSdmmc(sdmmc_card_t** card) {
  // DAT3 high at power-up keeps the card out of SPI mode.
  gpio_reset_pin(GPIO_NUM_10);
  gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_10, 1);
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_1BIT;
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;
  slot.clk = GPIO_NUM_12;
  slot.cmd = GPIO_NUM_11;
  slot.d0 = GPIO_NUM_13;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
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

// SD mode first; SPI stays as the fallback that worked before (#2).
// A card leaves SPI mode (and enters it from SD mode) only after a power cycle.
sdmmc_card_t* mountCard() {
  static char error[64];
  sdmmc_card_t* card = nullptr;
  const esp_err_t mmcErr = mountSdmmc(&card);
  s_sdmmcError = mmcErr == ESP_OK ? "" : esp_err_to_name(mmcErr);
  if (mmcErr == ESP_OK) {
    s_mode = "SD 1-bit";
  } else {
    const esp_err_t spiErr = mountSpi(&card);
    if (spiErr != ESP_OK) {
      snprintf(error, sizeof(error), "SD %s, SPI %s", esp_err_to_name(mmcErr), esp_err_to_name(spiErr));
      s_error = error;
      Serial.printf("Deck: SD mount failed: %s\n", s_error);
      return nullptr;
    }
    s_mode = "SPI";
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
  sdmmc_card_t* card = nullptr;
  for (int attempt = 1; attempt <= SD_MOUNT_TRIES && !card; attempt++) {
    if (attempt > 1) delay(250);
    card = mountCard();
  }
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
