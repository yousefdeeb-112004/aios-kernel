#!/usr/bin/env python3
"""Convert ELF binary to C source file with byte array."""
import sys

elf_path = sys.argv[1]
out_path = sys.argv[2]

data = open(elf_path, 'rb').read()

with open(out_path, 'w') as f:
    f.write('/* Generated - do not edit */\n')
    f.write('#include <kernel/types.h>\n')
    f.write('const uint8_t user_hello_elf[] = {\n')
    for i in range(0, len(data), 12):
        chunk = data[i:i+12]
        f.write('  ' + ', '.join('0x%02x' % b for b in chunk) + ',\n')
    f.write('};\n')
    f.write('const uint32_t user_hello_elf_len = sizeof(user_hello_elf);\n')

print(f'  ELF embedded: {len(data)} bytes -> {out_path}')
