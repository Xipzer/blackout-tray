# Device definitions

Blackout learns about fan hardware from small text files in the `devices` folder beside `blackout.exe`. One file describes one motherboard fan chip or one USB controller: where its fan and temperature registers are, or which bytes it understands. Supporting new hardware means adding a file, not changing the program.

At startup Blackout reads the folder, keeps the definitions that `blackout.ini` uses, and forgets the rest. Hardware you don't use costs nothing while Blackout runs.

## Using a definition

Run `blackout.exe --devices` as admin. It writes what it finds to `%TEMP%\blackout-devices.txt` and opens it:

```
Config port 2E: chip D42A at 2A0, Nuvoton NCT6796D-R (nuvoton-nct6796d-r.ini, verified)
  fan  1   Fan 1                   1021 rpm   duty 100%   mode 44   can be controlled
  fan  2   Fan 2                   1350 rpm   duty  40%   mode 04   can be controlled
  temp  1  SYSTIN                 31.0 C
```

Then name the fans to drive, and the temperature that drives them, in `blackout.ini`:

```ini
[fans]
; Fans: <definition>/<fan number or name>. sio is the motherboard fan chip that was found.
outputs=sio/2,sio/4,sio/5
; Temperature: link (default) is the iCUE LINK hub's coolant sensor. Otherwise <definition>/<name or number>.
source=sio/AUXTIN2
; Allow fan control through a definition marked status=untested.
untested=1
```

A definition is named by its file name without `.ini`, for example `corsair-commander-pro/1`. Up to 7 outputs.

## Safety

- `status=untested` means the file was written from documentation and other open source drivers and has not been run on that hardware. Blackout shows such a device in `--devices` but does not take control of its fans unless `untested=1` is set.
- Before it takes a motherboard fan, Blackout saves the registers it is about to change. It puts them back on exit, sleep, shutdown, when the curve is switched off and when the temperature reading is lost for 20 s.
- The saved values are also noted in the registry until shutdown (`HKLM\SOFTWARE\Blackout\Held`, volatile). If Blackout is killed while it holds a fan, the next start can still hand it back as the BIOS had it.
- A USB controller has no BIOS curve to return to, so its fans are set to 100% in those cases.
- A chip is asked for its ID before anything is written to it. Fan chips that the BIOS leaves in config mode (the second chip on some Gigabyte boards) are left exactly so.
- `guard=` lets a definition refuse control when the chip is not in the state its registers assume.
- Reading fans and temperatures never writes a register.

When a definition works on your hardware, please open a pull request that sets `status=verified` and adds your board or device to `tested=`.

## Common keys

```ini
[device]
name=Nuvoton NCT6796D     ; shown in the panel and in --devices
type=sio                  ; sio = motherboard fan chip, hid = USB controller
status=untested           ; verified | untested
tested=                   ; boards or devices it was run on (free text)
source=                   ; where the numbers come from (free text)
```

Registers and bytes are hexadecimal without a prefix. Counts, offsets, bit numbers and divisors are decimal. Text after `;` is a comment. Files are plain ASCII, named with `a-z`, `0-9` and `-`. Names of fans and temperatures are up to 23 characters, and one file holds up to 16 of each.

## type=sio (motherboard fan chips)

Needs PawnIO and `LpcIO.bin`, and Blackout running as admin.

```ini
[device]
name=ITE IT8689E
type=sio
status=untested
family=ite                ; how the chip is reached, see the table below
chip=8689                 ; start of the 16-bit chip ID (config registers 20 and 21), 2 to 4 digits.
                          ; D42 matches D420 to D42F. Up to four: chip=D420,D423. The longest match wins.
ldn=04                    ; logical device number of the hardware monitor
iolock=1                  ; nuvoton: clear bit 4 of config register 28 (NCT6791D and newer)
guard=06:60=00,14:87!=00  ; optional, up to two. Control only while (register AND mask) = value, or != value.

[fans]
; name = duty:<reg> mode:<reg> manual:<mask>=<value> enable:<reg>.<bit> rpm:<reg>
Fan 1=duty:63 mode:15 manual:FF=7F enable:13.0 rpm:0D,18

[temps]
; name = <reg> or <reg>,<reg>.<bit>
Temp 1=29
CPUTIN=150,151.7
```

