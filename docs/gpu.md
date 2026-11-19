# GPU profiles without a tuning app

Blackout can hold an NVIDIA overclock itself: a clock curve, a power limit, a memory offset and an XBAR (crossbar clock) offset.
It writes them to the driver through `nvapi64.dll`, the library the driver installs, so MSI Afterburner or a similar app does not have to run.

Tested on one card: RTX 5090 Founders Edition, driver 621.24, Windows 11. Treat everything else as untested.

## What you need to know first

- **These are private driver interfaces.** NVIDIA does not document them. The community worked them out, and a future driver can change or block them. Keep your tuning app installed until you trust this.
- **Blackout applies a profile, it does not find one.** Tune and stress test in your usual app, then save the result (below).
- **The driver forgets everything** when it resets (a crash) or the PC restarts. Blackout sends the profile 10 seconds after it starts, 10 seconds after the PC wakes from sleep and 15 seconds after a driver reset.
- **Writing needs admin.** Reading does not.
- **Only the first NVIDIA GPU** is used.

## Set it up

1. Apply your tested overclock with your tuning app.
2. Open the panel (right-click the tray icon), Settings > GPU, and press **Read from card**. It reads the card once and writes `[gpu.1]` to `blackout.ini`. **+** is the next empty profile, up to five.
3. Click the profile's name to name it. Optionally add `xbar=0,300,400` under `[gpu]` for the XBAR stepper.
4. Stop your tuning app from starting with Windows.

To carry over several profiles from a tuning app, load each one there, press **+** here and read it in. To change a profile later, show it, press **Read from card** and confirm.
The profile read from the card becomes the active one, because it is what the card is running.

The same from a command line: `blackout.exe --gpu-save 1` (to `5`), then restart Blackout.

## In the panel

- A profile pill only shows that profile. **Apply** sends it and makes it the one that is sent again after a restart or a driver reset.
- Red means on the card: the pill, and the curve. A profile that is only being looked at is drawn without colour.
- The chart is the profile's clock curve (MHz over volts) over the card's own curve in grey. The marker is the top of the curve at the lowest voltage that reaches it. The card's own curve is known once a profile has been sent or read.
- Under the chart: power limit in % of the card's default and memory offset in MHz. **XBAR offset** is the row below them, with a minus and plus; it belongs to the card, so it stays the same when you change profile.
- The bottom line is what the driver answered: `Applied at 09:15`, or why it was not, for example `Memory offset refused (-104)`.

```ini
[gpu]
xbar=0,300,400,500
xbar_default=400

[gpu.1]
name=Maximum
power=121739
memory=3000000
curve=480000,480000,...
```

| Key | Meaning |
|---|---|
| `power` | Power limit in thousandths of a percent of the card's default (100000 = 100 %). Clamped to what the card allows |
| `memory` | Memory clock offset in kHz, as the driver counts it (+3000 MHz in Afterburner = 3000000) |
| `curve` | One frequency offset in kHz per core voltage point, lowest voltage first. The count must match the card |

Commands, each writing its answer to `%TEMP%\blackout-gpu.txt`:

| Command | What it does |
|---|---|
| `--gpu-status` | Reads the card: points, power limit, memory offset, XBAR offset, a sample of the curve |
| `--gpu-save N` | The same, then saves it as profile N (what **Read from card** does) |
| `--gpu-apply N` | Applies profile N now (needs admin) |

## How it is applied

The order is fixed, because of how the driver behaves:

1. Power limit.
2. Memory offset. **This write zeroes every curve offset** (measured on 621.24), so it has to come before the curve.
3. XBAR offset.
4. Clock curve, then a read back that must match.

If a step is refused, Blackout stops there. The card is then on the stock curve, never on a half-written one. The GPU tab shows what the driver answered.

## The interfaces

All through `nvapi_QueryInterface(id)` in `nvapi64.dll`. Sizes are in bytes; the version word is `size | version << 16`.

| What | Read | Write | Buffer |
|---|---|---|---|
| Curve, what each slot is | `0x507B4B59` | | `0x182C`, v1. Mask at +4, 255 points of 24 bytes from `0x44`: type at +0 (0 = core), set-by-voltage flag at +4 |
| Curve, offsets | `0x23F1B133` | `0x0733E009` | `0x2420`, v1. Mask at +4 (copied from above), 255 points of 36 bytes from `0x44`, signed offset in kHz at +20 |
| Power limit | `0x70916171` (range: `0x34206D86`, 184 bytes) | `0xAD95F5ED` | 72 bytes, v1. Value at `0x10` |
| Memory offset | `0x6FF81213` | `0x0F4DAE6B` | 7416 bytes, v3 (the public `NV_GPU_PERF_PSTATES20_INFO`). Top state, clock domain 4 |
| XBAR offset | `0xF58938F5` | `0xD14B69CF` | `0x13000` bytes, version word `0x000261A4`, domain mask `0xFF` at +8. Domains of `0x304` bytes from `0x124`; XBAR is domain 1, signed offset in kHz at +`0x114` |

Rules that cost us time:

- Read, change, write back. A zeroed buffer is silently ignored.
- On this card 127 of the 128 used slots are core points. The other one is not (it reads like a memory point: 405 MHz at 530 mV), so count core points from the type fields, not from the mask.
- A memory offset set by Afterburner did not show in the memory read above (it read 0 at +3000). Two non-core slots of the curve table held offsets instead, which is our guess at where it lives. `--gpu-save` takes the larger reading.
- Error -137 means the process is not elevated.

## Sources

The layouts come from these projects and write-ups, checked again on our card before use:

- XBAR and the clock domains table: https://github.com/SHANAjam/rtx5090-xbar-control (MIT)
- Curve tables: https://github.com/arcnmx/nvapi-rs (MIT), https://vuplea.github.io/simple-nvidia-undervolt/DEVELOPMENT.html
- Memory through Pstates20 and the curve reset it causes: https://erfi.dev/reference/vfctl-gpu-curve-tool/
- The Afterburner developer naming the interfaces: https://www.overclock.net/posts/29608363/
