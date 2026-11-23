import math, struct, sys
# 16-bit 4:4:4 planar, little endian: a bright textured pattern drifting by
# fractional amounts with a brightness fade, so inter prediction needs
# sub-sample interpolation, bi-prediction and values near the top of range.
out, w, h, frames = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
with open(out, 'wb') as f:
    for t in range(frames):
        dx, dy = t * 1.75, t * 0.75
        gain = 1.0 - 0.04 * t
        for plane in range(3):
            row = []
            for y in range(h):
                for x in range(w):
                    u, v = x + dx, y + dy
                    s = (math.sin(u * 0.37 + plane) + math.sin(v * 0.29 - plane)
                         + math.sin((u + v) * 0.11)) / 3.0
                    edge = 1.0 if (int(u / 9) + int(v / 7)) % 2 else 0.0
                    val = 36000 + 22000 * s + 7000 * edge
                    val = max(0, min(65535, int(val * gain)))
                    row.append(val)
            f.write(struct.pack('<%dH' % len(row), *row))
