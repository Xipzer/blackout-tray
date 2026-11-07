# Blackout

A tiny Windows tray app. Click it and every screen turns off. Lighting, fans, monitor power and window positions follow the screens.

## What it does

| Screens off | Screens on |
|---|---|
| RGB off | RGB back on |
| Fans to the "screens off" speed | Fans to the "screens on" speed |
| Listed monitors powered off over DDC/CI | Those monitors powered on |
| Window positions saved | Windows put back where they were |

This works for a Blackout click and for Windows' own display timeout.

**Left-click** the icon: screens off.
**Right-click**: RGB off/on, RGB follows screens, Fans, Start with Windows, Exit.

## Supported hardware

| Feature | Hardware | Needs |
|---|---|---|
| RGB | Anything [OpenRGB](https://openrgb.org) supports | OpenRGB portable |
| Pump/fan RGB | Corsair iCUE LINK System Hub (XD5, QX, LX...) | Nothing |
| RAM RGB | Kingston FURY DDR5 | FURY module + Kingston FURY CTRL |
| Fan speed | Corsair Commander Pro | Nothing |
| Monitor power | Any monitor with DDC/CI | Nothing |

Every feature is optional. Missing hardware is skipped.

## Install

1. Download `blackout.exe` from [Releases](../../releases) into a folder, for example `%LOCALAPPDATA%\Blackout`.
2. Copy `blackout.example.ini` there as `blackout.ini` and edit it. No ini means screens off and fans only.
3. Run `blackout.exe`. Right-click the icon and tick **Start with Windows**.

For RGB, also unzip [OpenRGB](https://openrgb.org/releases.html) portable to the path in `blackout.ini`.

## Configuration

`blackout.ini` sits beside `blackout.exe`. See [blackout.example.ini](blackout.example.ini).

| Section | Key | Meaning |
|---|---|---|
| `[rgb]` | `openrgb` | Path to `OpenRGB.exe`. Empty skips OpenRGB. |
| | `off`, `on` | OpenRGB arguments for each state |
| | `color` | Hex colour for the iCUE LINK hub and RAM when on |
| `[link]` | `leds` | LED count on the iCUE LINK chain. 0 disables it. |
| `[ddc]` | `monitors` | Monitor hardware IDs to power off, comma separated (e.g. `BNQ7F2F`) |

Fan speeds and toggles are set from the tray menu and kept in `HKCU\Software\Blackout`.

To find a monitor's hardware ID: Device Manager > Monitors > Properties > Details > Hardware Ids.

## FURY module (optional)

For Kingston FURY DDR5 RGB only. OpenRGB can't detect some of these kits, so this talks to Kingston's own service instead.

1. Install Kingston FURY CTRL. Its background service does the hardware access.
2. Put `furyled.exe` beside `blackout.exe`. Delete it to turn the module off.

Test it: `furyled off` or `furyled FF0000`.

## Resource use

Measured on Windows 11.

| Process | When | Memory | CPU |
|---|---|---|---|
| `blackout.exe` | Always | 0.2 to 0.4 MB | 0% idle |
| iCUE LINK keep-alive | Every 4 s, only with `[link]` | 5 small USB transfers | ~0% |
| `OpenRGB.exe` | 3 to 5 s per change, then exits | ~100 MB while running | Brief |
| `furyled.exe` | Under 1 s per change | ~10 MB while running | Brief |
| Kingston FURY CTRL service | Always, if installed | ~40 MB | Kingston's |

## Setup notes

- **OpenRGB with Corsair Commander Pro or Lighting Node Pro:** set each channel's LED count once in OpenRGB (Zone Initialization). Static mode won't light a zone of size 0.
- **OpenRGB with an iCUE LINK hub:** disable OpenRGB's "Corsair iCUE Link System Hub" detector. Blackout drives the hub itself.
- **Screens off** briefly sets the Windows display timeout to 1 second, then restores your value.

## Build

Needs [zig](https://ziglang.org) and Python 3 with Pillow.

```sh
./build.sh            # build/blackout.exe
```

FURY module, on Windows: `modules\fury\build.cmd`.

## License

[PolyForm Noncommercial 1.0.0](LICENSE.md). Free to use, change and share. Not for sale.

## Credits

Protocol details from [OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB) (iCUE LINK) and [liquidctl](https://github.com/liquidctl/liquidctl) (Commander Pro).
