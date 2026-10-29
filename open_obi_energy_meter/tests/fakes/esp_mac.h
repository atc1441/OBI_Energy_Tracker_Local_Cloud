#pragma once
#include <string.h>
#include <stdint.h>
#define ESP_OK 0
#define ESP_MAC_BT 2
inline int esp_read_mac(uint8_t *mac, int) {
  const uint8_t address[] = {0x54, 0x48, 0xE6, 0x8F, 0x80, 0xA5};
  memcpy(mac, address, 6); return 0;
}
