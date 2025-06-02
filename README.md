# System Installs

``` bash
sudo apt-get install build-essential pkg-config bluez bluez-tools meson ninja-build python3 libffi-dev zlib1g-dev libmount-dev gettext
```

## GLib src files (last resort)
```
https://download.gnome.org/sources/glib/2.74/
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
