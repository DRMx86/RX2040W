#include "avrcp.h"
#include <stdio.h>
#include "btstack.h"

// --- Watchdog config ---
#define WATCHDOG_INTERVAL_MS      8000
#define METADATA_STALE_THRESHOLD  15000
#define STALE_RESET_CYCLES        2      // [FIX 3] cycles before re-register

static uint16_t _cid   = 0;
static bool     _playing = false;
static uint8_t  _volume  = 0;

// --- Bluetooth state variables ---
static bt_state_t _bt_state   = BT_STATE_UNPAIRED;
static bd_addr_t  _remote_addr;
static bool       _ever_paired = false;
static uint8_t    _play_status = 0;


// Watchdog state
static btstack_timer_source_t _watchdog_timer;
static uint32_t _last_metadata_ms   = 0;
static bool     _notifications_enabled = false;
static bool     _metadata_pending    = false; // [FIX 4]
static uint8_t  _stale_count         = 0;     // [FIX 3]


static void avrcp_volume_changed(uint8_t volume) {
    const btstack_audio_sink_t *audio = btstack_audio_sink_get_instance();
    if (audio) audio->set_volume(volume);
}

static void watchdog_start(void);

static void bt_state_set(bt_state_t s) {
    if (_bt_state == s) return;
    _bt_state = s;
    switch (s) {
        case BT_STATE_UNPAIRED:     printf("BT:STATE:UNPAIRED\n");     break;
        case BT_STATE_PAIRING:      printf("BT:STATE:PAIRING\n");      break;
        case BT_STATE_PAIRED:       printf("BT:STATE:PAIRED\n");       break;
        case BT_STATE_CONNECTED:    printf("BT:STATE:CONNECTED\n");    break;
        case BT_STATE_DISCONNECTED: printf("BT:STATE:DISCONNECTED\n"); break;
    }
}
  // forward declaration


// [FIX 1] Always remove timer before re-adding to prevent stacking
static void watchdog_callback(btstack_timer_source_t *ts) {
    btstack_run_loop_remove_timer(&_watchdog_timer); // [FIX 1]
    if (_cid == 0) return;

    uint32_t now = btstack_run_loop_get_time_ms();

    if (!_notifications_enabled) {
        printf("WATCHDOG: Re-enabling notifications\n");
        avrcp_controller_enable_notification(_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
        avrcp_controller_enable_notification(_cid, AVRCP_NOTIFICATION_EVENT_NOW_PLAYING_CONTENT_CHANGED);
        avrcp_controller_enable_notification(_cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
        _notifications_enabled = true;   // [FIX 5] assume success – stale check will catch real failures
        _stale_count = 0;
    }

    if (_playing) {
        // [FIX 2] Only check staleness — never update _last_metadata_ms here
        uint32_t silence = (_last_metadata_ms == 0)
            ? METADATA_STALE_THRESHOLD + 1
            : (now - _last_metadata_ms);

        if (silence > METADATA_STALE_THRESHOLD) {
            _stale_count++;
            printf("WATCHDOG: Metadata stale (%lu ms) [cycle %d]\n",
                   silence, _stale_count);

            // [FIX 3] Force notification re-register after N stale cycles
            if (_stale_count >= STALE_RESET_CYCLES) {
                printf("WATCHDOG: Forcing notification re-register\n");
                _notifications_enabled = false;
                _stale_count = 0;
            }

            // [FIX 2] Re-request only — timestamp updated on actual receipt
            avrcp_controller_get_now_playing_info(_cid);
        }
    }

    watchdog_start();
}


static void watchdog_start(void) {
    btstack_run_loop_set_timer(&_watchdog_timer, WATCHDOG_INTERVAL_MS);
    btstack_run_loop_set_timer_handler(&_watchdog_timer, watchdog_callback);
    btstack_run_loop_add_timer(&_watchdog_timer);
}


static void watchdog_stop(void) {
    btstack_run_loop_remove_timer(&_watchdog_timer);
}


// --- Helper: timestamp only on confirmed receipt ---

static void request_play_status(void) {
    if (_cid == 0) return;
    avrcp_controller_get_play_status(_cid);
}

static void metadata_received(void) {        // [FIX 4]
    _last_metadata_ms = btstack_run_loop_get_time_ms();
    _metadata_pending = false;
    _stale_count      = 0;
}



static btstack_packet_callback_registration_t _hci_event_cb;

static void hci_packet_handler(uint8_t packet_type, uint16_t channel,
                                uint8_t *packet, uint16_t size) {
    UNUSED(channel); UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_PIN_CODE_REQUEST: {
            bd_addr_t addr;
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            printf("BT:PAIR_PIN:%s\n", bd_addr_to_str(addr));
            bt_state_set(BT_STATE_PAIRING);
            gap_pin_code_response(addr, "0000");
            break;
        }
        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            bd_addr_t addr;
            hci_event_user_confirmation_request_get_bd_addr(packet, addr);
            uint32_t passkey = little_endian_read_32(packet, 8);
            printf("BT:PAIR_SSP:%s:PASSKEY:%06lu\n", bd_addr_to_str(addr), (unsigned long)passkey);
            bt_state_set(BT_STATE_PAIRING);
            gap_ssp_confirmation_response(addr);
            break;
        }
        case HCI_EVENT_AUTHENTICATION_COMPLETE_EVENT: {
            uint8_t status = hci_event_authentication_complete_get_status(packet);
            if (status == ERROR_CODE_SUCCESS) {
                _ever_paired = true;
                bt_state_set(BT_STATE_PAIRED);
                printf("BT:PAIR_OK\n");
            } else {
                printf("BT:PAIR_FAIL:0x%02X\n", status);
                bt_state_set(BT_STATE_UNPAIRED);
            }
            break;
        }
        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            if (_cid == 0) {
                bt_state_set(_ever_paired ? BT_STATE_DISCONNECTED : BT_STATE_UNPAIRED);
            }
            break;
        }
        default: break;
    }
}

