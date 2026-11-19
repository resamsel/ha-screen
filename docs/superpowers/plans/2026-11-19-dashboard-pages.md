# Dashboard-driven Pages Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The HA dashboard view `esp32-c6-dev/main` defines the main-screen pages; the board re-reads it every 60 s and rebuilds when it changed.

**Architecture:** A pure-C parser (`layout.c`, cJSON) turns the `lovelace/config` JSON into a fixed-size `ha_layout_t`. The HA network task fetches the config over a short websocket connection every 60 s, validates entities over REST, caches the last good layout in NVS, and hands it to the UI, which builds pages from it instead of compile-time arrays.

**Tech Stack:** ESP-IDF v5.3.2 (Docker `espressif/idf:v5.3.2`), LVGL 8.4, cJSON (IDF `json` component), `espressif/esp_websocket_client`, NVS.

**Spec:** `docs/superpowers/specs/2026-11-19-dashboard-pages-design.md`

## Global Constraints

- Dashboard `esp32-c6-dev`, view path `main`; set in `config.h` as `HA_DASHBOARD` / `HA_VIEW`.
- Max 8 pages, max 4 rows per value page; extras dropped (logged).
- Re-read interval 60 s (`HA_LAYOUT_INTERVAL_MS 60000`); value/graph polling stays at `HA_POLL_INTERVAL_MS`.
- Never error out on misconfiguration: skip unknown/unusable parts; discard a fetch only when zero usable pages remain, keeping the last good layout.
- Skipped parts are logged (ESP_LOGW) once per fetch, never shown in the on-screen error box.
- Graph: y-axis from 0 when unit is `%`, else fit; decimals 1 if |value| < 100 else 0; span = card `hours_to_show`, default 24, clamped 1..168.
- Name: card `name` if a plain string, else entity `friendly_name`.
- Settings panel (theme/landscape/brightness) unchanged.
- Build: `docker run --rm -v "$PWD":/project -v ha-screen-build:/build -w /project --env-file .env espressif/idf:v5.3.2 sh -c 'idf.py -B /build build && cp /build/lvgl_example.bin out/'`

## Review Focus

- Dashboard edited mid-session so the current page index no longer exists → land on page 0, no crash.
- Tile entity that is `unavailable` at fetch time → row kept, shows "--" (not dropped).
- Websocket response split over several frames (config > one frame) → assembled before parsing.
- HA unreachable at boot with an NVS cache → cached pages shown; without cache → "Loading layout…".
- Section whose heading card is missing but has tiles → page with empty title.

Tests for these are in Tasks 1, 3 and 5.

---

### Task 1: Layout parser (host-tested)

**Files:**
- Create: `main/layout.h`, `main/layout.c`
- Create: `test/layout_test.c`, `test/run.sh`, `test/fixtures/dashboard.json` (copy of the current `esp32-c6-dev` config, full ws message)
- Modify: `main/CMakeLists.txt` (add `layout.c`, `PRIV_REQUIRES json` if needed)

**Interfaces:**
- Produces (`layout.h`):
```c
#define LAYOUT_MAX_PAGES 8
#define LAYOUT_MAX_ROWS  4
typedef enum { LAYOUT_VALUES, LAYOUT_GRAPH } layout_kind_t;
typedef struct { char entity[64]; char name[32]; } layout_item_t;   /* name "" = use friendly_name */
typedef struct { layout_kind_t kind; char title[32]; int hours; int count; layout_item_t items[LAYOUT_MAX_ROWS]; } layout_page_t;
typedef struct { int count; layout_page_t pages[LAYOUT_MAX_PAGES]; } ha_layout_t;
/* json: full ws result message ({"result":{...}}) or the bare config. Returns page count, 0 = unusable.
 * Zeroes *out first. Skipped parts go to ESP_LOGW (host build: stderr). */
int layout_parse(const char *json, const char *view_path, ha_layout_t *out);
uint32_t layout_hash(const ha_layout_t *l);   /* FNV-1a over the struct */
/* Drop items/pages for which keep(entity) is false; returns new page count. */
int layout_filter(ha_layout_t *l, bool (*keep)(const char *entity, void *ctx), void *ctx);
```

