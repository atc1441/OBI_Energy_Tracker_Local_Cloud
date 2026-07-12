// gateway_web.h — WiFi config portal (WiFiManager) + web dashboard + MQTT client.
// Runs alongside the LoRa gateway; both are serviced from loop().
#pragma once

void web_setup();          // bring up WiFi (captive portal on first run), the web server and MQTT
void web_loop();           // service HTTP + MQTT (non-blocking); call every loop()
void web_factory_reset();  // wipe saved WiFi (WiFiManager) + MQTT settings; caller reboots afterwards
bool web_wifi_connected(); // true when joined to a WiFi network (drives the status LED)
bool web_night_mode();     // true when normal-operation LED activity is disabled
