# nDISKY `.a3k` archive tools

This directory contains tools for working with **`.a3k`** files — the archive
format used by *A3kDisky* (also known as Disky/CDBONK), a Windows librarian 
for the **Yamaha A3000** sampler (1997–2000 era).  

The original Turbo Delphi source for A3kDisky and its compression unit are 
lost; the format was reverse-engineered from the sample archives in this directory.

There are two independent implementations, both capable of **unpacking** an
`.a3k` archive into a directory of files and **packing** a directory back into
an `.a3k` archive:

| Tool | Language | Entry point |
|------|----------|-------------|
| `a3kpacker.py` | Python 3 | `python3 a3kpacker.py unpack|pack ...` |
| `a3kpack-cli`  | C++20 | `./a3kpack-cli unpack|pack ...` (built from `a3kpack-cli.cpp` + `a3kPack.cpp`) |

Both are wired into the `Makefile` with full round-trip verification.

---

## 1. The `.a3k` file format

An `.a3k` file is **not** a zip/lzh/arc archive.  It is a custom container
("**A3kDiskyPC**") that stores **raw, uncompressed SFS file images** — the same
filesystem format the A3000 uses internally.  The payloads are not deflate/zip
compressed; sample data is plain 16-bit PCM.

### 1.1 Container layout

```
 offset      size   field
 -------     ----   -----------------------------------------------------
 [0:4]       4      version (little-endian, always 1)
 [4:8]       4      dsz = start offset of the file-info section (LE)
 [8:12]      4      file count (LE) == number of file-info entries
 [12:22]     10     magic "A3kDiskyPC"
 [22:70]     48     padding (zeros)
 [70:86]     16     marker "XXXXXXXXXXXXXXXX"
 [86:1110]   1024   padding (zeros)
 [1110:dsz]  var    volume banner / description text (entry 0 of the file info)
 [dsz:EOF]   n*271  file-info section
```

The header is a fixed **1110 bytes**.  The volume banner text begins at offset
`1110` and runs to `dsz`.  `dsz` is simply:

```
dsz = file_size - (file_count * 271)
```

i.e. it is the start of the file-info section.

### 1.2 File-info section (the index)

The file-info section is the archive's index.  It is exactly
`file_count * 271` bytes, one **271-byte entry** per item:

```
 offset   size   field
 -------  ----   ---------------------------------------------------------
 [0:256]  256    path "VolumeName \TYPE\FileName" (NUL-terminated, zero-padded)
 [256:258] 2     marker: 01 00 for file entries, 00 00 for the banner entry
 [258:262] 4     offset of the entry in the file (little-endian)
 [262:266] 4     size of the entry (little-endian)
 [266:270] 4     01 00 00 00
 [270]     1     00
```

- **Entry 0** is always `"/A3kFileInfo.txt"` and points at the banner text
  (offset `1110`, size = banner length).
- **Entries 1..N** each point at a raw SFS file image.

The path field is 256 bytes followed by a 2-byte marker.  Note the marker
distinction: `01 00` for file entries, `00 00` for the banner entry — this must
be reproduced when packing.

### 1.3 SFS file-image header (big-endian)

Each file entry begins with the magic `FSFSDEV3SPLX` followed by a 4-byte type:

| Type | Meaning |
|------|---------|
| `PROG` | Program |
| `SBAC` | Sample bank (all) |
| `SBNK` | Sample bank |
| `SMPL` | Sample |
| `SEQU` | Sequence |

After the 16-byte magic+type, the header is **big-endian** (the A3000's 68k CPU
is big-endian):

```
 offset   size   field
 -------  ----   ---------------------------------------------------------
 [16:20]  4      header size (SMPL = 512; all other types = 48)
 [20:24]  4      number of extents (2 or 4)
 [24:28]  4      data size (2-extent entries) / file size (4-extent entries)
 [28:32]  4      data size (4-extent entries and SMPL)
 [44:48]  4      create date
 [48:...] var    payload (for SMPL the payload begins after the 512-byte header)
```

The data-size field location depends on the entry type/extent count:
- 2-extent entries (`PROG`/`SBAC`/`SBNK`/`SEQU`): data size at `[24:28]`, 48-byte header.
- 4-extent entries: data size at `[28:32]`, 48-byte header.
- `SMPL`: header size at `[16:20]` (=512), data size at `[28:32]`.

Sample payloads are little-endian 16-bit PCM.

### 1.4 Key reverse-engineering findings

