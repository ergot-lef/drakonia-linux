"""Run with:  python -m pytest tests   (or simply: python tests/test_drakonia.py)"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import drakonia as d  # noqa: E402


def test_scramble_matches_captures():
    for clear, wire in d.VECTORS:
        c, w = bytes.fromhex(clear), bytes.fromhex(wire)
        assert d.scramble(c) == w[1:]
        assert d.unscramble(w[1:]) == c


def test_factory_config_is_unchanged():
    assert d.build([], False) == (d.FACTORY_BUTTONS, d.FACTORY_MACROS)


def test_ctrl_c_macro_matches_capture():
    # capture: Ctrl+C on button 7 -> entry 53 00 06 0a, slot 6 = 00 01 00 e0 00 06 80 06 80 e0
    table, macros = d.build(["7 = keys ctrl+c"], False)
    assert table[24:28] == bytes.fromhex("5300060a")
    assert macros[576:586] == bytes.fromhex("000100e00006800680e0")


def test_letter_macro_matches_capture():
    # capture: letter A (HID 04) on button 10 -> entry 53 00 09 06
    table, macros = d.build(["10 = text a"], False)
    assert table[36:40] == bytes.fromhex("53000906")
    assert macros[864:870] == bytes.fromhex("000100048004")


def test_azerty_layout():
    _, macros = d.build(["10 = text a"], True)
    assert macros[867] == 0x14  # key labelled A on AZERTY is the Q position


def test_disable_matches_capture():
    table, _ = d.build(["10 = disable"], False)
    assert table[36:40] == bytes(4)


def test_macro_delays_match_captures():
    # "Gaming Mouse" vendor software, macro A then B with a pause: the pause sits in the
    # release flag of A, 0x80 | (ms / 50). Captures of 50, 100 and 1000 ms.
    for ms, flag in ((50, 0x81), (100, 0x82), (1000, 0x94)):
        assert d.macro_events(f"a {ms}ms b", False) == [(0x00, 0x04), (flag, 0x04), (0x00, 0x05), (0x80, 0x05)]
    # three pauses of 50, 100 and 150 ms (capture: 81 04, 82 1d, 83 08)
    assert d.macro_events("a 50ms z 100ms e 150ms", False) == [
        (0x00, 0x04), (0x81, 0x04), (0x00, 0x1D), (0x82, 0x1D), (0x00, 0x08), (0x83, 0x08)]
    assert d.macro_events("ctrl+c 1s", False)[-1] == (0x80 | 20, 0xE0)


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok", name)
