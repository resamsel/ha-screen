# Main-screen pages from a Home Assistant dashboard

## Why
Pages, rows and graphs are hard-coded in `main/config.h`, so changing what the screen
shows means rebuilding and reflashing. The user wants to edit the layout in the HA dashboard
`esp32-c6-dev` (view `main`) and have the board pick up the change on its own within a minute.

## Goals
- The dashboard view `esp32-c6-dev/main` defines the main-screen pages.
- Edits show up on the board within 60 s, with no reflash or restart.
- If HA can't be reached or the dashboard is broken, the board shows the last good layout.

## Non-goals
- The settings panel (theme, landscape, brightness) stays as it is and still comes from `config.h`.
- No new card types. Only tiles and line-graph sensor cards are understood.
- The 5 s value/graph polling stays as it is.

## Anti-goals
- A dashboard edit must never leave the screen blank or crash the board.

## Resilience (user requirement)
Misconfiguration is ignored piece by piece, never treated as an error:
- Unknown view/section/card types, unknown or wrongly typed fields: skipped.
- A tile or graph card without `entity`, or whose entity doesn't exist / has a non-numeric
  state: that row or page is skipped (a temporarily `unavailable` entity keeps its row and
  shows "--", as today).
- A section left with no usable cards: no page. A missing heading: page title is empty.
- Too many pages/rows: the extra ones are left out.
- Only if nothing usable is left at all (view missing, JSON unparsable, zero pages) is the
  fetched layout discarded; the board then keeps the last good layout.
- Skipped parts are logged once per fetch (serial log), not shown in the error box.

## Approaches
- **A. Re-read the dashboard every 60 s (chosen).** Short websocket connection: connect, auth,
  `lovelace/config`, close. Rebuild the UI only when the config changed. Small addition;
  existing REST polling stays.
- B. Keep a websocket open and subscribe to `lovelace_updated`. Changes show up in about 1 s, but
  needs a connection that stays open plus reconnect logic, and really only pays off if state
  updates move to the same connection. Too large a change for the benefit.
- C. HA copies the layout into a sensor the board already reads over REST. Rejected: HA templates
  can't read dashboard configs, so this needs fragile setup on the HA side.

## Mapping (decided with the user)
- Each **section** of the view = one page, in dashboard order. Its `heading` card text = page
  title in the status line.
- A section of **`tile`** cards = a value page; one row per tile, in dashboard order.
- A section with a **`sensor` card using `graph: line`** = a graph page for that entity.
- Row/graph name: the card's `name` if it is a plain string, otherwise the entity's
  `friendly_name`.

## My calls (for the builder; flag if wrong)
- Dashboard path and view (`esp32-c6-dev`, `main`) go in `config.h` as `HA_DASHBOARD`/`HA_VIEW`.
- Limits: at most 8 pages and 4 rows per value page; extra sections and tiles are left out (logged).
- Sections with neither tiles nor a graph card are skipped. A section with both becomes a graph
  page if its first data card is a graph.
- Graph time span: the card's `hours_to_show`, default 24 (max what fits `HA_GRAPH_POINTS`).
- Graph y-axis starts at 0 when the unit is `%`, otherwise fits the data. Decimals: 1 if the
  current value is under 100, otherwise 0. These replace `HA_GRAPH_DECIMALS`/`HA_GRAPH_ZERO_BASED`.
- Smoothing, line width, tick spacing and timezone stay global in `config.h`.
- The last good parsed layout is cached in NVS; at boot the board uses the cache, then the
  first successful fetch. With no cache and no HA, it shows a "loading layout" message.
- The board compares a hash of the fetched JSON with the previous one and only rebuilds when it
  differs; the rebuild keeps the current page index when it still exists.
- Websocket: ESP-IDF `esp_websocket_client` component, same token, TLS like the REST client.
  The response can be several KB; read it into a heap buffer and parse with cJSON.
- The `HA_SENSOR_PAGES`/`HA_*_ENTITIES`/`HA_GRAPH_*` page arrays and `HA_ROW_NAMES` go away.

## Left to the builder
- How the network task schedules the 60 s fetch alongside the 5 s poll.
- How the UI code moves from compile-time arrays to a runtime layout struct.
