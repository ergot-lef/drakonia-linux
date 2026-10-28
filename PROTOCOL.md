# Sharkoon Drakonia USB protocol

Reverse-engineered from USB captures (USBPcap) of the official "Drakonia Configurator"
software, and checked on a real mouse. Everything marked **verified** was tested on
the device; the rest is marked as unknown.

## Device

| | |
|---|---|
| USB ID | `04d9:a067` (Holtek) |
| Interface 0 | keyboard (usage page 0x01, usage 0x06): macros are typed through it |
| Interface 1 | mouse, consumer keys, and vendor pages `0xff00` / `0xff01`. Carries **feature report 7** (settings) |
| Interface 2 | vendor page `0xff00`, no report IDs. Receives **output reports of 8 bytes** (data blocks) |

The device never answers: no GET_REPORT was ever seen, so the configuration cannot be
read back. Settings are kept in the mouse after unplugging (verified).

## Scrambling

Every message carries 7 scrambled bytes. The scrambler is the inverse of the function
`EncodeSecrecy_V11` of the vendor DLL. On the 7 clear bytes `d[0..6]`:

1. `d[i] += swap_nibbles(K1[i])` with `K1 = "oNgtEng"` (mod 256)
2. rotate the 56-bit big-endian value left by 3 bits
3. XOR with a session key, which is all zeros
4. swap `d0↔d3`, `d2↔d6`, `d4↔d5`

Unscrambling applies the steps in reverse order. 12 real messages are used as test
vectors in `drakonia.py selftest`.

Example: clear `09 03 00 00 00 00 00` (DPI step 3) → on the wire `07 3a 3b b7 ff 33 a7 b2`.

## Settings (interface 1, feature report 7)

8 bytes: `07` followed by the 7 scrambled bytes below (zero padded).
Each command works on its own (verified); the vendor software waits 16 ms between messages.

| Command | Meaning | Values | Status |
|---|---|---|---|
| `01 n` | polling interval in ms | `08`=125 Hz, `04`=250, `02`=500, `01`=1000 | verified |
| `09 n` | DPI step | `01`..`07` = 500, 1000, 1500, 2000, 3000, 4000, 5000 | verified |
| `0A r g b` | logo color | RGB, 0–255 each | verified |
| `0B n` | logo effect | `01` steady, `02` slow pulse, `03` medium pulse, `04` fast pulse | verified |
| `0C n` | logo brightness | `01` off, `02` low, `03` medium, `04` high | verified |
| `04 01 40` | announces the button table (64 bytes) on interface 2 | | verified |
| `05 01 00 06` | announces the macro area (6×256 = 1536 bytes) on interface 2 | | verified |
| `12 x y` | sensitivity, X and Y axes | `01` (slowest) .. `0A` (factory, full speed); values above `0A` are out of range (`14` made it very slow) | verified |
| `02 n` | operating mode | `01` normal; `00` sensor off (no movement, no DPI buttons, clicks still work); `02` all reports off | verified |
| `03 n` | sent first by the vendor software (`03 01`) | `00`, `01`, `02`: no visible effect | unknown |

`x` is the horizontal axis, `y` the vertical one (verified: `12 01 0A` makes left-right
movement slow and leaves up-down movement at full speed).

The vendor "Sensitivity" slider sends the same value for both axes. With "Enable advanced
sensitivity" ticked, the general slider is locked and the two axis values are sent as they
are, without combining them. Note: the vendor software swaps its labels. With
"X-Axis" = 3 and "Y-Axis" = 8 it sends `12 08 03`, so its "X-Axis" slider actually
controls the vertical axis.

Mouse speed, scroll speed, double-click speed and the vendor "profiles" are handled on the PC side
by the vendor software, not by the mouse.

## Data blocks (interface 2, output report without ID)

Each block is cut in 8-byte packets. **Byte 0 is sent in clear, bytes 1..7 are
scrambled** like the settings. With hidapi or hidraw, write `00` (report number) + 8 bytes.

### Button table (64 bytes)

11 entries of 4 bytes, then 20 zero bytes. Order of the physical buttons:

