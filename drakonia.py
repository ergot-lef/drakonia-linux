#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ergot-lef
"""
drakonia.py - configure the Sharkoon Drakonia gaming mouse (USB 04d9:a067)
without the vendor software. Works on Linux and Windows.

Setup:   pip install hidapi
Linux:   run with sudo, or install the udev rule from the README.
Windows: close the Sharkoon software first (tray icon near the clock).

Examples:
  python drakonia.py selftest                 check the encoding, touches nothing
  python drakonia.py list                     show how the mouse is seen
  python drakonia.py color 0 0 255            logo in blue
  python drakonia.py lighting steady high
  python drakonia.py dpi 2000
  python drakonia.py rate 1000
  python drakonia.py sensitivity 5            1 = slow ... 10 = factory (full speed)
  python drakonia.py buttons                  list buttons and actions
  python drakonia.py config "10=keys ctrl+c" "11=keys ctrl+v" "6=text hello"
  python drakonia.py config --file my-mouse.txt
  python drakonia.py factory                  factory buttons and macros
  python drakonia.py rescue                   back to normal mode if the mouse stopped moving
  python drakonia.py --dry-run color 255 0 0  print the bytes, send nothing

The mouse always receives the whole button table and all macros at once,
and it cannot be read back: `config` starts from the factory layout and
changes only the buttons you name. Keep your layout in a file.
"""
import argparse
import sys
import time

VID, PID = 0x04D9, 0xA067
DELAY = 0.016  # 16 ms between messages, like the vendor software

# ---------------------------------------------------------------- scrambling
# Inverse of EncodeSecrecy_V11 in the vendor's HIDApi.dll (session key = 0).
K1 = bytes(((b << 4) | (b >> 4)) & 0xFF for b in b"oNgtEng")


def _rotate(data, k, left):
    n = int.from_bytes(data, "big")
    bits = len(data) * 8
    n = ((n << k) | (n >> (bits - k))) if left else ((n >> k) | (n << (bits - k)))
    return (n & ((1 << bits) - 1)).to_bytes(len(data), "big")


def _permute(d):
    d = bytearray(d)
    d[0], d[3] = d[3], d[0]
    d[2], d[6] = d[6], d[2]
    d[4], d[5] = d[5], d[4]
    return bytes(d)


def scramble(data7):
    x = bytes((a + b) & 0xFF for a, b in zip(data7, K1))
    return _permute(_rotate(x, 3, True))


def unscramble(enc7):
    x = _rotate(_permute(enc7), 3, False)
    return bytes((a - b) & 0xFF for a, b in zip(x, K1))


# Real messages captured on the wire: (clear, as sent)
VECTORS = [
    ("03010000000000", "073a2bb7cf33a7b2"), ("02010000000000", "073a2bb7c733a7b2"),
    ("120a0a00000000", "073a74b04733a702"), ("04014000000000", "073a2db7d733a7b2"),
    ("05010006000000", "076a2bb7df33a7b2"), ("0aff0000000000", "073a1bb00733a7b2"),
    ("0b040000000000", "073a43b00f33a7b2"), ("0c040000000000", "073a43b01733a7b2"),
    ("09030000000000", "073a3bb7ff33a7b2"), ("0a0000fe000000", "072a23b00733a7b2"),
    ("09050000000000", "073a4bb7ff33a7b2"), ("01040000000000", "073a43b7bf33a7b2"),
]

# ---------------------------------------------------------------- values
DPI = {500: 1, 1000: 2, 1500: 3, 2000: 4, 3000: 5, 4000: 6, 5000: 7}
RATE = {125: 8, 250: 4, 500: 2, 1000: 1}  # the mouse takes the interval in ms
MODE = {"steady": 1, "slow": 2, "medium": 3, "fast": 4}  # slow/medium/fast = pulsing
BRIGHTNESS = {"off": 1, "low": 2, "medium": 3, "high": 4}

