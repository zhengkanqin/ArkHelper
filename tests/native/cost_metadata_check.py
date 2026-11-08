"""Read-only check of cost-hook signatures against an installed IL2CPP game.

Reads files only: never loads GameAssembly.dll, opens a process or touches IPC.
Usage: python cost_metadata_check.py GLOBAL_METADATA_DAT GAME_ASSEMBLY_DLL
"""

import argparse
from pathlib import Path
import struct


def check(metadata_path, assembly_path):
    metadata = Path(metadata_path).read_bytes()
    image = Path(assembly_path).read_bytes()
    header = struct.unpack_from("<68I", metadata)
    assert header[0] == 0xFAB11BAF, "unexpected metadata magic"
    type_count = header[41] // 92

    def string(index):
        start = header[6] + index
        return metadata[start:metadata.index(b"\0", start)].decode("utf-8")

    def definition(index):
        offset = header[40] + index * 92
        values = struct.unpack_from("<16I", metadata, offset)
        return offset, values

    def class_name(index):
        _, values = definition(index)
        namespace, name = string(values[1]), string(values[0])
        return namespace + "." + name if namespace else name

    pe = struct.unpack_from("<I", image, 60)[0]
    assert image[pe:pe + 4] == b"PE\0\0", "unexpected PE signature"
    optional = pe + 24
    assert struct.unpack_from("<H", image, optional)[0] == 0x20B, "expected PE32+"
    base = struct.unpack_from("<Q", image, optional + 24)[0]
    section_count = struct.unpack_from("<H", image, pe + 6)[0]
    optional_size = struct.unpack_from("<H", image, pe + 20)[0]
    sections = []
    for index in range(section_count):
        offset = optional + optional_size + index * 40
        name = image[offset:offset + 8].split(b"\0")[0]
        _, rva, size, raw = struct.unpack_from("<4I", image, offset + 8)
        sections.append((name, rva, size, raw))

    def file_offset(address):
        for _, rva, size, raw in sections:
            if base + rva <= address < base + rva + size:
                return raw + address - base - rva
        raise ValueError("address is outside PE data: " + hex(address))

    # IL2CPP metadata registration contains field-offset and type-size counts
    # equal to the number of definitions. Use both counts and primitive probes.
    candidates = []
    marker = struct.pack("<Q", type_count)
    for name, _, size, raw in sections:
        if name not in (b".rdata", b".data"):
            continue
        start = raw
        while True:
            found = image.find(marker, start, raw + size)
            if found < 0:
                break
            start = found + 8
            if found % 8 or found < 80:
                continue
            entries = struct.unpack_from("<16Q", image, found - 80)
            if entries[10] != type_count or entries[12] != type_count:
                continue
            try:
                types = file_offset(entries[7])
                if entries[6] > 100000 and types + entries[6] * 8 <= len(image):
                    candidates.append((entries, types))
            except ValueError:
                continue
    assert len(candidates) == 1, "cannot uniquely locate metadata registration"
    registration, types = candidates[0]

    def type_info(index):
        assert index < registration[6], "invalid type index"
        address = struct.unpack_from("<Q", image, types + index * 8)[0]
        data, flags = struct.unpack_from("<QI", image, file_offset(address))
        kind, attributes = (flags >> 16) & 255, flags & 65535
        primitive = {1: "System.Void", 2: "System.Boolean", 8: "System.Int32"}
        name = primitive.get(kind)
        if name is None:
            name = class_name(data) if kind in (17, 18) else "IL2CPP kind " + str(kind)
        return name, kind, attributes

    methods = {}
    for index in range(type_count):
        name = class_name(index)
        if name not in ("Torappu.Battle.BattleController", "Torappu.Battle.Character"):
            continue
        offset, values = definition(index)
        count = struct.unpack_from("<H", metadata, offset + 68)[0]
        for method_index in range(values[9], values[9] + count):
            method = struct.unpack_from("<6I4H", metadata, header[12] + method_index * 32)
            method_name = string(method[0])
            if method_name not in ("ModifyCost", "GetCost", "Withdraw", "WithdrawByLevel"):
                continue
            parameters = []
            for parameter_index in range(method[3], method[3] + method[-1]):
                parameter = struct.unpack_from("<3I", metadata, header[22] + parameter_index * 12)
                parameters.append((string(parameter[0]), type_info(parameter[2])))
            assert not method[6] & 0x10, "cost method must be an instance method"
            methods[(name, method_name)] = (type_info(method[2])[0], parameters)

    controller = "Torappu.Battle.BattleController"
    character = "Torappu.Battle.Character"
    expected = {
        (controller, "ModifyCost"): ("System.Boolean", ["System.Int32", "Torappu.Battle.Entity",
            "Torappu.PlayerSide"] + ["System.Boolean"] * 5),
        (controller, "GetCost"): ("System.Int32", ["Torappu.PlayerSide"]),
        (character, "Withdraw"): ("System.Boolean", ["System.Boolean"] * 4),
        (character, "WithdrawByLevel"): ("System.Void", []),
    }
    for key, (return_name, parameter_names) in expected.items():
        actual_return, parameters = methods[key]
        assert actual_return == return_name and [info[0] for _, info in parameters] == parameter_names, key
        print("PASS", key[0] + "." + key[1], "->", actual_return)
        for name, (type_name, kind, attributes) in parameters:
            print(f"  {name}: {type_name}; kind={kind}; attributes=0x{attributes:04X}")
        if key[1] in ("ModifyCost", "Withdraw"):
            booleans = parameters[3:] if key[1] == "ModifyCost" else parameters
            assert all(info[1] == 2 and info[2] & 0x1010 == 0x1010 for _, info in booleans), \
                "expected default-valued ordinary Booleans, not Nullable<Boolean>"
    print("PASS cost hook signature: optional/default flags are attributes on ordinary bool parameters")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("metadata")
    parser.add_argument("assembly")
    args = parser.parse_args()
    check(args.metadata, args.assembly)
