"""Convert a 24/32-bit uncompressed BMP to PNG (debug helper, stdlib only)."""
import struct, sys, zlib

def bmp_to_png(src, dst):
    b = open(src, 'rb').read()
    off = struct.unpack_from('<I', b, 10)[0]
    w, h = struct.unpack_from('<ii', b, 18)
    bpp = struct.unpack_from('<H', b, 28)[0]
    bpr = ((w * bpp // 8) + 3) & ~3
    rows = []
    for y in range(abs(h)):
        sy = abs(h) - 1 - y if h > 0 else y
        row = b[off + sy * bpr: off + sy * bpr + w * bpp // 8]
        px = bytearray()
        step = bpp // 8
        for x in range(w):
            p = row[x * step: x * step + 3]
            px += bytes((p[2], p[1], p[0]))
        rows.append(b'\x00' + bytes(px))
    raw = b''.join(rows)
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, abs(h), 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(dst, 'wb').write(png)

if __name__ == '__main__':
    bmp_to_png(sys.argv[1], sys.argv[2])