# Factory tables, from a capture of "reset to defaults"
FACTORY_BUTTONS = bytes.fromhex(
    "00f0000000f1000000f2000000f4000000f30000040000f3040000f4"
    "71010000710200007201000053000a0a") + bytes(20)
# 16 slots of 96 bytes; slot 10 holds the factory double click
FACTORY_MACROS = bytes(961) + bytes.fromhex("0100f080f000f080f0") + bytes(1536 - 970)

BUTTON_NAMES = {
    1: "left click", 2: "right click", 3: "wheel click", 4: "forward", 5: "back",
    6: "wheel tilt left", 7: "wheel tilt right", 8: "DPI+", 9: "DPI-",
    10: "right side button", 11: "left side button",
}

ACTIONS = {
    "left": "00f00000", "right": "00f10000", "middle": "00f20000",
    "back": "00f30000", "forward": "00f40000",
    "tilt-left": "040000f3", "tilt-right": "040000f4",
    "dpi+": "71010000", "dpi-": "71020000",
    "disable": "00000000",
    "double-click": "53000a0a",       # factory macro in slot 10
    "color-cycle": "72010000",        # cycles the logo blue -> yellow -> red
    # media keys: 04 00 00 + USB HID consumer usage (same codes as the vendor menu)
    "volume+": "040000e9", "volume-": "040000ea", "mute": "040000e2",
    "play-pause": "040000cd", "stop": "040000b7",
    "next-track": "040000b5", "prev-track": "040000b6",
}

# Shortcuts of the vendor "Basic" and "Advanced" menus (sent as macros, like the vendor does)
SHORTCUTS = {
    "cut": "ctrl+x", "copy": "ctrl+c", "paste": "ctrl+v", "undo": "ctrl+z",
    "select-all": "ctrl+a", "find": "ctrl+f", "new": "ctrl+n", "print": "ctrl+p",
    "save": "ctrl+s", "switch-window": "alt+tab", "close-window": "alt+f4",
    "explorer": "win+e", "run": "win+r", "show-desktop": "win+d", "lock": "win+l",
}

# USB HID keyboard usages (physical key positions)
KEYS = {c: 0x04 + i for i, c in enumerate("abcdefghijklmnopqrstuvwxyz")}
KEYS.update({str((i + 1) % 10): 0x1E + i for i in range(10)})
KEYS.update({"enter": 0x28, "esc": 0x29, "backspace": 0x2A, "tab": 0x2B, "space": 0x2C,
             " ": 0x2C, "-": 0x2D, "=": 0x2E, "[": 0x2F, "]": 0x30, ";": 0x33, "'": 0x34,
             ",": 0x36, ".": 0x37, "/": 0x38, "delete": 0x4C, "home": 0x4A, "end": 0x4D,
             "pageup": 0x4B, "pagedown": 0x4E, "right": 0x4F, "left": 0x50, "down": 0x51,
             "up": 0x52, "printscreen": 0x46})
KEYS.update({f"f{i}": 0x3A + i - 1 for i in range(1, 13)})
KEYS.update({f"f{i}": 0x68 + i - 13 for i in range(13, 25)})
MODIFIERS = {"ctrl": 0xE0, "shift": 0xE1, "alt": 0xE2, "win": 0xE3, "super": 0xE3,
             "altgr": 0xE6}
AZERTY = {"a": "q", "q": "a", "z": "w", "w": "z", "m": ";"}

MACRO_SLOT = 96
MACRO_MAX_EVENTS = (MACRO_SLOT - 2) // 2


def key_code(name, azerty):
    name = name.lower()
    if name in MODIFIERS:
        return MODIFIERS[name]
    if azerty and name in AZERTY:
        name = AZERTY[name]
    if name not in KEYS:
        sys.exit(f"Unknown key: {name!r}")
    return KEYS[name]


