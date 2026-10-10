"""Original anti-aliased switch masks, embedded by the RCO resource builder."""
import math
import struct
import zlib


def png(width, height, radius, outline=False):
    samples = 4
    data = bytearray()
    for y in range(height):
        data.append(0)
        for x in range(width):
            alpha = 0.0
            for sy in range(samples):
                for sx in range(samples):
                    px, py = x + (sx + .5) / samples, y + (sy + .5) / samples
                    dx = max(abs(px - width / 2) - (width / 2 - radius), 0)
                    distance = math.hypot(dx, abs(py - height / 2))
                    if distance <= radius:
                        inside = distance < radius - 1.5 and abs(py - height / 2) < radius - 1.5
                        alpha += .25 if outline and inside else 1
            data.extend((255, 255, 255, round(255 * alpha / samples ** 2)))

    def chunk(kind, payload):
        return (struct.pack('>I', len(payload)) + kind + payload +
                struct.pack('>I', zlib.crc32(kind + payload)))

    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(bytes(data), 9)) + chunk(b'IEND', b''))
