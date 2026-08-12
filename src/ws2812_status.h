#ifndef ws2812_status_h
#define ws2812_status_h

#include <stdbool.h>
#include <stdint.h>

void ws2812_status_init(void);
void ws2812_status_set_rgb(uint8_t red, uint8_t green, uint8_t blue);
void ws2812_status_set_packed(uint32_t rgb);
void ws2812_status_set_enabled(bool enabled);
void ws2812_status_off(void);

#endif
