# Notifications from HA Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An HA automation sets `input_text.<board>_notification`; the board shows it as a banner, dismissed by tap or after 30 s, then clears the helper.

**Architecture:** A pure parser (`main/notify.c`, host-tested) turns the helper's state JSON into display text. `ha_client.c` adds the helper like the other board helpers (create, subscribe, dispatch, write back) and exposes a callback plus `ha_client_notify_clear()`. `main.c` draws the banner on `lv_layer_top()`.

**Tech Stack:** ESP-IDF 5.3, LVGL, cJSON, host gcc tests via `test/run.sh`.

**Spec:** `docs/superpowers/specs/2026-11-23-notifications-design.md`

## Global Constraints

- Helper: `input_text`, name `"<board> notification"`, `max` 255, icon `mdi:message-alert`, initial `""`; entity id `input_text.<slug>_notification`.
- `HELPERS_VERSION` 2 → 3 (existing boards create the new helper once).
- Banner: top of the current page, wraps to at most 3 lines, auto-dismiss 30 s, tap dismisses, a new message replaces the current one, wakes the screen.
- Dismiss writes `""` via `/api/services/input_text/set_value`.
- Empty, `unknown`, `unavailable` states are ignored (no banner, no dismissal of a shown banner).

## Review Focus

- Text with JSON escapes (`\"`, `\n`, `ä`, emoji) must render unescaped, not as raw backslashes — test in Task 1.
- A 255-char message must not overflow buffers — test in Task 1 (buffer `NOTIFY_MAX` = 256).
- The board's own clear (state → `""`) must not re-trigger or hide a newer banner — Task 1 test (`""` → false) + Task 2 dispatch ignores false.
- A message arriving while the screen is off must wake it and still show the full 30 s — Task 3 step.
- Boards without the helper (non-admin token, HA 404) must not issue writes — Task 2 `has(H_NOTIFY)` guard.

---

### Task 1: Notification text parser

**Files:**
- Create: `main/notify.c`, `main/notify.h`
- Create: `test/notify_test.c`
- Modify: `test/run.sh` (add build+run lines like `live_test`), `main/CMakeLists.txt` (add `notify.c` to SRCS)

**Interfaces:**
- Produces: `#define NOTIFY_MAX 256` and `bool notify_text(const char *state_json, char *out, size_t n);` — `state_json` is an entity state object (`{"state":"...", ...}`, as HA's REST/`subscribe_entities` give, the latter as `{"s":"..."}` — check what `live_dispatch` receives and accept that shape). Returns true and fills `out` (UTF-8, unescaped, truncated on a char boundary) when the state is non-empty and not `unknown`/`unavailable`.

- [ ] **Step 1: Write failing tests** in `test/notify_test.c` (same assert style as `live_test.c`):
  - `{"state":"Washing machine done"}` → true, `"Washing machine done"`
  - `{"state":""}` → false; `{"state":"unknown"}` → false; `{"state":"unavailable"}` → false; `{}` → false; `"garbage"` → false
  - `{"state":"Say \"hi\"\nnow"}` → `Say "hi"` + newline + `now`
  - `{"state":"Tür offen"}` → `"Tür offen"`
  - 255-char state → true, `strlen(out) == 255`
  - `n = 6` with `"Tür offen"` → `"Tür o"` (no split `ü`)
- [ ] **Step 2: Run** `docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.3.2 sh test/run.sh` — expect compile failure (no `notify.h`).
- [ ] **Step 3: Implement `notify_text`** with cJSON (it unescapes), UTF-8-safe truncation.
- [ ] **Step 4: Run tests** — all pass.
- [ ] **Step 5: Commit** `feat: notification text parser (#19)`

### Task 2: Board helper in the HA client

**Files:**
- Modify: `main/ha_client.c` (enum `H_NOTIFY` after `H_SCR_TO`; `s_has` initializer; `s_ent.notify[80]`; `ent_build`; `create_helpers` case + name list; `HELPERS_VERSION 3` with comment "3: notification, issue #19"; `fetch_state` `helper_get` for presence only; subscribe ids list near line 2297; `live_dispatch` branch; new `NOTIFY_BIT` in the next free bit, added to `USER_BITS` and the write block next to `NIGHT_BIT`)
- Modify: `main/ha_client.h`

**Interfaces:**
- Consumes: `notify_text`, `NOTIFY_MAX` (Task 1).
- Produces:
  - `typedef void (*ha_notify_cb_t)(const char *text);` and `void ha_client_set_notify_cb(ha_notify_cb_t cb);` — called from the HA task with a non-empty message.
  - `void ha_client_notify_clear(void);` — sets `NOTIFY_BIT`; the HA task posts `{"entity_id":"<s_ent.notify>","value":""}` to `/api/services/input_text/set_value` only if `has(H_NOTIFY)`.

- [ ] **Step 1: Implement** the items above, mirroring the night helpers.
- [ ] **Step 2: Build** `idf.py build` — no warnings/errors.
- [ ] **Step 3: Run host tests** (`test/run.sh`) — still pass.
- [ ] **Step 4: Commit** `feat: board notification helper in HA client (#19)`

### Task 3: Banner UI

**Files:**
- Modify: `main/main.c` (register callback next to `ha_client_set_night_cb` ~line 292; banner code near the wake layer ~line 3370)

**Interfaces:**
- Consumes: `ha_client_set_notify_cb`, `ha_client_notify_clear` (Task 2), `screen_wake()`.

- [ ] **Step 1: Implement**
  - Callback copies text (strdup) and hands it to the LVGL side under `lvgl_port_lock(0)` (like other HA callbacks).
  - Banner: one object on `lv_layer_top()`, created once, hidden by default; full width minus margins, top-aligned; label with `LV_LABEL_LONG_WRAP`, max height for 3 lines (clip with `LV_LABEL_LONG_DOT` behaviour if longer); theme colours as the existing cards (dark/light).
  - Show: set text, unhide, move to foreground above the wake layer; `screen_wake()`; (re)start a one-shot 30 000 ms `lv_timer`.
  - Dismiss (tap via `LV_EVENT_CLICKED` on the banner, or timer): hide, delete timer, call `ha_client_notify_clear()`. The tap must not reach widgets below (clear `EVENT_BUBBLE`).
  - If the screen was off, the waking happens before the banner is shown so the 30 s count from visible.
- [ ] **Step 2: Build** `idf.py build`.
- [ ] **Step 3: Flash and verify on device** (esptool at `~/Library/Python/3.9/bin/esptool.py` if needed): set the helper in HA via REST `input_text.set_value` → banner within ~1 s; tap → gone and helper back to `""`; set again with the same text → shows again; screen off → wakes and shows; wait 30 s → gone; long 255-char text → ≤3 lines.
- [ ] **Step 4: Commit** `feat: notification banner (#19)`

### Task 4: Docs

**Files:**
- Modify: `README.md` (section "Board name and helpers in HA": list the notification helper; new short "### Notifications" section with an automation example using `input_text.set_value`, 255-char limit, 30 s / tap, replaces current)

- [ ] **Step 1: Write the docs.**
- [ ] **Step 2: Commit** `docs: notifications (#19)`
