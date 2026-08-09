#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"

#include "bt.h"
#include <btstack_run_loop.h>
#include <btstack_run_loop.h>
#include <btstack_run_loop.h>
#include <btstack_run_loop.h>


// Unrecoverable error happened. Reboot by setting watchdog.
// Blink led until watchdog fires
// If RUN_PIN is defined then try reset via run pin after 5 blinks
void fatal() {
    watchdog_enable(1000, true);  // reboot in 1s
    #ifdef RUN_PIN
        unsigned count = 0;
    #endif
    while(true) {  // blink until reboot
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
        sleep_ms(20);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
        sleep_ms(80);
        #ifdef RUN_PIN
            if (++count >= 5) {
                // Pull run pin low, to reset the pico
                gpio_init(RUN_PIN);
                gpio_set_dir(RUN_PIN, GPIO_OUT);
                gpio_put(RUN_PIN, (count & 1) ? false : true);
            }
        #endif
    }
}


// --- LED state machine ---
static btstack_timer_source_t _blink_timer;
static bool _led_state  = false;
static bool _connected  = false;

static void blink_handler(btstack_timer_source_t *ts) {
    if (_connected) return;          // stop blinking once connected
    _led_state = !_led_state;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, _led_state);
    btstack_run_loop_set_timer(ts, 250);  // toggle every 250ms
    btstack_run_loop_add_timer(ts);
}

// Called from a2dp.c on connect / disconnect
void led_connected(bool connected) {
    _connected = connected;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, connected);
}

void on_bt_up( void *arg ) {
    printf("Bluetooth stack is up\n");
    // Start blinking to show we are in pairing mode
    _connected = false;
    btstack_run_loop_set_timer_handler(&_blink_timer, blink_handler);
    btstack_run_loop_set_timer(&_blink_timer, 200);
    btstack_run_loop_add_timer(&_blink_timer);
}


int main() {
    stdio_init_all();

    // initialize CYW43 driver architecture (will enable BT if/because CYW43_ENABLE_BLUETOOTH == 1)
    if (cyw43_arch_init()) {
        printf("Failed to init cyw43_arch\n");
        fatal();
        return -1;
    }

    // led on during setup until bt is up
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);

    bt_begin(BT_NAME, BT_PIN, on_bt_up, NULL);

    printf("Setup done\n");
    bt_run();

    fatal();
    return -2;
}
