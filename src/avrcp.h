#ifndef avrcp_h
#define avrcp_h

#include <btstack.h>

#define AVRCP_PLAY_STATUS_STOPPED    0x00
#define AVRCP_PLAY_STATUS_PLAYING    0x01
#define AVRCP_PLAY_STATUS_PAUSED     0x02
#define AVRCP_PLAY_STATUS_FWD_SEEK   0x03
#define AVRCP_PLAY_STATUS_REV_SEEK   0x04


void avrcp_begin();

uint8_t avrcp_get_volume();  // 0..127
bool avrcp_is_connected();
bool avrcp_is_playing();


// Bluetooth connection state
typedef enum {
    BT_STATE_UNPAIRED     = 0,
    BT_STATE_PAIRING      = 1,
    BT_STATE_PAIRED       = 2,
    BT_STATE_CONNECTED    = 3,
    BT_STATE_DISCONNECTED = 4,
} bt_state_t;

bt_state_t  avrcp_get_bt_state(void);
const char *avrcp_get_remote_addr_str(void);
uint8_t     avrcp_get_play_status(void);      // returns AVRCP_PLAY_STATUS_*

#endif