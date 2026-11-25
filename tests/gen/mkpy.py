# Sample archives written by Python's own libraries (zipfile, tarfile, gzip, bz2, lzma), which share
# no code with Kompressor, plus small formats built by hand: MS COMPRESS (SZDD), Intel HEX,
# compressed SWF and Base64.
#   python mkpy.py <corpus-dir> <out-dir>
import base64
import bz2
import gzip
import lzma
import os
import struct
import sys
import tarfile
import zipfile
import zlib

corpus, out = sys.argv[1], sys.argv[2]


def files():
    for d, _, names in os.walk(corpus):
        for n in sorted(names):
            p = os.path.join(d, n)
            yield p, os.path.relpath(p, corpus).replace(os.sep, '/')


def read(rel):
    with open(os.path.join(corpus, *rel.split('/')), 'rb') as f:
        return f.read()


def write(name, data):
    with open(os.path.join(out, name), 'wb') as f:
        f.write(data)


# ZIP, every method zipfile has
for name, method in (('py-stored.zip', zipfile.ZIP_STORED), ('py-deflate.zip', zipfile.ZIP_DEFLATED),
                     ('py-bzip2.zip', zipfile.ZIP_BZIP2), ('py-lzma.zip', zipfile.ZIP_LZMA)):
    with zipfile.ZipFile(os.path.join(out, name), 'w', method) as z:
        for p, rel in files():
            z.write(p, rel)

# Tarballs
for name, mode in (('py.tar', 'w'), ('py.tar.gz', 'w:gz'), ('py.tar.bz2', 'w:bz2'), ('py.tar.xz', 'w:xz')):
    with tarfile.open(os.path.join(out, name), mode, format=tarfile.PAX_FORMAT) as t:
        for p, rel in files():
            t.add(p, rel)

# Single-file streams
write('log.txt.gz', gzip.compress(read('data/log.txt'), mtime=0))
write('log.txt.bz2', bz2.compress(read('data/log.txt')))
write('log.txt.xz', lzma.compress(read('data/log.txt')))
write('log.txt.lzma', lzma.compress(read('data/log.txt'), format=lzma.FORMAT_ALONE))


# MS COMPRESS (SZDD): LZSS with a 4 KiB window that starts out full of spaces.
def szdd(data, name_last):
    res = bytearray()
    i = 0
    while i < len(data):
        flags, chunk = 0, bytearray()
        for bit in range(8):
            if i >= len(data):
                break
            best_len = best_pos = 0
            start = max(0, i - 4000)
            for L in (18, 12, 8, 5, 3):
                if i + L > len(data):
                    continue
                k = data.rfind(data[i:i + L], start, i + L - 1)
                if 0 <= k and k + L <= i:
                    best_len = L
                    while (best_len < 18 and i + best_len < len(data) and k + best_len < i
                           and data[k + best_len] == data[i + best_len]):
                        best_len += 1
                    best_pos = (k + 4096 - 16) & 4095
                    break
            if best_len >= 3:
                chunk += bytes([best_pos & 0xFF, ((best_pos >> 4) & 0xF0) | (best_len - 3)])
                i += best_len
            else:
                flags |= 1 << bit
                chunk.append(data[i])
                i += 1
        res.append(flags)
        res += chunk
    return b'SZDD\x88\xf0\x27\x33A' + name_last + struct.pack('<I', len(data)) + bytes(res)


write('readme.tx_', szdd(read('readme.txt'), b't'))

# Intel HEX: two ranges, the second after an extended linear address record.
app = read('code/app.exe')


def rec(t, addr, data):
    b = bytes([len(data), addr >> 8, addr & 0xFF, t]) + data
    return ':' + b.hex().upper() + '%02X' % ((-sum(b)) & 0xFF) + '\n'


lines = [rec(0, o, app[o:o + 16]) for o in range(0, 4096, 16)]
lines.append(rec(4, 0, b'\x00\x01'))
lines += [rec(0, 0x100 + o, app[8192 + o:8192 + o + 32]) for o in range(0, 1024, 32)]
lines.append(rec(1, 0, b''))
write('fw.hex', ''.join(lines).encode())

# Compressed Flash movies (zlib and LZMA) around a minimal uncompressed body.
body = bytes([0x08, 0x00]) + struct.pack('<HH', 0x1800, 1) + b'\x40\x00' * 3 + app[:5000] + b'\x00\x00'
fws_len = 8 + len(body)
write('movie-zlib.swf', b'CWS\x0a' + struct.pack('<I', fws_len) + zlib.compress(body))
alone = lzma.compress(body, format=lzma.FORMAT_ALONE)
write('movie-lzma.swf', b'ZWS\x0d' + struct.pack('<I', fws_len) + struct.pack('<I', len(alone) - 13) + alone[:5] + alone[13:])

# Base64 text, wrapped at 76 columns
b = base64.b64encode(app).decode()
write('app.b64', '\n'.join(b[k:k + 76] for k in range(0, len(b), 76)).encode() + b'\n')
print('python samples written')
