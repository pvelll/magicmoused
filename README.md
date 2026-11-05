# magicmoused

A userspace driver that makes the Apple Magic Mouse 2 on Linux behave like it does on macOS: smooth pixel-precise scrolling with inertia, workspace swipes that follow your fingers, macOS-style secondary click, smart zoom and Mission Control taps.

[Русская версия](README.ru.md)

| On the mouse                              | Result                                                                    | macOS equivalent                   |
|-------------------------------------------|---------------------------------------------------------------------------|------------------------------------|
| One finger up/down                        | Smooth scrolling with inertia; touching the mouse stops it                | Scroll                             |
| One finger left/right                     | Horizontal scrolling; back/forward in Firefox, Chrome and GNOME apps      | Swipe between pages                |
| Two fingers left/right                    | Switch GNOME workspaces, the animation follows the fingers; a flick switches at once | Swipe between full-screen apps |
| Two fingers up/down                       | Open/close the GNOME overview                                             | -                                  |
| Click with every finger on the right half | Right click (a finger resting on the left half keeps it a left click)    | Secondary click                    |
| One-finger double tap                     | Smart zoom (a pinch around the pointer)                                   | Smart zoom                         |
| Two-finger double tap                     | GNOME overview (Super)                                                    | Mission Control                    |

Developed and tested on Ubuntu 26.04 LTS (GNOME on Wayland, kernel 7.0). Other libinput-based desktops get scrolling and clicks; whether the swipes do something depends on the desktop's touchpad gestures.

## Requirements

- Apple Magic Mouse 2 (Lightning, `004c:0269`) or Magic Mouse USB-C (`004c:0323`), paired over Bluetooth. The original Magic Mouse (2009) is not supported.
- Linux with `uinput` and `hidraw` (any distribution kernel has both).
- A C compiler and `make`: `sudo apt install build-essential` on Ubuntu/Debian.

## Installation

```bash
cd magicmoused
make
make test
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now magicmouse
```

`make install` puts the daemon and `magicmouse-dump` into `/usr/local/bin`, the config into `/etc/magicmouse/magicmouse.conf` (an existing one is kept) and the unit into `/etc/systemd/system/magicmouse.service`.

Check that the mouse was picked up:

```bash
journalctl -u magicmouse -b --no-pager | tail -5
```

You should see `connected: Magic Mouse ... multitouch on, 1 input node(s) grabbed`.

Then, in **Settings → Mouse & Touchpad**, a **Touchpad** section appears: that is the driver's virtual touchpad. Turn on **Natural Scrolling** there for the macOS direction. Pointer speed is set in the **Mouse** section as usual.

Make sure nothing else drives the mouse at the same time (an older driver, a gesture script, `ydotool`-based tools):

```bash
pgrep -a magicmouse
```

Only `/usr/local/bin/magicmoused` should be listed.

### Updating

```bash
make clean && make && make test
sudo make install && sudo systemctl daemon-reload && sudo systemctl restart magicmouse
```

Your `/etc/magicmouse/magicmouse.conf` is kept. In the shipped file every option is commented out, so new defaults apply automatically. If your file comes from an older version and still has explicit values, replace it once:

```bash
sudo install -m644 magicmouse.conf /etc/magicmouse/magicmouse.conf && sudo systemctl reload magicmouse
```

## Configuration

Edit `/etc/magicmouse/magicmouse.conf`. Every option is listed there with its default and a description. Uncomment what you want to change, then:

```bash
sudo systemctl reload magicmouse
```

`magicmoused -p` prints the effective settings and reports invalid lines.

| Option                  | Default      | Meaning                                                              |
|-------------------------|--------------|----------------------------------------------------------------------|
| `scroll_speed`          | `1.0`        | Scrolling speed                                                      |
| `scroll_accel`          | `1.0`        | Extra distance for fast strokes (`0` = none)                         |
| `momentum`              | `1`          | Inertia by the driver; `0` leaves it to the apps                     |
| `momentum_time_ms`      | `500`        | How long the inertia coasts                                          |
| `axis_lock`             | `1`          | Keep a stroke on the axis it started on                              |
| `horizontal_swipe`      | `scroll`     | `keys`: a one-finger sideways stroke presses Alt+Left/Right          |
| `two_finger_swipe`      | `native`     | `keys`: Super+PageUp/PageDown, `off`: disabled                       |
| `two_finger_vertical`   | `overview`   | `scroll`: two fingers up/down scroll instead, `off`: nothing         |
| `swipe_speed`           | `1.6`        | How far workspaces move per millimetre of finger travel              |
| `one_finger_double_tap` | `smart-zoom` | `smart-zoom`, `none` or a key combination such as `CTRL+ALT+T`       |
| `two_finger_double_tap` | `SUPER`      | Same                                                                 |
| `secondary_click`       | `right`      | `left` for left-handed use, `off` for left click only                |
| `middle_click`          | `0`          | `1`: a click with one finger on each half is a middle click          |
| `scroll_mode`           | `touchpad`   | `wheel`: fallback, a high-resolution wheel with inertia              |

## How it works

```
/dev/hidrawN ─► report.c ─► engine.c ─► vinput.c (uinput)
 report 0x12    decoding    gestures    ├─ mouse:     pointer and clicks
                                        ├─ touchpad:  scrolling, swipes, pinch
                                        └─ keyboard:  shortcuts
```

