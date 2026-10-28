# drakonia-linux

Configure the **Sharkoon Drakonia** gaming mouse without the vendor software, on
**Linux and Windows**. [Version française](README.fr.md)

- `drakonia.py`: a small command-line tool (Python, works on both systems)
- `libratbag/`: a driver for [libratbag](https://github.com/libratbag/libratbag), so the
  mouse can be set up in [Piper](https://github.com/libratbag/piper), the graphical
  mouse tool on Linux
- `PROTOCOL.md`: the full USB protocol, reverse-engineered from captures

Everything the official software does with the mouse itself is supported: logo color,
effect and brightness, DPI, polling rate, the 11 buttons and macros.

## Status

| | |
|---|---|
| `drakonia.py` | tested on a real mouse (under Windows) |
| libratbag driver | builds, produces the same bytes as the tested script, **not yet tested on a real mouse** |

## The command-line tool

Requires Python 3 and hidapi:

```
pip install hidapi
```

- **Windows**: close the Sharkoon software first (tray icon near the clock).
- **Linux**: run with `sudo`, or install the udev rule once so your user can access the mouse:
  ```
  sudo cp 99-sharkoon-drakonia.rules /etc/udev/rules.d/
  sudo udevadm control --reload && sudo udevadm trigger
  ```

Examples:

```
python drakonia.py selftest              # checks the encoding, touches nothing
python drakonia.py color 0 0 255         # blue logo
python drakonia.py lighting steady high  # steady|slow|medium|fast  off|low|medium|high
python drakonia.py dpi 2000              # 500 1000 1500 2000 3000 4000 5000
python drakonia.py rate 1000             # 125 250 500 1000 Hz
python drakonia.py sensitivity 5         # 1 (slow) .. 10 (factory); optional 2nd value for Y
python drakonia.py buttons               # list buttons and actions
python drakonia.py factory               # factory buttons and macros
python drakonia.py --dry-run dpi 2000    # print the bytes, send nothing
```

### Buttons and macros

The mouse always receives **all buttons and all macros at once**, and it cannot be read
back. So describe your whole layout, in a file or on the command line; buttons you do
not mention get their factory action.

```
python drakonia.py config "10=keys ctrl+c" "11=keys ctrl+v" "6=text hello"
python drakonia.py config --file examples/my-mouse.txt
```

| # | Button | # | Button |
|---|---|---|---|
| 1 | left click | 7 | wheel tilt right |
| 2 | right click | 8 | DPI+ |
| 3 | wheel click | 9 | DPI− |
| 4 | forward | 10 | right side button |
| 5 | back | 11 | left side button |
| 6 | wheel tilt left | | |

Actions: `left`, `right`, `middle`, `back`, `forward`, `tilt-left`, `tilt-right`, `dpi+`,
`dpi-`, `disable`, `double-click`, `color-cycle`, `keys <shortcut>` (e.g. `keys ctrl+shift+t`),
`text <text>`.

Keys are physical positions. On a French AZERTY keyboard, add `--azerty` so that
`text bonjour` types what you expect.

## The libratbag driver (Piper)

The driver is in `libratbag/` as a patch for libratbag, plus the two new files.

```
git clone https://github.com/libratbag/libratbag
cd libratbag
git am /path/to/drakonia-linux/libratbag/0001-Add-a-driver-for-the-Sharkoon-Drakonia.patch
meson setup builddir && ninja -C builddir && sudo ninja -C builddir install
sudo systemctl restart ratbagd
```

Then open Piper. Notes:

- The mouse cannot be read back, so Piper shows the factory configuration at start,
  not the one currently stored in the mouse.
- The right side button's factory action (logo color cycle) has no libratbag
  equivalent and shows as "unknown"; it is kept as long as you don't change it.
- Delays inside macros are ignored (their encoding is unknown).

Once tested on real hardware, the driver will be proposed upstream to libratbag.

## Not done / unknown

- reading the configuration back from the mouse
- delays inside macros
- the meaning of the `02 01` and `03 01` messages (see `PROTOCOL.md`)
- the sensitivity setting is not in the libratbag driver (Piper has no such setting)
- mouse speed, scroll speed and double-click speed are Windows/KDE settings, not mouse settings

Reports and captures from other Drakonia owners are welcome.

## Legal

Not affiliated with or endorsed by Sharkoon. "Sharkoon" and "Drakonia" are trademarks of
their owners. The protocol was studied for interoperability only; no vendor code or file
is included in this repository.

MIT license, see [LICENSE](LICENSE).
