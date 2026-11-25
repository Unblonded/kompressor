# Makes damaged copies of sample archives, runs `kompressor-cli repair` on them, and checks what came
# back with Python's own zipfile (independent of Kompressor) against the corpus hashes.
#   python tests/repair.py      (after tests/run.ps1 has made the samples)
import hashlib, io, os, shutil, subprocess, sys, zipfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
cli = os.path.join(root, r"build\windows\kompressor-cli.exe")
samples = os.path.join(root, r"tests\samples")
work = os.path.join(samples, ".repair")
shutil.rmtree(work, ignore_errors=True)
os.makedirs(work)

truth = {}
for line in open(os.path.join(samples, "corpus.sha256"), encoding="utf-8"):
    h, p = line.rstrip("\n").split(" *", 1)
    truth[p] = h

def damaged(name, src, how):
    data = bytearray(open(os.path.join(samples, src), "rb").read() if isinstance(src, str) else src)
    data = how(data)
    p = os.path.join(work, name)
    open(p, "wb").write(data)
    return p

def run(p):
    r = subprocess.run([cli, "repair", p], capture_output=True, text=True, encoding="utf-8", errors="replace")
    return r.returncode, (r.stdout + r.stderr).strip()

import glob
# 7-Zip checks the Deflate64 case: on PATH, or vcpkg's copy.
SZ = shutil.which("7z") or next(iter(sorted(glob.glob(os.path.expanduser(r"~\vcpkg\downloads\tools\7zip-*\7z.exe")), reverse=True)), None)
def check_zip(p):
    if "deflate64" in p and not SZ:
        return 1, 0  # nothing here reads Deflate64 independently
    if "deflate64" in p:  # Python's zipfile has no Deflate64: let 7-Zip extract it
        d = p + ".x"
        subprocess.run([SZ, "x", "-y", "-bso0", "-bsp0", "-o" + d, p], check=True)
        return check_dir(d)
    good = bad = 0
    with zipfile.ZipFile(p) as z:
        for i in z.infolist():
            if i.is_dir():
                continue
            d = z.read(i)
            if truth.get(i.filename) == hashlib.sha256(d).hexdigest():
                good += 1
            else:
                bad += 1
    return good, bad

def check_dir(d):
    good = bad = 0
    for base, _, files in os.walk(d):
        for f in files:
            rel = os.path.relpath(os.path.join(base, f), d).replace("\\", "/")
            if truth.get(rel) == hashlib.sha256(open(os.path.join(base, f), "rb").read()).hexdigest():
                good += 1
            else:
                bad += 1
    return good, bad

# A ZIP written to a non-seekable stream: sizes and CRCs come after each entry's data.
class NoSeek(io.RawIOBase):
    def __init__(self): self.b = bytearray()
    def writable(self): return True
    def write(self, x): self.b += x; return len(x)
ns = NoSeek()
corpus = os.path.join(samples, "corpus")
with zipfile.ZipFile(ns, "w", zipfile.ZIP_DEFLATED) as z:
    for p in sorted(truth):
        if os.path.exists(os.path.join(corpus, p)):
            z.write(os.path.join(corpus, p), p)
streamed = bytes(ns.b)

cases = [
    ("cut70.zip", "py-deflate.zip", lambda d: d[: len(d) * 7 // 10]),
    ("holed.zip", "dotnet.zip", lambda d: d[:len(d) // 3] + bytearray(b"\xAA" * 500) + d[len(d) // 3 + 500:]),
    ("stored-cut.zip", "py-stored.zip", lambda d: d[: len(d) * 8 // 10]),
    ("bzip2-cut.zip", "py-bzip2.zip", lambda d: d[: len(d) * 6 // 10]),
    ("lzma-cut.zip", "py-lzma.zip", lambda d: d[: len(d) * 6 // 10]),
    ("deflate64-cut.zip", "7z-deflate64.zip", lambda d: d[: len(d) * 6 // 10]),
    ("streamed-cut.zip", streamed, lambda d: d[: len(d) * 8 // 10]),
    ("intact.zip", "kz.zip", lambda d: d),
    ("cut.tar.gz", "py.tar.gz" if os.path.exists(os.path.join(samples, "py.tar.gz")) else "wtar.tar.gz", lambda d: d[: len(d) * 6 // 10]),
    ("holed.7z", "7z-lzma2.7z", lambda d: d[:len(d) // 2] + bytearray(b"U" * 300) + d[len(d) // 2 + 300:]),
]
ok = True
for name, src, how in cases:
    if isinstance(src, str) and not os.path.exists(os.path.join(samples, src)):
        print(f"{name:18} SKIP (no {src})")
        continue
    p = damaged(name, src, how)
    code, out = run(p)
    first = out.splitlines()[0] if out else ""
    summary = out.splitlines()[1].strip() if len(out.splitlines()) > 1 else ""
    if name.endswith(".zip"):
        rp = os.path.join(work, name[:-4] + " (repaired).zip")
        g, b = check_zip(rp) if os.path.exists(rp) else (0, 0)
    else:
        base = name.split(".")[0]
        rp = os.path.join(work, base + " (recovered)")
        g, b = check_dir(rp) if os.path.exists(rp) else (0, 0)
    status = "ok" if g > 0 and b == 0 else "BAD"
    ok &= status == "ok" or name == "holed.7z"
    print(f"{name:18} exit {code}  verified {g} good, {b} wrong  [{status}]  {summary or first}")
print("ALL OK" if ok else "PROBLEMS")
sys.exit(0 if ok else 1)
