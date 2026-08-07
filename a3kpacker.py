#!/usr/bin/env python3
"""
a3kpacker.py - Unpack and pack Yamaha A3000 ".a3k" archives.

The ".a3k" files are produced by the Windows program "A3kDisky" (a Yamaha A3000
sampler librarian).  They are NOT zip/lzh/arc.  They use a custom container
called "A3kDiskyPC" that stores raw SFS file images.

Container layout
----------------
  [0:4]      version (little-endian, always 1)
  [4:8]      dsz  = start offset of the file-info section (little-endian)
  [8:12]     file count (little-endian) == number of file-info entries
  [12:22]    magic "A3kDiskyPC"
  [22:70]    padding (zeros)
  [70:86]    marker "XXXXXXXXXXXXXXXX"
  [86:1110]  padding (zeros)
  [1110:dsz] the volume banner/description text (entry 0 of the file info)
  [dsz:EOF]  file-info section, exactly file_count * 271 bytes

File-info section (271 bytes per entry)
---------------------------------------
  [0:258]    path  "VolumeName \TYPE\FileName" (NUL terminated, zero padded)
  [258:262]  offset of the entry in the file (little-endian)
  [262:266]  size   of the entry (little-endian)
  [266:270]  01 00 00 00
  [270]      00

Entry 0 is always "/A3kFileInfo.txt" and points at the banner text.  Every
following entry is a raw SFS file image beginning with the magic
"FSFSDEV3SPLX" followed by a 4-byte type (PROG / SBAC / SBNK / SMPL / SEQU).

SFS file-image header (big-endian, after the 16-byte magic+type)
---------------------------------------------------------------
  [16:20]    header size (only used by SMPL, = 512; else 48)
  [20:24]    number of extents (2 or 4)
  [24:28]    data size (2-extent entries) / file size (4-extent entries)
  [28:32]    data size (4-extent entries and SMPL)
  [44:48]    create date
  [48:...]   payload (for SMPL the payload begins after the 512-byte header)

Usage
-----
  python3 a3kpacker.py unpack archive.a3k [outdir]
  python3 a3kpacker.py pack   indir [out.a3k]

`unpack` writes the banner to A3kFileInfo.txt, a manifest.txt, and one file per
SFS entry (NNN_TYPE_fname.bin) holding the FULL entry bytes.  `pack` rebuilds
an .a3k from a directory produced by `unpack` (or from a directory of full
FSFS .bin entries).
"""

import os
import re
import sys

ENTRY_SIZE = 271
HEADER_SIZE = 1110
MAGIC = b"FSFSDEV3SPLX"


# --------------------------------------------------------------------------- #
# parsing
# --------------------------------------------------------------------------- #

def parse_archive(fn):
    data = open(fn, "rb").read()
    version = int.from_bytes(data[0:4], "little")
    dsz = int.from_bytes(data[4:8], "little")
    count = int.from_bytes(data[8:12], "little")
    assert data[12:22] == b"A3kDiskyPC", "not an A3kDiskyPC archive"
    finfo = data[len(data) - count * ENTRY_SIZE:]
    entries = []
    for i in range(count):
        e = finfo[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE]
        path = e[0:256].split(b"\x00")[0].decode("latin1")
        off = int.from_bytes(e[258:262], "little")
        size = int.from_bytes(e[262:266], "little")
        entries.append((path, off, size))
    return data, version, dsz, count, entries


def entry_type(entry):
    return entry[12:16].decode("latin1", "replace")


def sfs_payload(entry):
    """Return (type, header_size, payload) for one FSFS file image."""
    typ = entry_type(entry)
    n_ext = int.from_bytes(entry[20:24], "big")
    if typ == "SMPL":
        hdr = int.from_bytes(entry[16:20], "big")
        dsize = int.from_bytes(entry[28:32], "big")
    elif n_ext == 4:
        hdr = 48
        dsize = int.from_bytes(entry[28:32], "big")
    else:
        hdr = 48
        dsize = int.from_bytes(entry[24:28], "big")
    return typ, hdr, entry[hdr:hdr + dsize]


# --------------------------------------------------------------------------- #
# unpack
# --------------------------------------------------------------------------- #

def sanitize(name):
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("_") or "file"


def unpack(fn, outdir):
    data, version, dsz, count, entries = parse_archive(fn)
    os.makedirs(outdir, exist_ok=True)

    # banner (entry 0)
    path0, off0, size0 = entries[0]
    banner = data[off0:off0 + size0]
    with open(os.path.join(outdir, "A3kFileInfo.txt"), "wb") as f:
        f.write(banner)

    # volume name from the first file entry's path
    volume = ""
    if count > 1:
        volume = entries[1][0].split("\\")[0]

    width = len(str(count))
    manifest_lines = ["# a3kpacker manifest", f"volume={volume}"]
    extracted = []

    for i, (path, off, size) in enumerate(entries[1:], start=1):
        entry = data[off:off + size]
        typ = entry_type(entry)
        fname = path.split("\\")[-1].strip()
        fname_safe = sanitize(fname)
        outname = f"{i:0{width}d}_{typ}_{fname_safe}.bin"
        with open(os.path.join(outdir, outname), "wb") as f:
            f.write(entry)
        manifest_lines.append(f"{path}\t{outname}")
        extracted.append((typ, fname, len(entry)))

    with open(os.path.join(outdir, "manifest.txt"), "w", encoding="latin1") as f:
        f.write("\n".join(manifest_lines) + "\n")

    return banner, volume, extracted


