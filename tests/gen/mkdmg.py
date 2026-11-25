# Wraps a raw disk image in an Apple DMG (UDIF): zlib, raw and zero chunks, the blkx table as a
# base64 plist, and the koly trailer.
#   python mkdmg.py <disk-image> <out.dmg>
import base64, struct, sys, zlib
src, dst = sys.argv[1], sys.argv[2]
disk = open(src, 'rb').read()
disk += b'\0' * (-len(disk) % 512)
sectors = len(disk) // 512
data = bytearray(); chunks = []
CH = 2048  # sectors per chunk (1 MiB)
for s in range(0, sectors, CH):
    n = min(CH, sectors - s)
    raw = disk[s * 512:(s + n) * 512]
    if raw.count(0) == len(raw):
        chunks.append((0x00000002, s, n, len(data), 0))
    elif (s // CH) % 3 == 1:
        chunks.append((0x00000001, s, n, len(data), len(raw))); data += raw
    else:
        z = zlib.compress(raw, 9)
        chunks.append((0x80000005, s, n, len(data), len(z))); data += z
chunks.append((0xFFFFFFFF, sectors, 0, len(data), 0))
mish = bytearray(b'mish' + struct.pack('>IQQQII', 1, 0, sectors, 0, 0, len(chunks)) + b'\0' * 24 + b'\0' * 136)
mish += struct.pack('>I', len(chunks))
for t, s, n, o, l in chunks:
    mish += struct.pack('>IIQQQQ', t, 0, s, n, o, l)
b64 = base64.b64encode(bytes(mish)).decode()
xml = ('<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>resource-fork</key><dict>'
       '<key>blkx</key><array><dict><key>Attributes</key><string>0x0050</string><key>Data</key><data>\n'
       + b64 + '\n</data><key>Name</key><string>whole disk (Apple_HFS : 0)</string></dict></array></dict></dict></plist>\n').encode()
xml_off = len(data)
koly = bytearray(512)
struct.pack_into('>4sIII', koly, 0, b'koly', 4, 512, 1)
struct.pack_into('>QQQ', koly, 16, 0, 0, len(data))
struct.pack_into('>II', koly, 56, 1, 1)
struct.pack_into('>QQ', koly, 216, xml_off, len(xml))
struct.pack_into('>IQ', koly, 488, 1, sectors)
open(dst, 'wb').write(bytes(data) + xml + bytes(koly))
print('chunks', len(chunks) - 1)