| # | Button | Factory entry | Meaning |
|---|---|---|---|
| 1 | left click | `00 F0 00 00` | mouse button 1 |
| 2 | right click | `00 F1 00 00` | mouse button 2 |
| 3 | wheel click | `00 F2 00 00` | mouse button 3 |
| 4 | forward | `00 F4 00 00` | mouse button 5 (forward) |
| 5 | back | `00 F3 00 00` | mouse button 4 (back) |
| 6 | wheel tilt left | `04 00 00 F3` | horizontal scroll left |
| 7 | wheel tilt right | `04 00 00 F4` | horizontal scroll right |
| 8 | DPI+ | `71 01 00 00` | next DPI step |
| 9 | DPI− | `71 02 00 00` | previous DPI step |
| 10 | right side button | `72 01 00 00` | cycles the logo color (blue → yellow → red) |
| 11 | left side button | `53 00 0A 0A` | macro in slot 10 (factory: double click) |

The role of each physical button was identified on the device; disabling a button
and assigning macros were verified.

- `00 00 00 00` disables the button (verified).
- `53 00 ss ll` runs the macro in slot `ss`; `ll` = 2 × number of events + 2.
  The vendor software always uses slot *button number − 1*.
- `04 00 00 xx` sends the USB HID consumer usage `xx` (media keys, see below).

### Every action of the vendor menu

Read from the vendor software (the function that builds the button table), and checked
against the captures where available.

| Menu | Entry | Notes |
|---|---|---|
| Click / Menu / Middle Button | `00 F0` / `00 F1` / `00 F2` `00 00` | |
| Backward / Forward | `00 F3 00 00` / `00 F4 00 00` | |
| Double Click | macro: left button pressed and released twice | |
| Bind Key | `00 uu 00 00` | `uu` = keyboard usage, no macro needed (not tested) |
| Macro | `53 00 nn ll` | |
| Disable | `00 00 00 00` | |
| Advanced › DPI+ / DPI− | `71 01 00 00` / `71 02 00 00` | |
| Advanced › LED Color Switch | `72 01 00 00` | |
| Advanced › Scroll Left / Right | `04 00 00 F3` / `04 00 00 F4` | |
| Advanced › Switch Window, Close Window | macros Alt+Tab, Alt+F4 | |
| Advanced › Explorer, Run, Show Desktop, Lock | macros Win+E, Win+R, Win+D, Win+L | |
| Basic › Cut, Copy, Paste, Select All, Find, New, Print, Save | macros Ctrl+X, C, V, A, F, N, P, S | also Ctrl+Z (undo), not shown in the menu |
| Media › Play/Pause, Stop, Previous, Next | `04 00 00` + `CD`, `B7`, `B6`, `B5` | |
| Media › Volume Up, Volume Down, Mute | `04 00 00` + `E9`, `EA`, `E2` | volume verified |

The vendor macros use fixed key positions (QWERTY), so its "Select All" sends Ctrl+Q on a
French AZERTY layout.

The software also knows three codes that its menu does not show: `00 F8 00 00`,
`00 F9 00 00` and `04 00 00 F6`. Assigned to a button, none of them had a visible
effect under Windows.

### Macro area (1536 bytes)

16 slots of 96 bytes. Slot `n` starts at `n × 96`:

| Offset | Content |
|---|---|
| +0 | `00` |
| +1 | `01` |
| +2… | pairs `(flag, code)`: flag `00` = press, `80` = release |

`code` is a USB HID keyboard usage (`04` = A position, `E0` = left Ctrl…) or `F0` for the
left mouse button (used by the factory double click). Up to 47 events per slot.

Examples (verified):

- Ctrl+C: `00 01 | 00 E0 | 00 06 | 80 06 | 80 E0`, entry `53 00 nn 0A`
- double click (factory slot 10): `00 01 | 00 F0 | 80 F0 | 00 F0 | 80 F0`

Usages are key *positions*: on a French AZERTY layout, usage `04` types "q".

Delays between macro events: encoding unknown.

## Sequence of the vendor software ("Apply")

`03 01`, `02 01`, `12 0A 0A`, `04 01 40` + button table, `05 01 00 06` + macro area,
`0A r g b`, `0B mode`, `0C brightness`, `09 dpi`. The polling rate (`01 n`) is sent alone.
Neither `03 01` nor `02 01` is required while the mouse is in normal mode, but sending
`02 01` first (as the vendor software does) brings a mouse left in another mode back to
normal. The factory reset of the vendor software sends exactly this sequence, with a red
logo (`0A FF 00 00`), fast pulse, high brightness and 1500 dpi.
