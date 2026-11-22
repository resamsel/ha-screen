# ha-screen

Home Assistant display for the Waveshare ESP32-C6-Touch-LCD-1.47.
Settings (HA URL, settings entities, poll interval, graph style) are in `main/config.h`.

## Pages from a dashboard

The main-screen pages come from an HA dashboard view chosen on the board (config panel; asked for
once the board first reaches HA). The board re-reads it every minute and rebuilds when it changed:

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

No helpers are needed: a new board works right after setup and keeps its settings (theme,
orientation, brightness, presentation mode, dashboard, view, page) itself. Create any of the
helpers below later and the board hands that setting to HA from then on.

After the setup portal is saved, and whenever the board's name changes, the board creates its
missing helpers itself, starting with its current settings. This needs the token of an HA
admin user; with any other token it skips them.

Each board gets an ID from its MAC, e.g. `screen_a1b2c3`. Its name (e.g. `kitchen`; slugified
like HA object IDs) is set in the setup portal; the text helper `input_text.screen_a1b2c3`, if
it exists and isn't empty, overrides it. The name prefixes the board's own helpers:

| Entity (name `kitchen`)                     | Value                                         |
|---------------------------------------------|-----------------------------------------------|
| `input_boolean.kitchen_theme`               | on = dark theme, off = light                  |
| `input_boolean.kitchen_landscape`           | on = landscape, off = portrait                |
| `input_boolean.kitchen_presentation`        | presentation mode on/off                      |
| `input_number.kitchen_presentation_interval`| seconds per page                              |
| `input_boolean.kitchen_night`              | night mode: backlight at the night brightness |
| `input_number.kitchen_night_brightness`    | backlight % at night, 0 = off                 |
| `input_number.kitchen_screen_timeout`      | screen off after N min without touch, 0 = never |
| `input_number.kitchen_brightness`           | backlight (uses the entity's min/max)         |
| `input_text.kitchen_dashboard`              | dashboard url_path (empty = ask on the board) |
| `input_text.kitchen_view`                   | view path (empty = the first view)            |
| `input_number.kitchen_page`                 | page, from 1 (min 1, max 8, step 1)           |

Renaming the board in HA switches it to the new helpers right away; the last name is kept in
flash. Dashboard, view and page are synced both ways (the last change wins): setting them in
HA switches the board, swiping or picking a view on the board writes them. The page isn't
written while presentation mode is on. Diagnostics go to `sensor.<name>_*`.

## Wi-Fi and Home Assistant setup

Wi-Fi name and password, the HA URL and the HA token are kept in flash (NVS) and set on the
board itself, so CI firmware can be flashed as-is. The board opens a **setup portal** when:

- a setting is missing (first boot),
- the BOOT button is held while it powers on, or
- Wi-Fi or Home Assistant didn't work within 2 min of booting (`HA_WIFI_PORTAL_S`): no Wi-Fi,
  HA unreachable at the URL, or the token rejected. The form then says which.

The screen then shows an access point named after the board ID (`screen_a1b2c3`): join it with a phone (the setup page
usually opens by itself, else open `http://192.168.4.1`), fill in the form and save; the board
restarts and connects. The form also asks for the device name (default: the board ID). Empty
password or token fields keep the stored value. With settings
already stored, the portal gives up after 5 min without a connected phone
(`HA_PORTAL_IDLE_S`) and restarts normally, so a router outage doesn't strand the board.

### Optional build-time defaults

`WIFI_SSID`, `WIFI_PASS` and `HA_TOKEN` environment variables, if set at build time, are used
until the portal stores a value (`HA_URL_DEFAULT` in `main/config.h` likewise). Keep them in
`.env` (gitignored; copy `.env.example`), which the build command passes to Docker with
`--env-file`. Format: `NAME=value`, one per line, no quotes. They end up in the generated
`build/esp-idf/main/secrets_env.h` and anyone with such a binary can read them; leave them
unset for firmware you share.

| Variable    | Value                                                        |
|-------------|--------------------------------------------------------------|
| `WIFI_SSID` | Wi-Fi network name                                           |
| `WIFI_PASS` | Wi-Fi password                                               |
| `HA_TOKEN`  | HA > your profile > Security > Long-lived access tokens      |

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
