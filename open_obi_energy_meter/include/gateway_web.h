// gateway_web.h — WiFi config portal (WiFiManager) + web dashboard + MQTT client.
// Runs alongside the LoRa gateway; both are serviced from loop().
#pragma once

void web_setup();   // bring up WiFi (captive portal on first run), the web server and MQTT
void web_loop();    // service HTTP + MQTT (non-blocking); call every loop()