- The daemon reads the touch surface from `hidraw` and takes the kernel's input device for the mouse over (`EVIOCGRAB`), so the kernel's coarse wheel emulation and its middle-click zone go silent. If the daemon stops, the mouse immediately falls back to the kernel driver.
- **Scrolling.** A finger on the mouse drives two fingers on a virtual touchpad. Applications receive real touchpad scrolling: pixel-precise, with the desktop's natural-scrolling setting. When the finger leaves the surface while moving, the driver keeps the virtual fingers gliding and slowing down, so inertia works in every application, not only in GTK and Firefox. Touching the mouse stops it, and quick repeated flicks build up speed.
- **Two-finger swipes** become the three-finger touchpad gesture GNOME already understands. Three virtual fingers land together as soon as both real fingers have moved 1.2 mm the same way. The axis is locked, because the curved surface makes sideways swipes drift. A quick flick keeps gliding for a moment, so GNOME sees its speed and completes the switch.
- **Clicks** follow the macOS rule: a click is secondary only if every finger on the surface is on the right half. A thumb gripping the side and a palm resting on the back are ignored.
- **No accidental scrolling.** Scrolling does not start while the mouse is being moved or a button is pressed: the finger always rolls a little then.
- **libinput-safe.** A virtual contact that does not move is held longer than libinput's tap timeout, so tap-to-click never produces a phantom right click. Frames are at least 4 ms apart, so a Bluetooth burst never looks like a jump that libinput would discard.
- The virtual devices are created once and survive the mouse going to sleep and reconnecting. Only one instance can run.

## Troubleshooting

**The cursor stutters or lags, even with the driver stopped.** The reports arrive late over Bluetooth, and no driver can fix that. With many adapters, Intel AX200/AX201 among them, USB power saving of the Bluetooth adapter is the cause. Turn it off for the running system:

```bash
echo on | sudo tee "$(readlink -f /sys/class/bluetooth/hci0/device/..)/power/control"
```

If that helps, make it permanent (takes effect after a reboot):

```bash
echo 'options btusb enable_autosuspend=n' | sudo tee /etc/modprobe.d/btusb-no-autosuspend.conf
```

Other things that cause Bluetooth stutter: Wi-Fi on 2.4 GHz on a combined Wi-Fi/Bluetooth card (use 5 GHz), USB 3 devices next to the adapter, and a low mouse battery.

**Driver health.** The daemon logs a `health:` line after about 20 seconds of use and then every 10 minutes:

```bash
journalctl -u magicmouse -b | grep health
```

For a fresh one right now:

```bash
sudo pkill -USR1 -x magicmoused; sleep 0.3; journalctl -u magicmouse -n 1 --no-pager
```

```
health: 89 reports/s while in use, max gap 18 ms, 0 gaps > 25 ms (of 880), queued <= 1, driver cpu 0.15%
```

`driver cpu` is the daemon's own CPU use, normally a fraction of a percent. `queued` above 2 would mean the driver falls behind. Many `gaps > 25 ms`, or a `max gap` above about 50 ms, means the mouse delivers its data late over Bluetooth.

**Scrolling goes the wrong way.** Toggle Natural Scrolling in the **Touchpad** section of GNOME Settings. If only one axis is wrong, set `invert_x = 1` or `invert_y = 1`.

**No scrolling or swipes at all.** The virtual touchpad follows GNOME's touchpad settings, and those apply to every touchpad. Make sure the touchpad is not switched off, two-finger scrolling is selected (not edge scrolling) and three-finger drag is off. To turn off a laptop's built-in touchpad only while a mouse is connected:

```bash
gsettings set org.gnome.desktop.peripherals.touchpad send-events 'disabled-on-external-mouse'
```

To check that the virtual devices exist:

```bash
sudo libinput list-devices | grep -A2 magicmoused
```

**A one-finger sideways swipe does not go back in Firefox.** In `about:config`, `widget.disable-swipe-tracker` must be `false`. For a browser without touchpad swipe navigation, set `horizontal_swipe = keys`.

**"magicmoused is already running".** Only one instance may drive the mouse. Stop the service before running it by hand.

**Watching what the driver sees:**

```bash
sudo systemctl stop magicmouse && sudo magicmoused -v
```

The service must be stopped first. `-v` logs touches, clicks, gestures and a `health:` line every 10 seconds. Afterwards, start the service again with `sudo systemctl start magicmouse`.

Raw touches, with edge and palm zones marked:

```bash
sudo magicmouse-dump
```

## Uninstall

```bash
sudo make uninstall
```

The kernel driver takes the mouse back at once. The config is left in `/etc/magicmouse/`.

## Development

```
src/report.c    HID report decoding
src/engine.c    gestures, scrolling, inertia, clicks, taps
src/config.c    defaults and config file
src/vinput.c    uinput devices
src/device.c    finding the mouse, multitouch mode, grabbing the kernel device
src/main.c      main loop, reconnects, health statistics, reload
src/dump.c      magicmouse-dump
tests/          engine tests
```

`report.c`, `engine.c` and `config.c` do not depend on Linux. `make test` runs 28 scenarios on synthetic HID reports: scrolling, acceleration, inertia and stopping it, clicks, taps, a double click that must not be a tap, swipes and flicks, palm rejection, dragging, mouse movement, Bluetooth bursts. One of them is a fuzz run of 20,000 random touches and clicks in four modes, checking that timers never spin and nothing stays pressed. The tests also build on macOS.
