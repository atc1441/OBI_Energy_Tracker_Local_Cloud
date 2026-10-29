#pragma once
#include "esp_mac.h"
#define ESP_BT_MODE_BTDM 3
extern unsigned testBtReleases;
inline int esp_bt_mem_release(int mode) {
  if (mode != ESP_BT_MODE_BTDM) return -1;
  ++testBtReleases;
  return ESP_OK;
}
