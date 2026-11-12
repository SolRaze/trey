#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Notification LED for the broadcast: WAIT slow blue pulse once armed, LIVE steady blue while
 * Azahar is connected, OFF when the stream ends or Relay starts. Through mcu::HWC; HOME Menu
 * notifications can take the LED over. */
typedef enum { LED_BROADCAST_OFF, LED_BROADCAST_WAIT, LED_BROADCAST_LIVE } led_broadcast_t;
void led_broadcast(led_broadcast_t mode);

#ifdef __cplusplus
}
#endif
