# System Installs

``` bash
sudo apt-get update && sudo apt-get install build-essential pkg-config libglib2.0-dev bluez bluez-tools
```

## GLib src files (last resort)
```
Download tar here: https://download.gnome.org/sources/glib/2.74/

sudo apt-get install build-essential pkg-config bluez bluez-tools meson ninja-build python3 libffi-dev zlib1g-dev libmount-dev gettext
```
# File Structure:
root:
- Makefile
- README
- include/
    - ble_hal.h
- src/
    - ble_hal.c
- examples/
    - hal_app.c
