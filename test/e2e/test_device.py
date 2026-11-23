"""End-to-end tests against the real board. Read-only: nothing here changes a setting
(theme, orientation, brightness, view, dashboard, presentation).

  pip install -r test/e2e/requirements.txt
  pytest test/e2e --port /dev/cu.usbmodem14401
"""
import re

from device import diff_ratio

SAME = 0.02       # at most this share of pixels may differ for "the same screen"
CHANGED = 0.05    # at least this much for "a different screen"

CRASH = re.compile(r"Guru Meditation|abort\(\) was called|Backtrace:|rst:0x|Stack canary|assert failed", re.I)


def assert_same(dev, ref, msg="screen changed", tries=3):
    """The screen matches ref, allowing for a moment of settling (a poll can redraw
    a value or the status line just then)."""
    for _ in range(tries):
        cur = dev.screenshot()
        r = diff_ratio(ref, cur)
        if r < SAME:
            return
        dev._drain(1.5)
    assert r < SAME, "%s (%.1f %% differs)" % (msg, r * 100)


def landscape(img):
    return img.size[0] > img.size[1]


def test_screenshot_size(dev):
    img = dev.screenshot()
    assert img.size in ((320, 172), (172, 320))


def test_screen_not_blank(dev):
    img = dev.screenshot()
    colors = img.convert("L").getcolors(maxcolors=1 << 16)
    assert colors is None or len(colors) > 8, "screen looks blank"


def test_same_page_is_stable(dev):
    assert_same(dev, dev.screenshot())


def test_pages_differ(dev):
    first = dev.screenshot()
    dev.page(1)
    second = dev.screenshot()
    assert diff_ratio(first, second) > CHANGED, "page 1 looks like page 0 (only one page?)"
    dev.page(0)
    assert_same(dev, first, "back on page 0 it looks different")


def test_config_panel_opens_and_closes(dev):
    main = dev.screenshot()
    dev.cfg(0)
    panel = dev.screenshot()
    assert diff_ratio(main, panel) > CHANGED
    dev.close_cfg()
    assert_same(dev, main, "the panel did not close")


def test_config_screens_distinct(dev):
    shots = []
    for n in range(5):            # list, display, presentation, night, firmware
        dev.cfg(n)
        shots.append(dev.screenshot())
    for i in range(len(shots)):
        for j in range(i + 1, len(shots)):
            assert diff_ratio(shots[i], shots[j]) > SAME, "config screens %d and %d look the same" % (i, j)


def test_dashboard_screen_opens_and_goes_back(dev):
    dev.cfg(0)
    before = dev.screenshot()
    # the Dashboard row, second in the list below the 36 px header
    row_h = 34 if landscape(before) else 40
    dev.tap(before.size[0] // 2, 36 + row_h + row_h // 2)
    picker = dev.screenshot()
    assert diff_ratio(before, picker) > 0.01, "the dashboard screen did not open"
    dev.tap(30, 18)               # the header's back arrow
    assert_same(dev, before, "back did not return to the list")


def test_touch_shows_status(dev):
    quiet = dev.screenshot()
    touched = dev.screenshot(touch=True)
    assert touched.size == quiet.size


def test_no_crash_in_log(dev):
    dev._drain(2)
    bad = [l for l in dev.log if CRASH.search(l)]
    assert not bad, "\n".join(bad[:10])
