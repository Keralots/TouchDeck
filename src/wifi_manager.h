#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>

void startWiFiDuringSplash();
void initWiFi();
void handleWiFi();
bool isWiFiConnected();
// Applies the timezone and the time source (NTP or Home Assistant).
void configureClock();
bool isAPMode();
String getAPSSID();
uint32_t wifiDisconnectCount();
uint8_t  wifiLastDisconnectReason();

#endif // WIFI_MANAGER_H
