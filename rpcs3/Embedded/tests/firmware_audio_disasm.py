"""Offline inspection of decrypted PowerPC ELF load segments, never guest execution."""
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path('build-uwp-msvc/analysis-tools').resolve()))
from capstone import Cs, CS_ARCH_PPC, CS_MODE_64, CS_MODE_BIG_ENDIAN

data = pathlib.Path(sys.argv[1]).read_bytes()
header = struct.unpack_from('>16sHHIQQQIHHHHHH', data)
segments = []
for index in range(header[10]):
    p = struct.unpack_from('>IIQQQQQQ', data, header[5] + index * header[9])
    if p[0] == 1:
        segments.append((p[3], data[p[2]:p[2] + p[5]]))
        if len(sys.argv) == 2:
            print(f'LOAD addr={p[3]:x} offset={p[2]:x} file={p[5]:x} memory={p[6]:x} flags={p[1]:x}')

if len(sys.argv) > 2:
    start, end = int(sys.argv[2], 0), int(sys.argv[3], 0)
    decoder = Cs(CS_ARCH_PPC, CS_MODE_64 | CS_MODE_BIG_ENDIAN)
    decoder.skipdata = True
    for address, content in segments:
        begin = max(address, start)
        finish = min(address + len(content), end)
        if finish > begin:
            for instruction in decoder.disasm(content[begin-address:finish-address], begin):
                print(f'{instruction.address:08x}: {instruction.mnemonic:12} {instruction.op_str}')
