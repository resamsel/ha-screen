# Development notes

Things that aren't obvious from the code.

## Host tests

The parser (`main/layout.c`), the calendar event parser (`main/cal_parse.c`), name shortening
(`main/names.c`) and the time-zone table (`main/tz.c`) build and run on the host without the board:

```bash
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.3.2 sh test/run.sh
```

Add a case to `test/layout_test.c` or `test/names_test.c` for every parser or naming fix.

## Flashing without a global esptool

The firmware only needs `esptool` on the host. Either `brew install esptool`, or use a
throwaway virtualenv:

```bash
python3 -m venv /tmp/esp && /tmp/esp/bin/pip install esptool
/tmp/esp/bin/esptool.py --chip esp32c6 -p $(ls /dev/cu.usbmodem* | head -1) -b 460800 write_flash \
  0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 lvgl_example.bin \
  0x570000 main/mdi_icons.bin 0x7f0000 ota_data_initial.bin
```

esptool 4.x spells it `write_flash`, 5.x `write-flash`. The files are in `build/` (and
`build/bootloader/`, `build/partition_table/`), or in a release / the CI `firmware` artifact.

## Flash layout and OTA updates

`partitions.csv`: two app slots (`ota_0`, `ota_1`, 2.75 MB each), the icon blob in its own
`icons` partition (`main/mdi_icons.bin`, mapped by `icons_init()`; not part of the app so the app
fits a slot), the `cache` and `otadata`. NVS stays where it was, so a board keeps its settings.

- **First time on this layout** (boards from before issue #14 had one 6 MB `factory` app): flash
  everything over USB once, as above. After that, updates go over the air.
- **USB flashing later**: writing only the app to `0x10000` doesn't run it when the board last
  updated into `ota_1`; also write `ota_data_initial.bin` to `0x7f0000` (resets to `ota_0`).
- **Icons** come with an update: after writing a new app, *Update* streams the release's
  `mdi_icons.bin` (next to `ha-screen.bin`) and compares it with the `icons` partition; only if
  it differs is it downloaded again and written (no RAM for 2.3 MB, so two passes). The magic
  `MDI1` is written last, so an interrupted write leaves the board without icons rather than
  with garbage; *Update* on the same app version then repairs a missing/broken blob. A failed
  icon download still restarts into the new app.
- **Update**: the panel's Firmware page shows the running version; *Update* fetches
  `releases/latest/download/ha-screen.bin` (`HA_OTA_URL`), skips it when the version is the
  same, otherwise writes it to the other slot and restarts. Versions are only compared for
  equality, so a board on a dev build always installs the latest release.
- **Rollback**: a new image is on trial until it has run `HA_OTA_CONFIRM_MS` (60 s); a crash,
  watchdog or reset before that boots the previous image again.
- **Testing OTA locally**: add `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y`, point `HA_OTA_URL` at
  `http://<mac>:8765/ha-screen.bin`, give two builds different `version.txt` contents and
  serve one (plus `mdi_icons.bin`) with `python3 -m http.server 8765`.

## Generated assets

- `tools/make_bg.py` downscales the dark/light background images to the screen size.
- `tools/make_fonts.py` generates `main/fonts/*.c` from LVGL's bundled Montserrat
  (needs Pillow; run from the repo root after a build has fetched `managed_components/`).
  - `lv_font_latin_12/14/16/22`: LVGL's built-in fonts are ASCII only, so umlauts, ß, µ, °
    in HA names and units would not show. These add Latin-1 and fall back to the built-in
    font for the `LV_SYMBOL_*` icons. Line height/baseline copy the built-in fonts, so
    layouts don't shift.
  - `lv_font_clock_24…72`: digits, space and `:` only, for the clock card. The clock picks
    40/56/72 px for small/medium/large and steps down proportionally when the block is too
    small, so small stays smaller than medium.
  - `npm lv_font_conv` is not used on purpose; the Pillow script needs no extra download.

## Layout cache in NVS

The parsed `ha_layout_t` is stored as one blob in NVS namespace `layout`, under a versioned
key (currently `v6` in `main/layout_store.c`). **Bump the key whenever `ha_layout_t` or
`layout_item_t` changes**, and add the old key to the erase list: an old blob with a
different size is otherwise read as garbage, and stale blobs fill the NVS partition so the
save fails.

`layout_sanitize` must not change fields that go into the hash, or the cached layout never
matches the fetched one and the UI rebuilds every minute.

## LVGL pitfalls

- `LV_LABEL_LONG_DOT` only shortens with "…" when the label has a fixed height of one
  line; with auto height it wraps into the value below.
- Charts: when `point_cnt >= width`, LVGL 8 switches to "crowded mode", which draws without
  antialiasing and sends no draw-part events (so no fill under the line). Charts use
  width/2 points and resample the history to that.
- The fill under the line is drawn in `LV_EVENT_DRAW_PART_BEGIN` with `lv_draw_polygon`.

## UI code (`main/main.c`)

- Blocks are addressed with `GI(pg, i)`; `graph_block()` gives their geometry (stacked in
  portrait, side by side in landscape).
- Helpers must not change the shared `s_gi` index; index directly. A helper that did put
  labels into the wrong block.
- Rebuilds keep the current page when it still exists.

## Testing dashboard changes

To try a card type on the real dashboard (`esp32-c6-dev/main`), back up the view's config
first, edit, check the serial log (115200 baud) and the screen, then restore and compare
the restored config with the backup. The board picks up changes within 60 s, or at once
with the refresh button.

## Versions and releases

The version is the git tag, [semver](https://semver.org) with a `v`: `v1.2.0`.

- **Dev builds** take it from `git describe` (`v1.2.0-3-gabc1234`, `-dirty` with local changes;
  the commit hash before the first tag). CI checks out the history for that.
- **Releases**: tag a commit on `main` and push the tag:

  ```bash
  git tag -a v1.2.0 -m "v1.2.0" && git push origin v1.2.0
  ```

  `.github/workflows/release.yml` checks the tag, builds with it as the version (`version.txt`)
  with no Wi-Fi/HA defaults compiled in, and publishes a GitHub release with notes generated
  from the merged PRs. Boards pick it up with *Update*.
- **Pre-releases**: a suffix (`v1.2.0-rc.1`) publishes a pre-release. `releases/latest` skips
  it, so boards don't; install it by USB to try it.
- **Bump**: patch for fixes, minor for features, major when boards need a USB flash again (a
  partition layout change; OTA can't change the partition table).

Release assets:

| File | What |
|------|------|
| `ha-screen.bin` | the app; what *Update* installs, USB at `0x10000` |
| `mdi_icons.bin` | the icons partition; *Update* writes it too, USB at `0x570000` |
| `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin` | USB at `0x0`, `0x8000`, `0x7f0000` |
| `ha-screen-full.bin` | everything in one image, USB at `0x0`. **Erases the board's settings** (Wi-Fi, HA, name): for a new board or a recovery; to keep settings flash the files above |
| `SHA256SUMS` | checksums |
