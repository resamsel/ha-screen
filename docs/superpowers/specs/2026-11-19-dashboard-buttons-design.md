# Buttons on dashboard pages

## Why
Quick on/off switching (e.g. the water timer) from the display, with the state visible.
Scripts and scenes can be triggered too.

## Design (approved)
- A section of `button` cards = a button page, up to 4 buttons in dashboard order.
  Portrait: stacked; landscape: side by side. Name resolved like tile names.
- Toggleable entities (state on/off): show "On"/"Off"; on = amber #FFC107 with dark text,
  off = neutral grey. `unavailable`/`unknown` = greyed "--", not tappable.
- Tap = `homeassistant.toggle`; the button flips at once and pulses until the next poll
  confirms; on failure it flips back.
- `script.*` / `scene.*`: show "Run" / "Activate"; tap = `script.turn_on` / `scene.turn_on`;
  brief accent flash, red flash on failure. Accepted whatever their state.
- `tap_action` absent or `toggle` = act; any other action = shown, tap does nothing (logged).
- Other entities (e.g. a sensor on a button card) are skipped. Non-button cards in a button
  section are skipped (first data card decides the page kind, as before).
- Page limit 8 -> 12 (NVS layout key bumped).

## Approaches
- Chosen: button page as a new layout kind, state via the existing 5 s poll, taps through the
  network task like the theme toggle.
- Rejected: buttons as rows on value pages (tap-to-zoom vs tap-to-toggle conflict); a persistent
  websocket for instant state (rejected earlier as too large).
