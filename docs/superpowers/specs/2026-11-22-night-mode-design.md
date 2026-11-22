# Night mode and screen-off timeout (issue #17)

## Intent
Bedroom board: no glowing screen at night. Plus energy savings: screen dark when nobody uses it.

## Goals
- **Idle timeout**: backlight off after N minutes without touch. Choices: never, 1, 2, 5, 10, 30 min.
- **Night mode**: while on, backlight runs at the *night brightness* (0 = off, else a low %).
- **Wake on touch**: a touch on a dark/dimmed screen only wakes it; that touch never reaches buttons, sliders or swipes.
- HA-driven, like theme/brightness/presentation.

## Non-goals
- No on-board schedule or time picker (approach B/C rejected).
- No ambient-light sensing.

## Approaches
- **A. HA decides when it's night (picked).** One switch `input_boolean.<name>_night`; HA schedule helpers/automations flip it. Cheap in firmware, flexible scheduling.
- B. On-board schedule with start/end times: works without HA, but needs a time picker on a 1.47" screen, more RAM, clock times only.
- C. Both: most flexible, most code, two sources of truth.

## Design
- **Settings (Display page, NVS, mirrored to optional helpers, created by `create_helpers` like the others):**
  - `input_number.<name>_screen_timeout` — minutes, 0 = never.
  - `input_number.<name>_night_brightness` — %, 0 = off.
  - `input_boolean.<name>_night` — night on/off (HA sets it; also togglable from the board).
- **Brightness in effect** = night ? night brightness : normal brightness; 0 if idle-timed-out.
- **Presentation mode running keeps the screen on**: no idle timeout while it cycles. *(My call: night mode still applies to it, so a slideshow at night runs at night brightness, off if 0.)*
- **Waking**: touch on a dark screen restores the brightness in effect. If that is 0 (night, brightness off), wake to a low fallback (10 %) so you can see something; the idle timeout (or 30 s if timeout is "never") turns it off again. *(My call.)*
- HA unreachable: keep the last night state (NVS).
- Bump the NVS keys only if a stored struct changes; new settings are separate NVS values.

## Left to the builder
- Where the touch is swallowed (input read callback vs. LVGL event).
- Fade vs. instant switch (instant is fine).
- Host test for the brightness-in-effect rule if it is factored out as a pure function.
