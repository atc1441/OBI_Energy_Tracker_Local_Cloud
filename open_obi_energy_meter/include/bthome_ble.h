#pragma once
#include <stdint.h>

struct Reader;
class WebServer;

#ifdef OBI_BTHOME
void bthome_init(WebServer *server);
void bthome_capture(const Reader &reader);
void bthome_forget(const uint8_t handle[3]);
void bthome_service();
void bthome_status();
void bthome_configure();
void bthome_reset();
#else
inline void bthome_init(WebServer *) {}
inline void bthome_capture(const Reader &) {}
inline void bthome_forget(const uint8_t *) {}
inline void bthome_service() {}
inline void bthome_reset() {}
#endif
