# PlatformIO extra script, run via library.json when WLED builds this usermod.
#
# 1. NimBLE-Arduino declares ble_hs_pvcy_default_irk as a tentative definition.
#    With GCC 8's default -fcommon, that makes the linker pull ble_hs_pvcy.c.obj
#    out of the framework's prebuilt libbt.a (which also contains a NimBLE
#    host), causing "multiple definition" errors. Compiling NimBLE-Arduino with
#    -fno-common stops that.
#
# 2. On the classic ESP32, WLED's default RMT-HI LED driver shares the level-5
#    interrupt with the Bluetooth controller. Once the controller starts, the
#    device hits the interrupt watchdog (ESP_RST_INT_WDT) within ~100 ms.
#    WLED_USE_SHARED_RMT selects the standard RMT driver instead. It must reach
#    WLED's own sources, which build with PlatformIO's "projenv". That is
#    exported before library builders (and so this script) are created.
Import("env", "projenv")
from SCons.Script import DefaultEnvironment

env.Append(CPPDEFINES=["WLED_USE_SHARED_RMT"])      # this usermod
projenv.Append(CPPDEFINES=["WLED_USE_SHARED_RMT"])  # WLED's sources
genv = DefaultEnvironment()
genv.Append(CPPDEFINES=["WLED_USE_SHARED_RMT"])     # libraries created after this one

for lb in genv.GetLibBuilders():
    lb.env.Append(CPPDEFINES=["WLED_USE_SHARED_RMT"])
    if lb.name == "NimBLE-Arduino":
        lb.env.Append(CCFLAGS=["-fno-common"])