- The `.a3k` files are a **custom "A3kDiskyPC" container**, not zip/lzh/arc.
- The payloads are **raw SFS file images**, effectively uncompressed.
- The file-info section is a complete index: it gives the offset, size, and
  path of every entry, so no magic-scanning is required to parse an archive.
- The SFS file-image headers are **big-endian**, which is why naive
  little-endian parsing of the payloads produces garbage.
- The container header, banner, file-info section, and SFS headers are all
  reproduced byte-for-byte on pack, so unpack→pack is lossless.

---

## 2. `a3kpacker.py` (Python)

A self-contained Python 3 script (no third-party dependencies).

### Usage

```bash
python3 a3kpacker.py unpack archive.a3k [outdir]
python3 a3kpacker.py pack   indir [out.a3k]
```

### `unpack`

Extracts the complete contents of an archive into `outdir` (default
`archive.a3k.unpacked`):

- `A3kFileInfo.txt` — the volume banner text.
- `manifest.txt` — the volume name and one `path<TAB>filename` line per entry.
- `NNN_TYPE_fname.bin` — one file per SFS entry holding the **full** entry bytes
  (header + payload), so packing is lossless.

### `pack`

Rebuilds an `.a3k` from a directory:

- If `manifest.txt` is present, it is used (round-trip mode — this is the
  layout produced by `unpack`).
- Otherwise it falls back to scanning `*.bin` files as full FSFS entries,
  deriving the type from the `FSFSDEV3SPLX` magic and the volume name from the
  banner (manual mode).

It reconstructs the 1110-byte header, the banner, the concatenated SFS entries,
and the 271-byte-per-entry file-info section.

---

## 3. `a3kpack-cli` (C++)

A C++20 command-line utility built on the `A3kPack` class in `a3kPack.{h,cpp}`
(located in this same folder).  It mirrors `a3kpacker.py`.

### Build

```bash
make cli          # builds ./a3kpack-cli
# or directly:
c++ -std=c++20 a3kpack-cli.cpp a3kPack.cpp -o a3kpack-cli
```

### Usage

```bash
./a3kpack-cli unpack archive.a3k [outdir]
./a3kpack-cli pack   indir [out.a3k]
```

Output matches the Python tool (archive/volume/banner on unpack; path/volume/
file count/size on pack).

---

## 4. The `A3kPack` C++ class (`a3kPack.h/.cpp`)

The same functionality is exposed as a reusable, dependency-free C++ class
included in this folder:

```cpp
class A3kPack {
public:
    struct Entry { std::string path; uint32_t offset, size; };
    struct UnpackInfo { std::string volume; std::vector<std::string> files; };

    bool unpack(const std::string& archivePath, const std::string& outDir,
                UnpackInfo* info = nullptr);
    bool pack(const std::string& inDir, const std::string& outArchive);
    const std::string& lastError() const;
};
```

It is pure standard C++ (C++17/20, no third-party or JUCE dependency), so it
can be dropped into any host application and called directly.  `a3kpack-cli.cpp`
is a thin command-line wrapper around it.

---

## 5. Makefile

The original `.a3k` archives live in `./originals` and are never modified; all
work happens in `./packLab` (Python) and `./packLab-cli` (C++).

| Target | Description |
|--------|-------------|
| `make unpack-all` | unpack every `originals/*.a3k` with the Python tool |
| `make repack-all` | repack every unpacked dir with the Python tool |
| `make verify` | Python round-trip check (unpack(repack) vs original) |
| `make cli` | build the C++ `a3kpack-cli` binary |
| `make cli-unpack-all` / `cli-repack-all` / `cli-verify` | C++ equivalents |
| `make all` | run both the Python and C++ round-trip suites |
| `make clean` | remove `packLab`, `packLab-cli`, and the CLI binary |

The recipes use shell globs (`for f in originals/*.a3k`) rather than make's
whitespace-splitting `$(wildcard)` because several filenames contain spaces
(e.g. `Dr Banks.a3k`, `Yamaha UK Support CD-ROM A3000 Volume.a3k`).

---

## 6. Verification / round-trip

Both tools are validated by unpacking every archive in `./originals`,
repacking it, and confirming the result is **byte-identical** to the original
(and that re-unpacking the repacked archive matches the original unpacked
content):

```
make all
# ...
ALL ARCHIVES ROUND-TRIP OK        (Python)
ALL ARCHIVES C++ ROUND-TRIP OK    (C++)
```

All 24 archives in `./originals` round-trip losslessly with both tools.
