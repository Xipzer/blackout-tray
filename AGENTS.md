# AGENTS.md

Guide for coding agents working on this repo. Humans: see README.md.

## Rules

- Lowest possible resource use. No polling loops, no resident helpers, no runtimes in `blackout.exe`. Helpers run once and exit.
- Win32 C only in `src/`. No new dependencies beyond system DLLs.
- Docs: plain, short, no filler. Never use em dashes or en dashes in any file.
- Every feature must be optional and skip silently when its hardware or config is missing.
- Never write to the RAM SMBus directly. RAM goes through the FURY module only.

## Map

| Path | Purpose |
|---|---|
| `src/blackout.c` | The whole tray app |
| `src/blackout.rc` | Icon resource (icon generated at build time) |
| `tools/make_icon.py` | Draws `blackout.ico` (Pillow) |
| `modules/fury/furyled.cs` | Optional Kingston FURY DDR5 client (.NET Framework 4, C# 5) |
| `blackout.example.ini` | Config template |
| `build.sh` | zig cross-build to `build/blackout.exe` |

## Build and check

```sh
./build.sh                         # needs zig + python3-pillow; ZIG=/path/to/zig to override
grep -rn $'\u2014\|\u2013' . && exit 1   # must print nothing
```

FURY module builds on Windows only: `modules\fury\build.cmd` (uses the csc in `%WINDIR%\Microsoft.NET`).

## Architecture

One hidden window. Everything is driven by `GUID_CONSOLE_DISPLAY_STATE` (`WM_POWERBROADCAST`), which Windows sends on registration and on every display on/off, including the idle timeout.

- **Display off (0):** save window placements, DDC power off listed monitors, `setRgb(off)`, apply fan "screens off" duty.
- **Display on (1):** DDC power on, restore windows after 4 s (and 2.5 s after each `WM_DISPLAYCHANGE`), `setRgb(on)`, fan "screens on" duty.
- **Dimmed (2):** ignored.
- **Screens off click:** set AC video idle timeout to 1 s via powrprof, restore it on the next display-off event. Fallback after 6 s: `SC_MONITORPOWER`. The fallback leaves some iGPU DisplayPort outputs half awake, which makes monitors flash "No Signal"; that is what `[ddc]` fixes.
- **`setRgb`:** writes `RgbOn`, refreshes the LINK hub, spawns OpenRGB and furyled. Changes during a 6 s busy window are queued; last one wins.

State lives in `HKCU\Software\Blackout` (DWORDs): `RgbOn`, `Sync`, `FanCtl`, `FanAwake`, `FanSleep` (0 = same as awake). Config lives in `blackout.ini` beside the exe (`[rgb]`, `[link]`, `[ddc]`).

## Device protocols

**Corsair Commander Pro** (VID 1B1C, PID 0C10). 65-byte output report `[0, 0x23, channel 0..5, duty%]`. Write only. Resets to its default after power loss, so duty is re-applied on every display event.

**Corsair iCUE LINK System Hub** (VID 1B1C, PID 0C3F, interface MI_00). 513-byte output reports `[0, 0, 1, cmd..., data...]`, each answered by one 513-byte input report (overlapped I/O, 1 s timeouts). Colour sequence:

1. Software mode `01 03 00 02`
2. Close endpoint `05 01 01` + `22`
3. Open colour endpoint `0d 00` + `22`
4. Write colour `06 00` + `[len lo, len hi, 0, 0, 12, 00, RGB x leds]`, len = leds * 3 + 2
5. Close endpoint `05 01 01` + `22`

The hub falls back to its stored hardware lighting about 5 s after the last frame, so the frame is re-sent every 4 s (30 s back-off on failure). On exit Blackout sends hardware mode `01 03 00 01`. Speeds: endpoint `17`, data type `25 00`.

**DDC/CI.** VCP `0xD6`: 1 = on, 5 = off. Monitors matched by hardware ID from `EnumDisplayDevices(szDevice).DeviceID`. A monitor powered off this way stays enumerated, so it can be woken over DDC.

**Kingston FURY (module).** Kingston's `FURYController_Service` (Windows service, auto start) hosts a WebSocket at `ws://127.0.0.1:55599/`. Every message is base64 of `salt(32) | iv(32) | ciphertext`, Rijndael with 256-bit block, CBC, PKCS7, key = PBKDF2-SHA1(passphrase `3m23s45i599`, salt, 1000 iterations, 32 bytes). Requests are JSON:

- `{"root":{"api":"set_keep_status","keep":"1"}}` makes the service re-apply the last effect after resume.
- `{"root":{"api":"set_dram_led","ctrl_settings_ddr5":{"slot_0":{...},"slot_1":{...}}}}` with `mode` `static_color`, `ctrl_mode` `ctrl_color`, `brightness` 0 to 100, `color_table` `[[r,g,b]]`. Reply `"status":"0"` means applied.

Field list is in `modules/fury/furyled.cs`. The service also logs to `C:\Program Files (x86)\Kingston Fury\FuryCTRL_SDK\Log`.

## Verifying on a real machine

- Fans: `liquidctl --match "Commander Pro" status` shows RPM per channel.
- OpenRGB: each run writes `%APPDATA%\OpenRGB\logs\OpenRGB_*.log`. Look for `Registering RGB controller` and errors.
- FURY: `furyled off` prints the reply and exits 0 on success.
- Display cycle without touching the PC: send `SC_MONITORPOWER` 2 from a window, wait, then `mouse_event` to wake.

## Known pitfalls

- OpenRGB 1.1 fails to detect some Kingston FURY DDR5 kits even with admin and PawnIO. Its probing also interrupts the RAM effect. Disable its DRAM detectors when using the FURY module.
- A missing device in an OpenRGB `-d` list makes the whole CLI call fail (exit -1). Keep `off`/`on` to devices that are always present.
- Corsair channels with zone size 0 ignore Static colours.
- OpenRGB releases the iCUE LINK hub to hardware mode on exit, which fights the keep-alive. Disable its LINK detector.
