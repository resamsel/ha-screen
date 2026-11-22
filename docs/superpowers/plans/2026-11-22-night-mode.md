# Night Mode and Screen-off Timeout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Backlight off after an idle timeout, dimmed/off while HA says it's night, wake on touch without the touch acting.

**Architecture:** A pure function `screen_level()` (new `main/screen.c`, host-tested) decides the backlight %. `main/main.c` owns state (idle tick, night flag, settings in NVS), a 1 s LVGL timer that applies the level, a top-layer overlay that eats the waking touch, and the Display-page controls. `main/ha_client.c` reads/writes/creates three new optional helpers, mirroring the presentation helpers.

**Tech Stack:** ESP-IDF 5.3, LVGL 8, cJSON, host tests via `test/run.sh`.

**Spec:** `docs/superpowers/specs/2026-11-22-night-mode-design.md`

## Global Constraints
- Helpers (all optional; 404 = board keeps its own NVS value): `input_boolean.<name>_night`, `input_number.<name>_night_brightness` (0–100 %, step 1, 0 = off), `input_number.<name>_screen_timeout` (minutes, 0 = never).
- Timeout choices on the board: never, 1, 2, 5, 10, 30 min.
- Wake fallback when the level in effect is 0: 10 %, for the idle timeout, or 30 s if the timeout is "never".
- Presentation mode running: no idle timeout; night brightness still applies.
- The waking touch never reaches widgets or swipes.
- New settings are separate NVS values (namespace used by `ui_save_u8`); the layout blob key is not bumped.

## Review Focus
- Night on with night brightness 0 and no touch: screen must be fully off (0 %, not the 5 % floor of `apply_backlight`). → Task 1 test `night_zero_is_off`.
- HA brightness change arriving while the screen is off/dim must not light it up. → Task 2 step: route `ha_brightness_changed` through `screen_apply()`.
- Touch on a woken-at-night screen after the 10 % fallback times out goes dark again. → Task 1 test `wake_fallback_expires`.
- Config panel open counts as activity (no turning off mid-edit): any touch resets idle. → Task 2.
- HA unreachable at boot: last night state from NVS. → Task 2.

---

### Task 1: Backlight level rule (pure, host-tested)

**Files:**
- Create: `main/screen.c`, `main/screen.h`, `test/screen_test.c`
- Modify: `test/run.sh`, `main/CMakeLists.txt` (add `screen.c`)

**Interfaces:**
- Produces:
```c
typedef struct {
    int normal_pct;      /* 5..100, day brightness */
    int night_pct;       /* 0..100 */
    bool night;
    int timeout_min;     /* 0 = never */
    bool presenting;     /* presentation mode cycling */
    uint32_t idle_ms;    /* since last touch */
    bool woken;          /* last touch woke a dark screen */
} screen_in_t;
int screen_level(const screen_in_t *in);   /* backlight % to set, 0 = off */
#define SCREEN_WAKE_PCT 10
#define SCREEN_WAKE_MS  30000
```

- [ ] **Step 1: Write failing tests** in `test/screen_test.c` (same assert style as `test/names_test.c`):
  - `day_active`: normal 60, night false, idle 0 → 60
  - `day_timeout`: timeout 5, idle 5*60000 → 0; idle 5*60000-1 → 60
  - `never_timeout`: timeout 0, idle 10 h → 60
  - `presenting_keeps_on`: timeout 1, idle 10 min, presenting → 60
  - `night_dim`: night, night_pct 15 → 15
  - `night_zero_is_off`: night, night_pct 0, idle 10 min, woken false → 0
  - `night_presenting_uses_night`: night, night_pct 0, presenting → 0
  - `wake_fallback`: night, night_pct 0, woken, idle 0 → 10
  - `wake_fallback_expires`: same, timeout 0, idle 30000 → 0; timeout 2, idle 60000 → 10, idle 120000 → 0
- [ ] **Step 2:** add the build line to `test/run.sh`; run `docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.3.2 sh test/run.sh` → fails to link.
- [ ] **Step 3:** implement `screen_level` in `main/screen.c`.
- [ ] **Step 4:** run tests → all pass.
- [ ] **Step 5:** commit `feat: backlight level rule for night mode and idle timeout (#17)`.

### Task 2: Apply on the board, wake overlay, Display-page controls

