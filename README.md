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
- an `entities` card = one row per entity (icon, name, state), up to 4 per page; its title becomes the section heading; dividers and other special rows are skipped
- a `thermostat` card = the target temperature between − and + (sent 1 s after the last tap, within the entity's min/max and step), the action (heating, idle, ...) and the current temperature below
- a `media-control` card = name, title and artist (the state when nothing plays), play/pause and next buttons
- a `light` card = name and brightness, an on/off button and a brightness bar: a tap on the bar sets the brightness there (sent 0.6 s after the last tap)
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
- a `calendar` card = the events of its `calendar.*` entities, merged, in the card's
  `initial_view`: Day = today's events ("● 13:00 title … 1h": a dot in the calendar's colour, the duration right-aligned, long
  titles on up to two lines), List (7 days) = the next 7 days'
  events under day headings, Month (HA's default) = a month grid with today circled and a dot on
  days with events. Up to 8 events, as many as fit. The heading is `title`, else the date.
  Up to 4 calendar cards per dashboard view
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
| `input_text.kitchen_notification`           | a message to show (see Notifications)         |

Renaming the board in HA switches it to the new helpers right away; the last name is kept in
flash. Dashboard, view and page are synced both ways (the last change wins): setting them in
HA switches the board, swiping or picking a view on the board writes them. The page isn't
written while presentation mode is on. Diagnostics go to `sensor.<name>_*`.

### Notifications

Setting `input_text.<name>_notification` shows its text as a banner at the top of the screen
(up to 3 lines; longer text ends in "..."). It wakes the screen and goes away after 30 s or
on a tap; then the board empties the helper, so the same message can be sent again. A new
message replaces the one shown. Up to 255 characters; the board's font has Latin letters
only (no emoji). Line breaks: a real one from an automation, or `\n` typed into the helper
(`\\` for a backslash). Example automation action:

```yaml
action: input_text.set_value
target:
  entity_id: input_text.kitchen_notification
data:
  value: "Washing machine done\nPlease empty it"
```

### Firmware updates

The config panel's last page (*Firmware*) shows the running version. *Update* installs the
latest [release](https://github.com/resamsel/ha-screen/releases) (firmware and icons) over
Wi-Fi and restarts; if
the new firmware doesn't run for a minute, the board goes back to the previous one. Boards
flashed before OTA support need one USB flash of everything first (see `docs/development.md`).

### Night mode

While `input_boolean.<name>_night` is on, the backlight runs at the night brightness (0 = off);
a touch on a dark screen only wakes it. The board has no schedule of its own: switch the helper
from HA. Example automation (*Settings → Automations → Create → Edit in YAML*), night from
sunset to dawn (30 min before sunrise) for a board named `kitchen`:

```yaml
alias: "kitchen screen: night mode at sunset, off at dawn"
mode: single
triggers:
  - trigger: sun
    event: sunset
    id: night
  - trigger: sun
    event: sunrise
    offset: "-00:30:00"
    id: day
actions:
  - choose:
      - conditions:
          - condition: trigger
            id: night
        sequence:
          - action: input_boolean.turn_on
            target:
              entity_id: input_boolean.kitchen_night
    default:
      - action: input_boolean.turn_off
        target:
          entity_id: input_boolean.kitchen_night
```

Any other trigger works the same way: a time, a bedtime scene, presence, a `schedule` helper.

### Views by time of day

`input_text.<name>_view` switches the board's view, so an automation can show a morning view
(clock, today's weather) in the morning and the usual one later. This one also cycles through
the morning view's pages (presentation mode) and stops at 9:

```yaml
alias: "kitchen screen: morning view 6-9, main view after"
mode: single
triggers:
  - trigger: time
    at: "06:00:00"
    id: morning        # the view's path
  - trigger: time
    at: "09:00:00"
    id: main
actions:
  - action: input_text.set_value
    target:
      entity_id: input_text.kitchen_view
    data:
      value: "{{ trigger.id }}"
  - action: >-
      {{ 'input_boolean.turn_on' if trigger.id == 'morning' else 'input_boolean.turn_off' }}
    target:
      entity_id: input_boolean.kitchen_presentation
```

The trigger IDs are the views' paths (*Edit dashboard → view → Settings → URL*).

### Offline alert

The board posts its diagnostics (`sensor.<name>_uptime`, ...) every 5 min. These sensors don't
turn `unavailable` when the board goes away; they just stop changing. This alerts after 15 min
without an update and clears the notification when the board is back:

```yaml
alias: "kitchen screen: offline alert"
mode: single
triggers:
  - trigger: template
    value_template: >-
      {% set s = states.sensor.kitchen_uptime %}
      {{ s is not none and now() - s.last_updated > timedelta(minutes=15) }}
actions:
  - action: persistent_notification.create
    data:
      notification_id: kitchen_screen_offline
      title: kitchen screen is offline
      message: >-
        No update since {{ as_local(states.sensor.kitchen_uptime.last_updated).strftime('%H:%M') }}.
        Check its power and Wi-Fi.
  - wait_for_trigger:
      - trigger: state
        entity_id: sensor.kitchen_uptime
  - action: persistent_notification.dismiss
    data:
      notification_id: kitchen_screen_offline
```

For a push to your phone, add a `notify.mobile_app_<phone>` action next to the notification.

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

The last dashboard layout and card data (values, graph history, buttons, weather, calendars, icons, theme)
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