def shortcut_events(text, azerty):
    """'ctrl+c' -> press ctrl, press c, release c, release ctrl."""
    codes = [key_code(t, azerty) for t in text.split("+")]
    return [(0x00, c) for c in codes] + [(0x80, c) for c in reversed(codes)]


def text_events(text, azerty):
    events = []
    for char in text:
        c = key_code(char, azerty)
        events += [(0x00, c), (0x80, c)]
    return events


def build(settings, azerty):
    """Whole configuration from 'N = action' lines.

    action: a name from ACTIONS, 8 hex digits, 'keys ctrl+c' or 'text hello'.
    Buttons that are not named keep their factory action.
    The macro of button N always goes to macro slot N-1.
    """
    table, macros = bytearray(FACTORY_BUTTONS), bytearray(FACTORY_MACROS)
    for line in settings:
        line = line.split("#")[0].strip()
        if not line:
            continue
        if "=" not in line:
            sys.exit(f"Cannot read {line!r} (expected: N = action)")
        num, action = (x.strip() for x in line.split("=", 1))
        if not num.isdigit() or not 1 <= int(num) <= 11:
            sys.exit(f"Invalid button number: {num!r} (1 to 11)")
        n = int(num)
        if action.lower() in SHORTCUTS:
            action = "keys " + SHORTCUTS[action.lower()]
        word, _, rest = action.partition(" ")
        if word.lower() in ("keys", "text"):
            maker = shortcut_events if word.lower() == "keys" else text_events
            events = maker(rest.strip(), azerty)
            if len(events) > MACRO_MAX_EVENTS:
                sys.exit(f"Macro too long on button {n} ({MACRO_MAX_EVENTS} events max).")
            entry = bytes([0x53, 0x00, n - 1, 2 * len(events) + 2])
            slot = (bytes([0x00, 0x01]) + b"".join(bytes(e) for e in events)).ljust(MACRO_SLOT, b"\0")
            macros[(n - 1) * MACRO_SLOT:n * MACRO_SLOT] = slot
            print(f"  button {n:2} ({BUTTON_NAMES[n]}): {word.lower()} {rest.strip()!r}")
        else:
            code = ACTIONS.get(action.lower(), action)
            try:
                entry = bytes.fromhex(code)
                assert len(entry) == 4
            except (ValueError, AssertionError):
                sys.exit(f"Unknown action: {action!r}. Choices: {', '.join(ACTIONS)}, keys ..., text ...")
            print(f"  button {n:2} ({BUTTON_NAMES[n]}): {action}")
        table[(n - 1) * 4:n * 4] = entry
    return bytes(table), bytes(macros)


def command(*args):
    """Setting message (feature report 7)."""
    return bytes([0x07]) + scramble(bytes(args).ljust(7, b"\0"))


def chunks(block):
    """Data block cut into 8-byte packets: first byte clear, 7 scrambled."""
    for i in range(0, len(block), 8):
        m = block[i:i + 8]
        yield bytes([m[0]]) + scramble(m[1:])


# ---------------------------------------------------------------- hardware
class Mouse:
    def __init__(self, dry_run):
        self.dry_run = dry_run
        self.settings = self.blocks = None
        if dry_run:
            return
        try:
            import hid
        except ImportError:
            sys.exit("Missing module: run  pip install hidapi")
        self.hid = hid
        infos = list(hid.enumerate(VID, PID))
        if not infos:
            sys.exit("Sharkoon Drakonia (04d9:a067) not found. Is it plugged in?")
        self.paths = {}
        for d in infos:
            self.paths.setdefault(d.get("interface_number"), []).append(d["path"])

    def _open(self, interface, test):
        for path in self.paths.get(interface, []):
            dev = self.hid.device()
            try:
                dev.open_path(path)
                if test(dev):
                    return dev
            except (OSError, IOError, ValueError):
                pass
            dev.close()
        sys.exit(f"Cannot talk to interface {interface} of the mouse. "
                 "Linux: use sudo or the udev rule. Windows: close the Sharkoon software.")

    def send(self, msg):
        if self.dry_run:
            print("  cmd  ", msg.hex(" "), "  =", unscramble(msg[1:]).hex(" "))
            return
        if self.settings is None:
            self.settings = self._open(1, lambda d: d.send_feature_report(list(msg)) > 0)
        elif self.settings.send_feature_report(list(msg)) <= 0:
            sys.exit("Failed to send a setting.")
        time.sleep(DELAY)

    def send_block(self, block):
        for m in chunks(block):
            if self.dry_run:
                print("  block", m.hex(" "))
                continue
            packet = [0x00] + list(m)  # report 0 + 8 bytes
            if self.blocks is None:
                self.blocks = self._open(2, lambda d: d.write(packet) > 0)
            elif self.blocks.write(packet) <= 0:
                sys.exit("Failed to send a data block.")
            time.sleep(DELAY)


