#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import dt_fixup


def node_with_compat(compat):
    node = dt_fixup.ADTNode()
    node.props["name"] = "aic"
    node.props["compatible"] = compat
    return node


def check_aic23(compat):
    node = node_with_compat(compat)
    dt_fixup.fixup_aic(node)
    assert node.props["aic-iack-offset"] == "u64:0x1000"


def main():
    check_aic23("aic,2")
    check_aic23("aic,3")
    check_aic23(b"aic,3\x00vendor,extra\x00")

    aic1 = node_with_compat("aic,1")
    dt_fixup.fixup_aic(aic1)
    assert "aic-iack-offset" not in aic1.props

    missing = dt_fixup.ADTNode()
    missing.props["name"] = "aic"
    try:
        dt_fixup.fixup_aic(missing)
    except ValueError as exc:
        assert "compatible" in str(exc)
    else:
        raise AssertionError("fixup_aic accepted an AIC node without compatible")

    print("AIC firmware fixup tests passed")


if __name__ == "__main__":
    main()
