# ha-screen

Home Assistant display for the Waveshare ESP32-C6-Touch-LCD-1.47.
Settings (HA URL, settings entities, poll interval, graph style) are in `main/config.h`.

## Pages from a dashboard

The main-screen pages come from the HA dashboard view `HA_DASHBOARD` (the view is chosen in the config panel)
(`esp32-c6-dev`). The board re-reads it every minute and rebuilds when it changed:

- each section = one page; its heading card = the page title
- `tile` cards = value rows (up to 4), in dashboard order; name = the card's name or the
  entity's friendly name
- a `sensor` card with `graph: line` = a graph page (span = `hours_to_show`, default 24)
- an `entity` card = like a tile
- a `gauge` card = the value on a half-circle arc (`min`/`max`, `severity` colours like HA)
- a `glance` card = one cell per entity (icon, state, name), up to 4 per page
- a `button` card = a tap toggles on/off entities, runs scripts, activates scenes and presses
  `button.*` / `input_button.*` entities
- a `picture-entity` card = a picture fitted into its block, name along the bottom
  (`show_name: false` hides it). Source: `camera_image`, else `image` (URL, absolute or relative
  to HA; the HA token is only sent to HA), else the card's own `camera.*`/`image.*` entity.
  Cameras refresh every 60 s while the page is shown (`HA_PICTURE_INTERVAL_MS`), image entities
  when their state changes, URLs on each page visit. JPEG (baseline) and PNG (non-interlaced)
- a `weather-forecast` card = current weather of a `weather.*` entity: icon, condition, name,
  temperature and humidity; with `show_forecast: true` also a forecast row (daily, or hourly
  with `forecast_type: hourly`) when the block is tall enough, as many columns as fit
- icons on tiles, sensor graphs and buttons, as in HA: the card's `icon`, else the entity's,
  else HA's icon for its integration, device class or domain (`show_icon: false` hides it).
  All Material Design Icons are built in (`tools/make_icons.py` -> `main/mdi_icons.bin`)
- up to 8 pages; anything else (other cards, missing or non-numeric entities) is skipped

The last good layout is kept in flash, so the board starts with it when HA isn't reachable.

### Board name and helpers in HA

Each board gets an ID from its MAC, e.g. `screen_a1b2c3`. Create the text helper
`input_text.screen_a1b2c3` and set it to the board's name (e.g. `kitchen`; slugified like HA
object IDs). Until it is set, the board shows a setup screen with the helper to create. The
name prefixes the board's own helpers, all optional:

| Entity (name `kitchen`)                     | Value                                         |
|---------------------------------------------|-----------------------------------------------|
| `input_boolean.kitchen_theme`               | on = dark theme, off = light                  |
| `input_boolean.kitchen_landscape`           | on = landscape, off = portrait                |
| `input_boolean.kitchen_presentation`        | presentation mode on/off                      |
| `input_number.kitchen_presentation_interval`| seconds per page                              |
| `input_number.kitchen_brightness`           | backlight (uses the entity's min/max)         |
| `input_text.kitchen_dashboard`              | dashboard url_path (empty = `HA_DASHBOARD`)   |
| `input_text.kitchen_view`                   | view path (empty = the first view)            |
| `input_number.kitchen_page`                 | page, from 1 (min 1, max 8, step 1)           |

Renaming the board in HA switches it to the new helpers right away; the last name is kept in
flash. Dashboard, view and page are synced both ways (the last change wins): setting them in
HA switches the board, swiping or picking a view on the board writes them. The page isn't
written while presentation mode is on. Diagnostics go to `sensor.<name>_*`.

## Secrets

Wi-Fi credentials and the HA token are read from environment variables **at build time**
and compiled into the firmware. The build stops if one is missing. Keep them in `.env`
(gitignored; copy `.env.example`), which the build command passes to Docker with
`--env-file`. Format: `NAME=value`, one per line, no quotes.

| Variable    | Value                                                        |
|-------------|--------------------------------------------------------------|
| `WIFI_SSID` | Wi-Fi network name                                           |
| `WIFI_PASS` | Wi-Fi password                                               |
| `HA_TOKEN`  | HA > your profile > Security > Long-lived access tokens      |

They end up in the generated `build/esp-idf/main/secrets_env.h` (in the build directory,
never in the source tree). Anyone with the firmware binary can read them.

## Start-up cache

The last dashboard layout and card data (values, graph history, buttons, weather, icons, theme)
are kept in the `cache` flash partition and shown right after booting, before Wi-Fi and HA
answer. The layout is saved when it changes, the data a minute after start and then at most
every 15 min (`HA_CACHE_INTERVAL_MS`). The partition table must be flashed once (see below).

## Screenshots

With the board on USB, `python3 tools/screenshot.py [port] [out.png]` saves what is on screen
(needs pyserial and Pillow; the board redraws once and streams it over the cable).
`--page=` sends any command first: a digit shows that page, `V<n>` shows dashboard view n,
`C<n>` opens config panel page n (`C9` closes it).

All screens, dark and light, portrait and landscape: [docs/screenshots.md](docs/screenshots.md).

## Build and flash

```bash
docker run --rm -v "$PWD":/project -v ha-screen-build:/build -w /project \
  --env-file .env \
  espressif/idf:v5.3.2 sh -c 'idf.py -B /build build && cp /build/lvgl_example.bin /build/partition_table/partition-table.bin out/'
esptool --chip esp32c6 -p /dev/cu.usbmodem1101 -b 460800 write-flash 0x8000 out/partition-table.bin 0x10000 out/lvgl_example.bin
```

## Tests

Unit tests (host, run in CI):

```bash
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.3.2 sh test/run.sh
```

End-to-end tests against the flashed board over USB (local only; they only read, no setting
is changed): page switching, the config panel and its pages, the dashboard list, no crash in
the log.

```bash
pip install -r test/e2e/requirements.txt
pytest test/e2e -p no:cacheprovider --port /dev/cu.usbmodem14401
```