# --------------------------------------------------------------------------- #
# pack
# --------------------------------------------------------------------------- #

def make_finfo_entry(path, off, size, marker=b"\x01\x00"):
    # path field is 256 bytes, then a marker, then offset/size.
    # File entries carry a 01 00 marker; the banner entry carries 00 00.
    path_b = path.encode("latin1") + b"\x00"
    path_b = path_b[:256].ljust(256, b"\x00")
    return (path_b
            + marker
            + int(off).to_bytes(4, "little")
            + int(size).to_bytes(4, "little")
            + b"\x01\x00\x00\x00"
            + b"\x00")


def read_manifest(indir):
    volume = ""
    entries = []  # list of (path, filename)
    with open(os.path.join(indir, "manifest.txt"), "r", encoding="latin1") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            if line.startswith("volume="):
                volume = line[len("volume="):]
                continue
            parts = line.split("\t")
            if len(parts) == 2:
                entries.append((parts[0], parts[1]))
    return volume, entries


def scan_dir(indir):
    """Fallback: build entries from any *.bin files (full FSFS images)."""
    volume = "New Volume"
    banner_path = os.path.join(indir, "A3kFileInfo.txt")
    if os.path.exists(banner_path):
        with open(banner_path, "rb") as f:
            banner = f.read()
        m = re.search(rb"Volume Name\s*:\s*([^\r\n]+)", banner)
        if m:
            volume = m.group(1).decode("latin1").strip()
    entries = []
    for name in sorted(os.listdir(indir)):
        if not name.lower().endswith(".bin"):
            continue
        full = os.path.join(indir, name)
        with open(full, "rb") as f:
            entry = f.read()
        if not entry.startswith(MAGIC):
            continue
        typ = entry_type(entry)
        fname = os.path.splitext(name)[0]
        # strip leading index prefix if present
        fname = re.sub(r"^\d+_", "", fname)
        fname = re.sub(r"^" + re.escape(typ) + "_", "", fname)
        entries.append((f"{volume} \\{typ}\\{fname}", name))
    return volume, entries


def pack(indir, outfn):
    manifest = os.path.join(indir, "manifest.txt")
    if os.path.exists(manifest):
        volume, entries = read_manifest(indir)
    else:
        volume, entries = scan_dir(indir)

    banner_path = os.path.join(indir, "A3kFileInfo.txt")
    if os.path.exists(banner_path):
        with open(banner_path, "rb") as f:
            banner = f.read()
    else:
        banner = (f"\r\n          A3kDisky Volume ArKive\r\n"
                  f"-------------------------------------------\r\n"
                  f"       Volume Name : {volume}\r\n").encode("latin1")

    # load each entry's full bytes
    entry_blobs = []
    for path, filename in entries:
        with open(os.path.join(indir, filename), "rb") as f:
            entry_blobs.append((path, f.read()))

    count = 1 + len(entry_blobs)
    off = HEADER_SIZE + len(banner)
    for _, blob in entry_blobs:
        off += len(blob)
    dsz = off

    # header
    header = bytearray(HEADER_SIZE)
    header[0:4] = (1).to_bytes(4, "little")
    header[4:8] = dsz.to_bytes(4, "little")
    header[8:12] = count.to_bytes(4, "little")
    header[12:22] = b"A3kDiskyPC"
    header[70:86] = b"XXXXXXXXXXXXXXXX"

    out = bytearray(header)
    out += banner
    for _, blob in entry_blobs:
        out += blob

    # file-info section
    finfo = bytearray()
    finfo += make_finfo_entry("/A3kFileInfo.txt", HEADER_SIZE, len(banner), marker=b"\x00\x00")
    off = HEADER_SIZE + len(banner)
    for path, blob in entry_blobs:
        finfo += make_finfo_entry(path, off, len(blob))
        off += len(blob)
    out += finfo

    with open(outfn, "wb") as f:
        f.write(out)
    return volume, len(entry_blobs), len(out)


# --------------------------------------------------------------------------- #
# main
# --------------------------------------------------------------------------- #

def main(argv):
    if len(argv) < 2 or argv[0] not in ("unpack", "pack"):
        print(__doc__)
        return 1

    cmd = argv[0]
    if cmd == "unpack":
        fn = argv[1]
        outdir = argv[2] if len(argv) > 2 else fn + ".unpacked"
        banner, volume, files = unpack(fn, outdir)
        print(f"Archive : {fn}")
        print(f"Volume  : {volume!r}")
        print("Banner  :")
        print(banner.decode("latin1"))
        print(f"\nUnpacked {len(files)} files to {outdir}/")
        for typ, fname, size in files:
            print(f"  {typ:4} {fname!r:24} {size:>8} bytes")
    else:  # pack
        indir = argv[1]
        outfn = argv[2] if len(argv) > 2 else indir.rstrip("/") + ".a3k"
        volume, nfiles, size = pack(indir, outfn)
        print(f"Packed  : {outfn}")
        print(f"Volume  : {volume!r}")
        print(f"Files   : {nfiles}")
        print(f"Size    : {size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