**Files:**
- Modify: `main/main.c` (backlight section near `apply_backlight` ~3335; Display config page; init ~224–250)

**Interfaces:**
- Consumes: `screen_level`, `screen_in_t`.
- Produces (for Task 3): `static void ha_night_changed(bool night, int night_pct, int timeout_min)` registered via `ha_client_set_night_cb`; calls `ha_client_set_night(night, night_pct, timeout_min)` on local edits.

- [ ] **Step 1:** state + NVS: `s_night` (u8 "night"), `s_night_pct` (u8 "nbri", default 0), `s_timeout_min` (u8 "scr_to", default 0), `s_last_touch`, `s_woken`, `s_level`. Load beside `pres_on`.
- [ ] **Step 2:** `static void screen_apply(void)` builds `screen_in_t` (normal = `s_boot_bri`; idle = `lv_disp_get_inactive_time(NULL)`; presenting = `s_pres_on && PAGE_COUNT > 1`), sets backlight only when the level changes (0 → `bsp_display_set_brightness(0)`, bypassing the 5 % floor). `apply_backlight` and `ha_brightness_changed` call `screen_apply()` instead of setting the backlight directly; the slider preview while dragging still sets the dragged value.
- [ ] **Step 3:** 1 s `lv_timer` calling `screen_apply()`. When the level drops to 0 or to the night level from a brighter one, clear `s_woken` once the woken period has passed (handled by `screen_level`; reset `s_woken` when `s_level` becomes 0).
- [ ] **Step 4:** wake overlay: full-screen transparent clickable object on `lv_layer_top()`, hidden normally, shown whenever `s_level == 0` or (night and not woken). Its `LV_EVENT_PRESSED` sets `s_woken = true`, `lv_disp_trig_activity(NULL)`, hides itself, calls `screen_apply()`; the press is consumed so no widget or swipe sees it.
- [ ] **Step 5:** Display config page: "Night" toggle button, "Night brightness" slider 0–100, "Screen off" button cycling never/1/2/5/10/30 min (same pattern as the presentation interval button). Each change saves NVS, calls `ha_client_set_night(...)`, `screen_apply()`.
- [ ] **Step 6:** `idf.py build` succeeds; flash; check on device: timeout 1 min → screen off after 60 s, touch wakes without pressing anything underneath; night on with 0 → off, touch → 10 %, off again after 30 s; presentation on → stays on.
- [ ] **Step 7:** commit `feat: night mode and screen-off timeout on the board (#17)`.

### Task 3: HA helpers

**Files:**
- Modify: `main/ha_client.c` (enum ~438, `s_ent` ~447, `create_helpers` ~987, helper polling ~1079, live ids ~1964, live state ~2021, setters ~2372), `main/ha_client.h`, `main/config.h` (helper list comment), `README.md` (helper table)

**Interfaces:**
- Produces:
```c
typedef void (*ha_night_cb_t)(bool night, int night_pct, int timeout_min);
void ha_client_set_night_cb(ha_night_cb_t cb);       /* before ha_client_start */
void ha_client_set_night(bool night, int night_pct, int timeout_min);
```
  Callback gets -1 for a value whose helper doesn't exist (board keeps its own).

- [ ] **Step 1:** add `H_NIGHT, H_NIGHT_BRI, H_SCR_TO` before `H_COUNT` and the `s_ent` ids; create bodies in `create_helpers` (`input_boolean/create` icon `mdi:weather-night`; `input_number/create` night brightness 0–100 step 1 `%` slider; screen timeout 0–30 step 1 `min` box), initial values from the board's settings (extend the settings struct passed for helpers).
- [ ] **Step 2:** poll (like `pres`/`pres_int`) and live updates call the night callback; add the three ids to the live subscription list.
- [ ] **Step 3:** `ha_client_set_night` sets an event bit; the task writes `input_boolean/turn_on|off` and `input_number/set_value` for existing helpers only.
- [ ] **Step 4:** document the helpers in `main/config.h` and `README.md`.
- [ ] **Step 5:** build, flash; in HA toggle `input_boolean.<name>_night` → board dims within a few seconds; change timeout in HA → board's button follows; change on board → HA follows. Host tests still pass.
- [ ] **Step 6:** commit `feat: night mode and screen timeout helpers in HA (#17)`.
