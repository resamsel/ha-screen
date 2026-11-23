# Notifications from HA (#19) — short design

**Intent:** HA shows a short message on the board ("Washing machine done"), dismissed by tap or timeout.

**Approaches**
1. *`<board> notification` input_text helper* — picked. Reuses helper creation and the existing
   `subscribe_entities` websocket; testable by typing into the helper in HA.
2. *Custom `ha_screen_notify` event + `subscribe_events`* — rejected: second subscription on a
   RAM-tight socket, no visible state in HA, board targeting in event data.

**Design**
- New helper `H_NOTIFY`: `input_text "<board> notification"`, max 255, icon `mdi:message-alert`,
  initial "". Bump `HELPERS_VERSION` to 3 so existing boards create it once.
- Added to the subscribed entities. A non-empty state (not `unknown`/`unavailable`) → callback to
  the UI with the text.
- UI: a banner across the top of the current page, wrapping to up to 3 lines; a new message
  replaces the one shown. Wakes the screen if off/dimmed (counts as activity).
- Dismiss on tap or after 30 s → board sets the helper back to "" (`input_text.set_value`), so the
  same text sent again fires again.
- The HA client keeps running while the screen is on; an empty helper is ignored.

**Non-goals:** queued/stacked messages, per-message timeout, icons/colours per message.

**Left to the builder:** banner styling within the existing theme; exact wake hook.
