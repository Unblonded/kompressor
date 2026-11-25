# Writes the test corpus: a fixed set of files whose contents never change (everything comes from
# SHA-256 in counter mode, so no random module and no Python version can alter it). Every sample
# archive is built from these files, and Windows' own Get-FileHash of them is the expected answer.
#   python mkcorpus.py <out-dir>
import hashlib
import os
import sys


class Stream:
    def __init__(self, seed):
        self.seed, self.n, self.buf = seed.encode(), 0, b''

    def take(self, k):
        while len(self.buf) < k:
            self.buf += hashlib.sha256(self.seed + self.n.to_bytes(8, 'little')).digest()
            self.n += 1
        out, self.buf = self.buf[:k], self.buf[k:]
        return out

    def below(self, m):
        return int.from_bytes(self.take(4), 'little') % m


WORDS = ('archive kompressor block window match literal offset length header entry folder volume stream '
         'checksum table index record sector cluster extent inode catalog directory file name size time '
         'the a of and to in is it that with for as on by this be are from at or').split()


def text(seed, size, line=72):
    s, out, cur = Stream(seed), [], ''
    total = 0
    while total < size:
        w = WORDS[s.below(len(WORDS))]
        if len(cur) + len(w) + 1 > line:
            out.append(cur + '\n')
            total += len(cur) + 1
            cur = ''
        cur = (cur + ' ' + w) if cur else w
    return ''.join(out).encode()[:size]


def x86ish(seed, size):
    # Code-like bytes with plenty of E8/E9 call and jump opcodes, so BCJ filters have work to do.
    s, out = Stream(seed), bytearray()
    while len(out) < size:
        r = s.below(10)
        if r < 3:
            out += bytes([0xE8 if r < 2 else 0xE9]) + (s.below(0x40000) - 0x20000).to_bytes(4, 'little', signed=True)
        else:
            out += bytes([0x55, 0x48, 0x89, 0xE5, 0x8B, 0x45, s.below(256), 0xC3][: 1 + s.below(8)])
    return bytes(out[:size])


def main():
    root = sys.argv[1]
    files = {
        'readme.txt': text('readme', 20000),
        'empty.txt': b'',
        'data/random.bin': Stream('random').take(512 * 1024),
        'data/zeros.bin': bytes(1024 * 1024),
        'data/log.txt': text('log', 1536 * 1024, line=110),
        'data/mixed.bin': b''.join(Stream('mixed%d' % i).take(4096) + bytes(4096) + text('mt%d' % i, 4096) for i in range(40)),
        'code/main.c': text('main.c', 9000, line=60),
        'code/util.h': text('util.h', 1500, line=60),
        'code/app.exe': x86ish('app', 300 * 1024),
        'deep/a/b/c/d/e/note.md': text('note', 700),
        'spaces in name/file with spaces.txt': text('spaces', 300),
        'unicode/café naïve.txt': text('accents', 400),
        'unicode/文件.txt': text('cjk', 400),
        'exact64k.bin': Stream('64k').take(65536),
        'one.txt': b'1',
    }
    for i in range(48):
        files['many/item%02d.txt' % i] = text('many%d' % i, 100 + i * 37)
    for rel, data in files.items():
        p = os.path.join(root, *rel.split('/'))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(data)
    print('%d files' % len(files))


main()
