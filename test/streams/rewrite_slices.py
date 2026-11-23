# Rewrites an HM stream (two slices per picture) so that the second slice of
# each picture differs from the first in a way HM itself never produces.
#   lists: ref_pic_list_modification reverses L0 in the second slice
#   pps:   the second slice names a second PPS (same but init_qp + 2)
# usage: rewrite.py base.bit offsets.txt lists|pps out.bit
# offsets.txt: the "PPS ..." / "SH ..." lines of the instrumented decoder.
import re, sys

src, offs, mode, dst = sys.argv[1:5]
data = open(src, 'rb').read()

def split_annexb(d):
    nals, i = [], 0
    starts = [m.end() for m in re.finditer(b'\x00\x00\x01', d)]
    for k, s in enumerate(starts):
        e = len(d) if k + 1 == len(starts) else starts[k + 1] - 3
        nal = d[s:e]
        while nal.endswith(b'\x00'):
            nal = nal[:-1]
        nals.append(nal)
    return nals

def unescape(b):
    out, z, i = bytearray(), 0, 0
    for byte in b:
        if z >= 2 and byte == 3:
            z = 0
            continue
        out.append(byte)
        z = z + 1 if byte == 0 else 0
    return bytes(out)

def escape(b):
    out, z = bytearray(), 0
    for byte in b:
        if z >= 2 and byte <= 3:
            out.append(3)
            z = 0
        out.append(byte)
        z = z + 1 if byte == 0 else 0
    return bytes(out)

def bits_of(b): return ''.join('{:08b}'.format(x) for x in b)
def bytes_of(s):
    assert len(s) % 8 == 0
    return bytes(int(s[i:i + 8], 2) for i in range(0, len(s), 8))
def ue(v):
    v += 1
    n = v.bit_length()
    return '0' * (n - 1) + '{:b}'.format(v)
def se(v): return ue(2 * v - 1 if v > 0 else -2 * v)
def align(s): return s + '1' + '0' * ((-(len(s) + 1)) % 8)

lines = open(offs).read().splitlines()
kv = lambda l: {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', l)}
pps_info = [kv(l) for l in lines if l.startswith('PPS')][0]
sh_info = [kv(l) for l in lines if l.startswith('SH')]

nals = split_annexb(data)
out, si = [], 0
for nal in nals:
    t = (nal[0] >> 1) & 63
    if t in (39, 40):          # SEI: picture hashes no longer apply
        continue
    hdr, rbsp = nal[:2], unescape(nal[2:])
    b = bits_of(rbsp)
    if t == 34:                # PPS
        if mode == 'lists':
            p = pps_info['lm_at']
            assert b[p] == '0'
            b = b[:p] + '1' + b[p + 1:]
            out.append(hdr + escape(bytes_of(b)))
        else:
            out.append(nal)
            a, e = pps_info['initqp_at'], pps_info['initqp_end']
            assert b[0] == '1'         # pps id 0
            body = b[:b.rindex('1')]   # drop rbsp_trailing_bits
            nb = ue(1) + body[1:a] + se(pps_info['initqp'] + 2) + body[e:]
            out.append(hdr + escape(bytes_of(align(nb))))
        continue
    if t > 31:
        out.append(nal)
        continue
    info = sh_info[si]; si += 1
    end = info['end']
    head, tail = b[:end], rbsp[end // 8 + 1:]
    assert b[end] == '1' and set(b[end + 1:(end // 8 + 1) * 8]) <= {'0'}
    second = info['first'] == 0
    if mode == 'lists':
        if info['type'] != 2 and info['used'] > 1:
            p = info['rplm_at']
            is_b = info['type'] == 0
            if second and info['n0'] > 1:
                n, nb = info['n0'], (info['used'] - 1).bit_length()
                assert n <= info['used'] and not info['col_l0']
                ins = '1' + ''.join('{:0{w}b}'.format(n - 1 - i, w=nb)
                                    for i in range(n))
            else:
                ins = '0'
            if is_b:
                ins += '0'
            head = head[:p] + ins + head[p:]
    else:
        if second:
            p = info['pps_at']
            assert head[p] == '1'
            head = head[:p] + ue(1) + head[p + 1:]
    out.append(hdr + escape(bytes_of(align(head)) + tail))
assert si == len(sh_info), (si, len(sh_info))
with open(dst, 'wb') as f:
    for nal in out:
        f.write(b'\x00\x00\x00\x01' + nal)
print(dst, 'nals', len(out))
