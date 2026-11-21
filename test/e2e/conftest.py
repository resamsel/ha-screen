import os

import pytest

from device import Device


def pytest_addoption(parser):
    parser.addoption("--port", default=os.environ.get("HA_SCREEN_PORT", "/dev/cu.usbmodem14401"),
                     help="the board's USB serial port")


@pytest.fixture(scope="session")
def dev(request):
    d = Device(request.config.getoption("--port"))
    d.close_cfg()
    d.page(0)
    yield d
    d.close_cfg()        # leave the board on its first page
    d.page(0)
    d.close()


@pytest.fixture(autouse=True)
def main_screen(dev):
    """Every test starts on the main screen, page 0, panel closed."""
    dev.close_cfg()
    dev.page(0)
    yield
