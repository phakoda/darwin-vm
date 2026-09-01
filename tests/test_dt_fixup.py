#!/usr/bin/env python3

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from dt_fixup import ADTNode, del_compat


def node(name, compatible):
    n = ADTNode()
    n.props["name"] = name
    n.props["compatible"] = compatible
    return n


def main():
    root = ADTNode()
    root.props["name"] = "root"

    preserved = [
        node("dart-usb", b"dart,t8030\x00dart,t8027\x00"),
        node("usb-drd", b"usb-drd,t8030\x00usb-drd,t8027\x00"),
        node("usb-device", b"usb-device,t8030\x00usb-device,s5l8900x\x00"),
        node("atc-phy", b"atc-phy,t8030\x00atc-phy,t8027\x00"),
    ]
    stripped = node("unsupported", b"some-unimplemented-device\x00")
    root.children = preserved + [stripped]

    del_compat(root)

    for n in preserved:
        assert "compatible" in n.props, f"{n.props['name']} lost compatible"
    assert "compatible" not in stripped.props, "unsupported driver was retained"

    print("dt_fixup compatibility tests passed")


if __name__ == "__main__":
    main()