static void connection_handler(uint8_t packet_type, uint16_t channel,
                               uint8_t *packet, uint16_t size) {
    UNUSED(channel); UNUSED(size);
    uint16_t cid; uint8_t status;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    switch (packet[2]) {
                case AVRCP_SUBEVENT_PLAY_STATUS: {
            _play_status = avrcp_subevent_play_status_get_play_status(packet);
            printf("PLAY_STATUS: %d\n", _play_status);
            metadata_received();
            break;
        }
case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED:
            cid    = avrcp_subevent_connection_established_get_avrcp_cid(packet);
            status = avrcp_subevent_connection_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) { _cid = 0; return; }
            _cid                   = cid;
            _notifications_enabled = false;
            _last_metadata_ms      = 0;
            _metadata_pending      = false;   // [FIX 4]
            _stale_count           = 0;       // [FIX 3]
            avrcp_target_support_event(cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
            avrcp_target_support_event(cid, AVRCP_NOTIFICATION_EVENT_BATT_STATUS_CHANGED);
            avrcp_target_battery_status_changed(cid, AVRCP_BATTERY_STATUS_EXTERNAL);
            avrcp_controller_get_supported_events(cid);
            watchdog_start();
            return;

        case AVRCP_SUBEVENT_CONNECTION_RELEASED:
            watchdog_stop();
            _cid                   = 0;
            _playing               = false;
            _notifications_enabled = false;
            _last_metadata_ms      = 0;
            _metadata_pending      = false;   // [FIX 4]
            _stale_count           = 0;       // [FIX 3]
            bt_state_set(BT_STATE_DISCONNECTED);
            return;

        default: break;
    }
}


