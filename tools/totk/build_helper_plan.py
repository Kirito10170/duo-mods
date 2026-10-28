#!/usr/bin/env python3
"""Build the Tears of the Kingdom 1.4.2 guest load plan from native/helper/totk/.

The helper (travel-hook.S) is assembled for AArch64, its .text becomes the plan's guest code,
and every relocation is resolved against the game's main module (symbols below) or the
companion mailbox. One call site in main is patched to branch into the helper:

* 0x386F30 -- the UI thread's per-frame `bl 0x387070` (heartbeat, travel request, then the
  original routine).

The expected word is the 1.4.2 (build 5CB42B1C...) instruction there, so the runtime refuses
the plan on any other build. Layout after igawa6's tools/botw/build_helper_plan.py.

    tools/totk/build_helper_plan.py --output packages/TearsOfTheKingdom/dualscreen/load-plans/<build id>.json

Needs clang, llvm-objcopy and llvm-readelf (any recent LLVM).
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "native/helper/totk"

TITLE_ID = "0100F2C0115B6000"
BUILD_ID = "5CB42B1CF25469FB0635FD046453D843C18BC8AB000000000000000000000000"
# main's NSO segments (memory offset, decompressed size) and .bss size, 1.4.2.
LAYOUT = {
    "segments": [
        {"location": 0x0, "size": 0x2BA61F0},
        {"location": 0x2BA7000, "size": 0x9AF05C},
        {"location": 0x3557000, "size": 0x4BCD60},
    ],
    "bss_size": 0xB92A0,
}
MAILBOX_SIZE = 4096
MAILBOX_EPOCH_OFFSET = 4092

UI_UPDATE_SITE, UI_UPDATE_TARGET = 0x386F30, 0x387070
UI_UPDATE_EXPECTED = struct.pack("<I", 0x94000000 | ((UI_UPDATE_TARGET - UI_UPDATE_SITE) // 4)).hex()
BRANCH = struct.pack("<I", 0x94000000).hex()

# Main-module offsets (1.4.2) the helper references. bridge_mailbox resolves to the mailbox.
SYMBOLS = dict(
    bridge_mailbox=0,
    native_ui_update=UI_UPDATE_TARGET,
    # Warp request the map screen's Travel calls with "StartPos_<name>".
    native_warp=0x1FCB2EC,
    # [[[[warp_root]+0x1E8]+0x58]+0x100] is its receiver.
    warp_root=0x3ACB230,
    # The pause menu's Equip and Remove; [pouch_root] is the pouch they take.
    native_equip=0x209B878,
    native_unequip=0x20A417C,
    pouch_root=0x3A6F678,
    # Add to pouch (the event system's, default stats; add_defaults is its constant third
    # argument) and the menu's Eat.
    native_add=0x2092E00,
    add_defaults=0x360C068,
    native_eat=0x20A9518,
)

RELOCATION_KINDS = {
    "R_AARCH64_CALL26": "aarch64_branch26",
    "R_AARCH64_JUMP26": "aarch64_branch26",
    "R_AARCH64_ADR_PREL_PG_HI21": "aarch64_adrp",
    "R_AARCH64_ADD_ABS_LO12_NC": "aarch64_add_lo12",
    "R_AARCH64_LDST64_ABS_LO12_NC": "aarch64_ldr64_lo12",
    "R_AARCH64_LDST32_ABS_LO12_NC": "aarch64_ldr32_lo12",
}
RELOCATION_RE = re.compile(
    r"([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+(R_AARCH64_\S+)\s+[0-9a-fA-F]+\s+(\S+)\s+([+-])\s+([0-9a-fA-F]+)")


def tool(name: str, *args: str) -> str:
    return subprocess.run([name, *args], check=True, capture_output=True, text=True).stdout


def build_plan(work: Path) -> dict:
    obj, binary = work / "hook.o", work / "hook.bin"
    tool("clang", "--target=aarch64-none-elf", "-c", str(HELPER / "travel-hook.S"), "-o", str(obj))
    tool("llvm-objcopy", "-O", "binary", "--only-section=.text", str(obj), str(binary))
    code = binary.read_bytes()
    if not code or len(code) % 4 or len(code) > 65536:
        raise SystemExit(f"unexpected helper size {len(code)}")
    # The runtime refuses exception/system instructions (0xD4/0xD5 words) other than NOP.
    for at in range(0, len(code), 4):
        word = struct.unpack_from("<I", code, at)[0]
        if word >> 24 in (0xD4, 0xD5) and word != 0xD503201F:
            raise SystemExit(f"system instruction {word:#010x} at {at:#x}: the runtime refuses it")

    relocations = []
    for line in tool("llvm-readelf", "-r", str(obj)).splitlines():
        match = RELOCATION_RE.fullmatch(line.strip())
        if not match:
            if line.strip().startswith("0000"):
                raise SystemExit(f"unparsed relocation: {line}")
            continue
        offset, kind, symbol, sign, addend = match.groups()
        if kind not in RELOCATION_KINDS or symbol not in SYMBOLS:
            raise SystemExit(f"unsupported relocation {kind} against {symbol}")
        at = int(offset, 16)
        value = int(addend, 16) * (1 if sign == "+" else -1)
        if at % 4 or at + 4 > len(code) or value < 0:
            raise SystemExit(f"bad relocation at {at:#x}")
        relocations.append(dict(offset=at, kind=RELOCATION_KINDS[kind],
                                target="mailbox" if symbol == "bridge_mailbox" else "main",
                                addend=SYMBOLS[symbol] + value))

    hook = next(int(line.split()[2], 16)
                for line in tool("llvm-nm", "--defined-only", "--format=posix", str(obj)).splitlines()
                if line.startswith("totk_ui_hook "))
    return dict(
        title_id=TITLE_ID, build_id=BUILD_ID, module="main", layout=LAYOUT,
        mailbox_size=MAILBOX_SIZE, mailbox_epoch_offset=MAILBOX_EPOCH_OFFSET, format=2,
        patches=[
            dict(offset=UI_UPDATE_SITE, expected=UI_UPDATE_EXPECTED, replacement=BRANCH,
                 relocations=[dict(offset=0, kind="aarch64_branch26", target="code", addend=hook)]),
        ],
        guest_code=dict(bytes=code.hex(), relocations=relocations),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, required=True, help="load-plan JSON to write")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as work:
        plan = build_plan(Path(work))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(plan, indent=2) + "\n")
    print(f"{args.output}: {len(bytes.fromhex(plan['guest_code']['bytes']))} code bytes, "
          f"{len(plan['guest_code']['relocations'])} relocations")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