- [ ] **Step 1: Write failing tests** in `test/layout_test.c` (plain `assert`, `main()` runs all):
  - `fixture`: parses `dashboard.json`, view `main` → 5 pages; page 0 `LAYOUT_VALUES`, title `"Temperatur"`, 3 items, item 0 entity `sensor.temperature_og`, name `""`; page 2 `LAYOUT_GRAPH`, title `"Paketverlust"`, 1 item, hours 24; page 4 title `"New section"`, entity `sensor.fritz_box_7590_ax_download_geschwindigkeit`.
  - `name_object_ignored`: `"name":{"type":"device"}` → name `""`; `"name":"Ping"` → `"Ping"`.
  - `hours`: `hours_to_show: 6` → 6; `0` or `"x"` → 24; `500` → 168.
  - `skips_unknown`: section with `{"type":"markdown"}` only → no page; unknown section `type` → skipped; card tile without `entity` → skipped; `"sections": 5` → 0 pages, no crash.
  - `no_heading`: tiles without heading → page with title `""`.
  - `graph_first_wins`: section with sensor line card then tile → `LAYOUT_GRAPH`; sensor card without `graph: line` is not a graph.
  - `limits`: 10 usable sections → 8 pages; 6 tiles → 4 items.
  - `bad_input`: `""`, `"{"`, `"null"`, missing view → returns 0.
  - `hash`: same input → same hash; changed tile → different hash.
  - `filter`: keep() rejects one of 3 tiles → 2 items; rejects the only graph entity → page removed.
- [ ] **Step 2: Create `test/run.sh`** — runs in the IDF image: `gcc -std=c11 -Wall -Werror -DLAYOUT_HOST -I main -I /opt/esp/idf/components/json/cJSON main/layout.c /opt/esp/idf/components/json/cJSON/cJSON.c test/layout_test.c -o /tmp/t && /tmp/t`. Invoke: `docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.3.2 sh test/run.sh`. Expected: compile error (layout.c missing).
- [ ] **Step 3: Implement `layout.c`.** `LAYOUT_HOST` maps `ESP_LOGW` to `fprintf(stderr)`. Every cJSON access type-checked (`cJSON_IsString`/`IsArray`/`IsNumber`); copy strings with truncation.
- [ ] **Step 4: Run tests** — Expected: `all layout tests passed`.
- [ ] **Step 5: Commit** `feat: dashboard layout parser`.

### Task 2: NVS layout cache

**Files:**
- Create: `main/layout_store.h`, `main/layout_store.c`; Modify `main/CMakeLists.txt`.

**Interfaces:**
- Consumes: `ha_layout_t`, `layout_hash` (Task 1).
- Produces: `bool layout_store_load(ha_layout_t *out);` and `void layout_store_save(const ha_layout_t *l);` — namespace `"layout"`, key `"v1"`, blob = struct; load returns false on missing/size mismatch/`count` out of 1..8. `nvs_flash_init` is already done in `main.c` (verify; add if not).

- [ ] **Step 1: Implement** both functions.
- [ ] **Step 2: Build** with the global build command. Expected: success.
- [ ] **Step 3: Commit** `feat: cache dashboard layout in NVS`.

(On-device verification happens in Task 5.)

### Task 3: Websocket dashboard fetch

**Files:**
- Create: `main/dash_fetch.h`, `main/dash_fetch.c`; Modify `main/idf_component.yml` (`espressif/esp_websocket_client: "^1.2.3"`), `main/CMakeLists.txt`, `main/config.h` (`HA_DASHBOARD "esp32-c6-dev"`, `HA_VIEW "main"`, `HA_LAYOUT_INTERVAL_MS 60000`).

**Interfaces:**
- Produces: `char *dash_fetch_config(void);` — returns malloc'd JSON of the `lovelace/config` result message for `HA_DASHBOARD`, or NULL. Caller frees.

- [ ] **Step 1: Implement.** URL = `HA_URL` with `https`→`wss` + `/api/websocket`; TLS via `crt_bundle_attach` like the HTTP client. Sequence: wait `auth_required` → send `{"type":"auth","access_token":HA_TOKEN}` → wait `auth_ok` → send `{"id":1,"type":"lovelace/config","url_path":HA_DASHBOARD}` → collect text frames until the message with `"id":1` is complete. Assemble fragmented data using `payload_offset`/`payload_len` into a heap buffer (cap 32 KB; over cap → NULL + log). Overall timeout 10 s; always stop/destroy the client. Raise `buffer_size` to 4096.
- [ ] **Step 2: Temporary check**: call once from `ha_task` after Wi-Fi up, `ESP_LOGI` the length and first 120 chars. Build, flash, read serial (`esptool`-venv `python -m serial.tools.miniterm /dev/cu.usbmodem* 115200` or `idf.py monitor` equivalent). Expected: length ≈ 2–3 KB, starts with `{"id":1,"type":"result","success":true`.
- [ ] **Step 3: Remove the temporary log call. Commit** `feat: fetch dashboard config over websocket`.

