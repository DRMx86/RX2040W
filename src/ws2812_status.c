#include "ws2812_status.h"

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "ws2812_status.pio.h"

#ifndef WS2812_PIN
#define WS2812_PIN 16
#endif

#ifndef WS2812_PIXEL_COUNT
#define WS2812_PIXEL_COUNT 1
#endif

#ifndef WS2812_STATUS_ENABLED
#define WS2812_STATUS_ENABLED 1
#endif

#ifndef WS2812_PIO
#define WS2812_PIO 1
#endif

#define WS2812_FREQ_HZ 800000

#if WS2812_PIO == 0
static PIO _pio = pio0;
#elif WS2812_PIO == 1
static PIO _pio = pio1;
#else
#error WS2812_PIO must be 0 or 1
#endif
static uint _sm = 0;
static bool _ready = false;
static bool _enabled = WS2812_STATUS_ENABLED;

static inline uint32_t rgb_to_grb(uint8_t red, uint8_t green, uint8_t blue) {
    return ((uint32_t) green << 24u) | ((uint32_t) red << 16u) | ((uint32_t) blue << 8u);
}

static void write_pixel(uint32_t grb) {
    pio_sm_put_blocking(_pio, _sm, grb);
}

void ws2812_status_init(void) {
    if (!_enabled) return;

    uint offset = pio_add_program(_pio, &ws2812_status_program);
    _sm = pio_claim_unused_sm(_pio, true);

    pio_sm_config config = ws2812_status_program_get_default_config(offset);
    sm_config_set_sideset_pins(&config, WS2812_PIN);
    sm_config_set_out_shift(&config, false, true, 24);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);

    const float divider = (float) clock_get_hz(clk_sys) / (WS2812_FREQ_HZ * (ws2812_status_T1 + ws2812_status_T2 + ws2812_status_T3));
    sm_config_set_clkdiv(&config, divider);

    pio_gpio_init(_pio, WS2812_PIN);
    pio_sm_set_consecutive_pindirs(_pio, _sm, WS2812_PIN, 1, true);
    pio_sm_init(_pio, _sm, offset, &config);
    pio_sm_set_enabled(_pio, _sm, true);

    _ready = true;
    ws2812_status_off();
}

static void set_rgb_force(uint8_t red, uint8_t green, uint8_t blue) {
    if (!_ready) return;

    const uint32_t grb = rgb_to_grb(red, green, blue);
    for (uint i = 0; i < WS2812_PIXEL_COUNT; ++i) {
        write_pixel(grb);
    }
}

void ws2812_status_set_rgb(uint8_t red, uint8_t green, uint8_t blue) {
    if (!_enabled) return;
    set_rgb_force(red, green, blue);
}

void ws2812_status_set_packed(uint32_t rgb) {
    ws2812_status_set_rgb((rgb >> 16u) & 0xffu, (rgb >> 8u) & 0xffu, rgb & 0xffu);
}

void ws2812_status_set_enabled(bool enabled) {
    _enabled = enabled;
    if (!enabled) {
        set_rgb_force(0, 0, 0);
    }
}

void ws2812_status_off(void) {
    ws2812_status_set_rgb(0, 0, 0);
}