static void controller_handler(uint8_t packet_type, uint16_t channel,
                                uint8_t *packet, uint16_t size) {
    UNUSED(channel); UNUSED(size);
    uint8_t play_status;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    if (_cid == 0) return;

    switch (packet[2]) {

        case AVRCP_SUBEVENT_GET_CAPABILITY_EVENT_ID_DONE:
            avrcp_controller_enable_notification(_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
            avrcp_controller_enable_notification(_cid, AVRCP_NOTIFICATION_EVENT_NOW_PLAYING_CONTENT_CHANGED);
            avrcp_controller_enable_notification(_cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
            avrcp_controller_get_now_playing_info(_cid);
            _notifications_enabled = true;
            _stale_count           = 0;
            break;

        case AVRCP_SUBEVENT_NOTIFICATION_TRACK_CHANGED:
            // [FIX 4] Set pending flag — timestamp updated only on receipt below
            _metadata_pending = true;
            avrcp_controller_get_now_playing_info(_cid);
            break;

        case AVRCP_SUBEVENT_NOW_PLAYING_TITLE_INFO:
            metadata_received();   // [FIX 4] timestamp on confirmed receipt
            printf("META:TITLE:%.*s\n",
                (int)avrcp_subevent_now_playing_title_info_get_value_len(packet),
                (char*)avrcp_subevent_now_playing_title_info_get_value(packet));
            break;

        case AVRCP_SUBEVENT_NOW_PLAYING_ARTIST_INFO:
            metadata_received();   // [FIX 4]
            printf("META:ARTIST:%.*s\n",
                (int)avrcp_subevent_now_playing_artist_info_get_value_len(packet),
                (char*)avrcp_subevent_now_playing_artist_info_get_value(packet));
            break;

        case AVRCP_SUBEVENT_NOW_PLAYING_ALBUM_INFO:
            metadata_received();   // [FIX 4]
            printf("META:ALBUM:%.*s\n",
                (int)avrcp_subevent_now_playing_album_info_get_value_len(packet),
                (char*)avrcp_subevent_now_playing_album_info_get_value(packet));
            break;

        case AVRCP_SUBEVENT_NOW_PLAYING_INFO_DONE:
            printf("META:END\n");
            break;

        case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_STATUS_CHANGED:
            play_status = avrcp_subevent_notification_playback_status_changed_get_play_status(packet);
            metadata_received();   // [FIX 4] counts as metadata activity
            switch (play_status) {
                case AVRCP_PLAYBACK_STATUS_PLAYING:
                    _playing = true;
                    printf("META:STATUS:PLAYING\n");
                    avrcp_controller_get_now_playing_info(_cid);
                    _metadata_pending = true;  // [FIX 4] wait for receipt
                    break;
                case AVRCP_PLAYBACK_STATUS_PAUSED:
                    _playing = false;
                    printf("META:STATUS:PAUSED\n");
                    break;
                default:
                    _playing = false;
                    printf("META:STATUS:STOPPED\n");
                    break;
            }
            break;

        default: break;
    }
}


static void target_handler(uint8_t packet_type, uint16_t channel,
                             uint8_t *packet, uint16_t size) {
    UNUSED(channel); UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    switch (packet[2]) {
        case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED:
            _volume = avrcp_subevent_notification_volume_changed_get_absolute_volume(packet);
            avrcp_volume_changed(_volume);
            printf("META:VOL:%d\n", (_volume * 100) / 127);
            break;
        default: break;
    }
}


void avrcp_begin() {
    avrcp_init();
    avrcp_controller_init();
    avrcp_target_init();
    _hci_event_cb.callback = &hci_packet_handler;
    hci_add_event_handler(&_hci_event_cb);
        avrcp_register_packet_handler(connection_handler);
    avrcp_controller_register_packet_handler(controller_handler);
    avrcp_target_register_packet_handler(target_handler);
}

uint8_t avrcp_get_volume()   { return _volume; }
bool    avrcp_is_connected() { return _cid != 0; }
bool    avrcp_is_playing()   { return _playing; }



// --- Public getter implementations ---
bt_state_t avrcp_get_bt_state(void) {
    return _bt_state;
}

const char *avrcp_get_remote_addr_str(void) {
    return bd_addr_to_str(_remote_addr);
}

uint8_t avrcp_get_play_status(void) {
    return _play_status;
}