### Task 4: HA client driven by the layout

**Files:**
- Modify: `main/ha_client.c`, `main/ha_client.h`, `main/config.h` (remove `HA_SENSOR_PAGES`, `HA_ROW_COUNT`, `HA_PAGE_TITLES`, `HA_ROW_NAMES`, `HA_TEMP_ENTITIES`, `HA_HUM_ENTITIES`, `HA_GRAPH_COUNT`, `HA_GRAPH_ENTITIES`, `HA_GRAPH_NAMES`, `HA_GRAPH_DECIMALS`, `HA_GRAPH_ZERO_BASED`, `HA_GRAPH_HOURS`; keep smoothing, line width, points, tick, timezone).

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces (`ha_client.h`):
```c
/* New layout ready (copy owned by callee). Called from the network task. */
typedef void (*ha_layout_cb_t)(const ha_layout_t *layout);
void ha_client_set_layout_cb(ha_layout_cb_t cb);          /* before ha_client_start */
/* Replaces ha_sensor_cb_t: page/row index into the current layout; name = resolved display name. */
typedef void (*ha_sensor_cb_t)(int page, int row, const char *value, const char *unit, const char *name);
/* ha_graph_cb_t: `graph` becomes the page index; adds `int hours`, `int decimals`, `bool zero_based`. */
```

- [ ] **Step 1: Layout lifecycle in `ha_task`.** At start: `layout_store_load` → if ok, emit via layout cb. Every `HA_LAYOUT_INTERVAL_MS` (and first loop after Wi-Fi): `dash_fetch_config` → `layout_parse(json, HA_VIEW, &l)` → `layout_filter(&l, entity_usable, NULL)` → if `count > 0` and `layout_hash` differs from current: store current, `layout_store_save`, emit. Fetch/parse failure: log, keep current. The fetch failure is not added to the error box.
- [ ] **Step 2: `entity_usable`**: GET `/api/states/<entity>`; 404 → false; state numeric, `unavailable` or `unknown` → true; other non-numeric → false; request error → true (don't drop on network blips).
- [ ] **Step 3: Polling uses the current layout** (guarded by a mutex or copied at loop start): value pages → `fetch_state` per item, reporting name = item name or `attributes.friendly_name`; graph pages → `fetch_graph` with the page's `hours` (bucket math uses `hours` instead of `HA_GRAPH_HOURS`), decimals and zero_based per the Global Constraints rule.
- [ ] **Step 4: Build.** Expected: ha_client compiles; main.c errors expected until Task 5 (do Tasks 4 and 5 before flashing).
- [ ] **Step 5: Commit together with Task 5.**

### Task 5: UI built from the layout

**Files:**
- Modify: `main/main.c`, `README.md` (describe the dashboard instead of `config.h` page arrays).

**Interfaces:**
- Consumes: `ha_layout_cb_t`, new `ha_sensor_cb_t`/`ha_graph_cb_t` (Task 4).

- [ ] **Step 1: Replace compile-time arrays** (`PAGE_COUNT`, `s_g_*_[HA_GRAPH_COUNT]`, row arrays) with arrays sized `LAYOUT_MAX_PAGES`/`LAYOUT_MAX_ROWS` plus a `static ha_layout_t s_layout`. `create_pages` builds from `s_layout`; status-line title = page title.
- [ ] **Step 2: Layout callback** takes the LVGL lock, copies the layout, remembers `s_page`, calls `create_ui()`, then restores `s_page` if `< s_layout.count`, else 0. Time-axis ticks every `hours/4` h (`AXIS_MAX 5`).
- [ ] **Step 3: Empty state**: `s_layout.count == 0` → centred label "Loading layout…" on the main screen; swiping does nothing.
- [ ] **Step 4: Build, flash, verify on device:**
  - Pages appear in dashboard order: Temperatur, Luftfeuchtigkeit, Paketverlust, Ping, New section.
  - In HA, rename the "New section" heading and swap two tiles; within 60 s the board shows the change and stays on the current page.
  - Add a markdown card and a tile with a non-existent entity → ignored, the rest still shows.
  - Swipe to page 5, delete that section in HA → board falls back to page 0.
  - Make an entity `unavailable` (or pick one that is) → row shows "--".
  - Unplug/replug with HA unreachable (e.g. wrong Wi-Fi) → cached pages show.
  - Revert the HA test edits afterwards.
- [ ] **Step 5: Commit** `feat: build main-screen pages from the HA dashboard` (Tasks 4+5).
