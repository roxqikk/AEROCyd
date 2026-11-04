AEROCyd for ESP32-2432S028R (CYD)

1. Plug the CYD in with a data USB cable.
2. Run:  ./flash.sh            (options: --erase  --monitor  --port /dev/ttyUSB0)
   The script installs PlatformIO and esptool into ./.venv, builds and flashes.
   The first run downloads the ESP32 toolchain (~1 GB), so it needs internet and a few minutes.

Troubleshooting
- Colours inverted: set CYD_INVERT to 1 in src/main.cpp. Red/blue swapped: add -DTFT_RGB_ORDER=TFT_BGR to platformio.ini.
- Touch is offset: adjust TX_MIN/TX_MAX/TY_MIN/TY_MAX in src/main.cpp.
- 2-USB-port CYD: use ST7789_DRIVER instead of ILI9341_2_DRIVER and set CYD_INVERT 1.
- Terminal commands: help, poweroff, reboot, dreammode hard|easy