def send_buttons(mouse, table, macros, prefix=False):
    if prefix:  # sent first by the vendor software, not needed so far
        mouse.send(command(0x03, 0x01))
        mouse.send(command(0x02, 0x01))
    mouse.send(command(0x04, 0x01, 0x40))
    mouse.send_block(table)
    mouse.send(command(0x05, 0x01, 0x00, 0x06))
    mouse.send_block(macros)


def full_sequence(mouse, rgb=(255, 0, 0), mode=4, brightness=4, dpi=3, speed=(10, 10)):
    """Replays the whole vendor sequence (also resets buttons and macros)."""
    mouse.send(command(0x03, 0x01))
    mouse.send(command(0x02, 0x01))
    mouse.send(command(0x12, *speed))
    send_buttons(mouse, FACTORY_BUTTONS, FACTORY_MACROS)
    mouse.send(command(0x0A, *rgb))
    mouse.send(command(0x0B, mode))
    mouse.send(command(0x0C, brightness))
    mouse.send(command(0x09, dpi))


# ---------------------------------------------------------------- commands
def selftest():
    ok = True
    for clear, wire in VECTORS:
        c, w = bytes.fromhex(clear), bytes.fromhex(wire)
        good = scramble(c) == w[1:] and unscramble(w[1:]) == c
        ok &= good
        print(("OK   " if good else "ERROR"), clear, "->", wire)
    print("\nEncoding matches the captures." if ok else "\nWARNING: encoding does not match.")
    return ok


def list_interfaces():
    try:
        import hid
    except ImportError:
        sys.exit("Missing module: run  pip install hidapi")
    found = False
    for d in hid.enumerate(VID, PID):
        found = True
        print(f"interface {d.get('interface_number')}  usage_page=0x{d.get('usage_page', 0):04x} "
              f"usage=0x{d.get('usage', 0):02x}  {d['path']!r}")
    if not found:
        print("Sharkoon Drakonia not found.")


