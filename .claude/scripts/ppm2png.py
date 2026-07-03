import sys, zlib, struct

def read_ppm(path):
    data = open(path, 'rb').read()
    assert data[:2] == b'P6', "not a P6 PPM"
    # parse header tokens (handle whitespace/newlines)
    idx = 2
    vals = []
    while len(vals) < 3:
        # skip whitespace and comments
        while idx < len(data) and data[idx:idx+1].isspace():
            idx += 1
        if data[idx:idx+1] == b'#':
            while idx < len(data) and data[idx:idx+1] != b'\n':
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx:idx+1].isspace():
            idx += 1
        vals.append(int(data[start:idx]))
    w, h, maxv = vals
    idx += 1  # single whitespace after maxval
    pix = data[idx:idx + w*h*3]
    return w, h, pix

def write_png(path, w, h, rgb):
    def chunk(typ, body):
        c = typ + body
        return struct.pack('>I', len(body)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter: none
        raw += rgb[y*w*3:(y+1)*w*3]
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)

w, h, rgb = read_ppm(sys.argv[1])
write_png(sys.argv[2], w, h, rgb)
print(f"converted {w}x{h} -> {sys.argv[2]}")