| Field | Meaning |
|---|---|
| `duty` | Register that takes the duty, 0 to FF |
| `mode` | Register that selects automatic or manual control |
| `manual` | To take the fan, Blackout writes `(old AND NOT mask) OR value` to `mode`. On hand-back the old value goes back whole |
| `enable` | Optional. A bit that must be set while the fan is driven. Blackout sets it and puts it back |
| `rpm` | Where the speed is read. The family decides how |

`duty`, `mode` and `manual` go together. A fan with only `rpm` is shown and never controlled.

A temperature is a signed 8-bit register in degrees C. The optional second part names a register and bit that adds half a degree.

| Family | Chips | Enter and leave config mode | Register access (base = `60`/`61` of the logical device) | `rpm` | Control |
|---|---|---|---|---|---|
| `nuvoton` | NCT6779D, NCT679x | `87 87`, then `AA` | Bank in register 4E, index at base+5, data at base+6. Registers are 3 digits: bank, index | `rpm:R`: RPM, high byte at R, low at R+1 | Yes |
| `ite` | IT86xx, IT87xx | `87 01 55 55` (port 2E) or `87 01 55 AA` (port 4E), then register 02 = 02 on port 2E only | Index at base+5, data at base+6 | `rpm:LOW,HIGH`: a count, RPM = 1350000 / (2 x count) | Yes |
| `fintek` | F718xx, F818xx | `87 87`, then `AA` | Index at base+5, data at base+6 | `rpm:R`: a count at R, R+1, RPM = 1500000 / count | Read only |
| `nct6683` | NCT6683D, NCT6686D, NCT6687D | `87 87`, then `AA` | Page at base+4, index at base+5, data at base+6 | `rpm:R`: RPM at R, R+1 | Read only |

Fintek and NCT668x chips need steps to take control that this format can't describe yet (a mode register shared by all fans, a request and commit sequence), so their definitions list `rpm` only.

Blackout looks behind config port 2E, and behind 4E only when `blackout.ini` names something that is not on the first chip.

## type=hid (USB controllers)

```ini
[device]
name=Corsair Commander Pro
type=hid
status=untested
vid=1B1C
pid=0C10,1D00             ; up to eight
interface=                ; optional: text the device path must contain, such as mi_00 or col01
id=00                     ; report ID in front of every command. Needed with set=, rpm= and read=
out=64                    ; output report size in bytes, without the report ID. Needed with commands
in=16                     ; input report size in bytes, without the report ID

[fans]
count=6
set=23 {ch} {duty}        ; bytes that set a duty
rpm=21 {ch} > u16be@2     ; bytes to send, then where the answer is in the reply

[temps]
count=4
read=11 {ch} > s16be@2 /100
```

Commands are hexadecimal bytes and placeholders, up to 16: `{ch}` is the channel from 0, `{ch1}` from 1, `{duty}` is 0 to 100, `{duty255}` is 0 to 255. The rest of the report is zero. Fans and temperatures made by `count=` are named `Fan 1`, `Temp 1` and so on.

A reply field is `type@offset`, optionally followed by `/divisor`. Types: `u8`, `u16be`, `u16le`, `s16be`, `s16le`. The offset counts from the first byte of the report as Windows returns it, so byte 0 is the report ID. Drivers on other systems often count without it: add 1 to their offsets for a device that does not use report IDs.

`in=` and `out=` are checked against the fields and commands in the file. When the device is opened, Blackout uses the report sizes Windows reports for it.

Controllers that send status reports on their own use `listen`:

```ini
[temps]
listen=1                  ; take the newest report the controller sent, send nothing
match=01                  ; optional: the report must start with these bytes (up to four)
invalid=7FFF              ; optional: raw value that means no sensor is connected
Coolant=u16be@87 /100     ; one line per sensor: name = field
```

`[fans]` takes the same keys, with one `name = field` line per fan for its RPM. Without `set=` the fans are shown and never controlled.

Not supported yet: controllers that need an init command before they report (NZXT Smart Device V2, Kraken X3 and Z3), checksums, sequence numbers and feature reports.

## Checking a definition

`./build.sh` builds `build/devtest` and runs it over every file in `devices`. The build stops if a file does not parse, claims an ID another file claims, or fails on the simulated hardware: the chip must be found behind both config ports with nothing unexpected written, reading must write nothing, and taking a fan, changing its duty and handing it back must leave every register as it was.

To run it alone:

```sh
build/devtest devices/*.ini
```

It can't tell you whether the register numbers are right for the real chip. Only a run on the hardware can: check that `--devices` shows believable speeds and temperatures before you set `untested=1`.