def main():
    p = argparse.ArgumentParser(description="Configure the Sharkoon Drakonia mouse")
    p.add_argument("--dry-run", action="store_true", help="print the bytes, send nothing")
    p.add_argument("--full", action="store_true",
                   help="replay the whole vendor sequence (resets buttons and macros)")
    sp = p.add_subparsers(dest="cmd", required=True)
    sp.add_parser("selftest", help="check the encoding against real captures")
    sp.add_parser("list", help="show the mouse interfaces")
    c = sp.add_parser("color", help="logo color")
    for name in ("red", "green", "blue"):
        c.add_argument(name, type=int)
    e = sp.add_parser("lighting", help="logo effect and brightness")
    e.add_argument("mode", choices=MODE)
    e.add_argument("brightness", choices=BRIGHTNESS)
    d = sp.add_parser("dpi", help="sensor resolution")
    d.add_argument("value", type=int, choices=sorted(DPI))
    r = sp.add_parser("rate", help="polling rate")
    r.add_argument("hz", type=int, choices=sorted(RATE))
    sp_ = sp.add_parser("sensitivity", help="sensor sensitivity, 1 (slow) to 10 (factory, full)")
    sp_.add_argument("x", type=int, help="1 to 10, factory 10")
    sp_.add_argument("y", type=int, nargs="?", help="defaults to x")
    rw = sp.add_parser("raw", help="send any setting message (for research)")
    rw.add_argument("bytes", nargs="+", help="up to 7 hex bytes, e.g. 03 01")
    sp.add_parser("buttons", help="list buttons and actions")
    cf = sp.add_parser("config", help="send the whole button configuration")
    cf.add_argument("settings", nargs="*", help='e.g. "10=keys ctrl+c" "5=disable"')
    cf.add_argument("--file", help="text file with one 'N = action' line per button")
    cf.add_argument("--azerty", action="store_true", help="French AZERTY keyboard")
    sp.add_parser("factory", help="factory buttons and macros")
    sp.add_parser("rescue", help="send 03 01 and 02 01 (normal mode) if the mouse stopped working")
    for x in (cf, sp.choices["factory"]):
        x.add_argument("--prefix", action="store_true",
                       help="send 03 01 and 02 01 first, like the vendor software")
    a = p.parse_args()

    if a.cmd == "selftest":
        sys.exit(0 if selftest() else 1)
    if a.cmd == "list":
        return list_interfaces()
    if a.cmd == "buttons":
        for n, name in BUTTON_NAMES.items():
            print(f"  {n:2}  {name}")
        print("\nActions:", ", ".join(ACTIONS))
        print("Shortcuts:", ", ".join(SHORTCUTS))
        print("         keys ctrl+c   |   text hello")
        return

    mouse = Mouse(a.dry_run)
    if a.cmd == "color":
        rgb = tuple(max(0, min(255, v)) for v in (a.red, a.green, a.blue))
        full_sequence(mouse, rgb=rgb) if a.full else mouse.send(command(0x0A, *rgb))
    elif a.cmd == "lighting":
        if a.full:
            full_sequence(mouse, mode=MODE[a.mode], brightness=BRIGHTNESS[a.brightness])
        else:
            mouse.send(command(0x0B, MODE[a.mode]))
            mouse.send(command(0x0C, BRIGHTNESS[a.brightness]))
    elif a.cmd == "dpi":
        full_sequence(mouse, dpi=DPI[a.value]) if a.full else mouse.send(command(0x09, DPI[a.value]))
    elif a.cmd == "sensitivity":
        y = a.x if a.y is None else a.y
        for v in (a.x, y):
            if not 1 <= v <= 10:  # out of range above 10 (20 made it very slow)
                sys.exit("Sensitivity must be between 1 and 10 (factory 10).")
        mouse.send(command(0x12, a.x, y))
    elif a.cmd == "raw":
        try:
            data = bytes(int(x, 16) for x in a.bytes)
        except ValueError:
            sys.exit("Give hex bytes, e.g.: raw 03 01")
        if not 1 <= len(data) <= 7:
            sys.exit("1 to 7 bytes.")
        mouse.send(command(*data))
    elif a.cmd == "rate":
        mouse.send(command(0x01, RATE[a.hz]))
    elif a.cmd == "rescue":
        mouse.send(command(0x03, 0x01))
        mouse.send(command(0x02, 0x01))
    elif a.cmd == "factory":
        send_buttons(mouse, FACTORY_BUTTONS, FACTORY_MACROS, prefix=a.prefix)
    elif a.cmd == "config":
        lines = list(a.settings)
        if a.file:
            with open(a.file, encoding="utf-8") as fh:
                lines += fh.read().splitlines()
        if not lines:
            sys.exit("Nothing to send: give settings or --file.")
        table, macros = build(lines, a.azerty)
        send_buttons(mouse, table, macros, prefix=a.prefix)
    print("Done." if not a.dry_run else "(dry run: nothing was sent)")


if __name__ == "__main__":
    main()
