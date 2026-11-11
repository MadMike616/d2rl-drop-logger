"""Extract the reviewed native witnesses from the locally supplied MIT reference suite.

Usage: python tools/generate_witnesses.py <RuffnecKk-D2RLoader-Suite-main>
The generated header is checked in; the reference repository is not needed to build.
"""
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
contract = (root / "plugins/auto-pickup/src/auto_interact/native_contract.hpp").read_text(encoding="utf-8-sig")
pickup = (root / "plugins/auto-pickup/src/native.hpp").read_text(encoding="utf-8-sig")
markers = (root / "plugins/mapsense/src/native_automap_marker.cpp").read_text(encoding="utf-8-sig")
output = ["// Native witnesses from RuffnecKk's MIT-licensed reference suite; see THIRD_PARTY_NOTICES.md.",
          "#pragma once", "#include <array>", "#include <span>", "#include <cstdint>", "namespace NativeWitnesses {"]
# This one witness describes the installed Loader image rather than the suite.
output.extend([
    "// The RIP-relative store witnesses the local authoritative game pointer.",
    "inline constexpr std::uintptr_t ActiveGameStoreRva = 0x4057AE;",
    "inline constexpr std::uintptr_t ActiveGameRva = ActiveGameStoreRva + 7 + 0x0269886B;",
    "inline constexpr std::array<std::uint8_t, 12> ActiveGameStoreBytes{",
    "    0x48,0x89,0x05,0x6B,0x88,0x69,0x02,0x48,0x83,0xC4,0x28,0xC3};",
])
entries = []

def extract(source, old_name, new_name):
    pattern = r"std::array<std::uint8_t,\s*(\d+)>\s+" + re.escape(old_name) + r"\s*\{(.*?)\};"
    match = re.search(pattern, source, re.S)
    if not match:
        raise ValueError(f"Missing witness: {old_name}")
    numbers = re.findall(r"0x[0-9A-Fa-f]+", match[2])
    if len(numbers) != int(match[1]):
        raise ValueError(f"Unexpected witness length: {old_name}")
    output.append(f"inline constexpr std::array<std::uint8_t, {len(numbers)}> {new_name}{{" + ",".join(numbers) + "};")

for name in ["GetUnitRoom", "RoomNeighbours", "RoomFirstUnit", "NextUnit", "UnitIdentityLayout",
             "GetGameFromUnit", "ServerUnit", "ObjectRecord", "ObjectDispatcher", "Operation4", "Operation19", "Operation20"]:
    match = re.search(r"\b"+name+r"Rva\s*=\s*(0x[0-9A-Fa-f]+)", contract)
    if not match:
        raise ValueError(f"Missing RVA: {name}")
    output.append(f"inline constexpr std::uintptr_t {name}Rva = {match[1]};")
    extract(contract, name+"Bytes", name+"Bytes")
    entries.append(name)
for source, original, name, rva in [(pickup,"GetItemCodeExpected","GetItemCode","0x36EF50"),
                                   (markers,"getXExpected","GetX","0x34AF60"),
                                   (markers,"getYExpected","GetY","0x34AFB0")]:
    output.append(f"inline constexpr std::uintptr_t {name}Rva = {rva};")
    extract(source, original, name+"Bytes")
    entries.append(name)
output.append("struct Witness { std::uintptr_t rva; std::span<const std::uint8_t> bytes; };")
output.append("inline constexpr std::array Witnesses{")
output.extend("    Witness{"+name+"Rva, "+name+"Bytes}," for name in entries)
output.extend(["};", "}", ""])
(Path(__file__).resolve().parents[1]/"src/native_witnesses.hpp").write_text("\n".join(output), encoding="utf-8")
print(f"Generated {len(entries)} native witnesses.")
