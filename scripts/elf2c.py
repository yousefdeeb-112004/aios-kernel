#!/usr/bin/env python3
"""Convert an ELF binary to a C source file with a byte array.

Usage: elf2c.py <in.elf> <out.c> [symbol]

`symbol` defaults to user_hello_elf so the original single-program invocation
keeps working unchanged; pass it explicitly to embed additional user programs
(each one needs its own symbol name to avoid colliding at link time).
"""
import sys

elf_path = sys.argv[1]
out_path = sys.argv[2]
symbol = sys.argv[3] if len(sys.argv) > 3 else 'user_hello_elf'

data = open(elf_path, 'rb').read()

with open(out_path, 'w') as f:
    f.write('/* Generated - do not edit */\n')
    f.write('#include <kernel/types.h>\n')
    f.write('const uint8_t %s[] = {\n' % symbol)
    for i in range(0, len(data), 12):
        chunk = data[i:i+12]
        f.write('  ' + ', '.join('0x%02x' % b for b in chunk) + ',\n')
    f.write('};\n')
    f.write('const uint32_t %s_len = sizeof(%s);\n' % (symbol, symbol))

print(f'  ELF embedded: {len(data)} bytes -> {out_path} ({symbol})')
