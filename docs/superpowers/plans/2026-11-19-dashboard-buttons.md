# Dashboard Buttons Implementation Plan

**Spec:** docs/superpowers/specs/2026-11-19-dashboard-buttons-design.md

1. **Parser** (`main/layout.[ch]`, host tests first): `LAYOUT_BUTTONS` kind; `layout_item_t.action`
   (`LAYOUT_ACT_TOGGLE`, `LAYOUT_ACT_NONE`) from `tap_action.action`; `LAYOUT_MAX_PAGES 12`;
   `layout_filter` keep callback gets the page kind. Tests: button section, tap_action none,
   tile in a button section skipped, 12-page limit.
2. **Client** (`main/ha_client.[ch]`): validation per kind (buttons: on/off/unavailable/unknown, or
   script/scene); poll button pages -> `ha_button_cb_t(page, row, ha_btn_state_t, name)`;
   `ha_client_press(page, row)` -> `homeassistant/toggle`, `script/turn_on`, `scene/turn_on`;
   result via `ha_press_cb_t(page, row, ok)`. NVS key v3.
3. **UI** (`main/main.c`): button page objects, theme colours, tap hit-test in `value_tap`,
   optimistic flip + pulse, flash for script/scene, red flash on failure.
4. Build, flash, check on device (toggle the water timer twice, back to its original state).
