# DRAFT bug report: makefs -t udf (NetBSD)

> [!CAUTION]
> **To NetBSD developers: this is an unconfirmed draft. Please don't act on it yet.**
> It was found and written with the help of an AI assistant and has not yet been checked by
> a person on real hardware or on NetBSD itself. It has not been sent through send-pr or the
> mailing lists. If it is confirmed, it will arrive through the usual channels; until then,
> treat everything below as unverified.

Status: **draft, not yet sent.** Checked again on 2026-10-07 (still by the AI assistant, on
Linux): `repro/repro.sh` gave exactly the summary below, every quoted line was read in the
unmodified source, the bug 3 table and both "also noticed" items were redone by hand, and
trunk at `5601fdcca316ec9bfdc6a8016d9dae6d41dc03da` (2026-10-07) has all 17 files unchanged
from the pinned commit. Two corrections came from that check (the bug 3 table's 128-character
row, and a line number). Still to do before sending: a person confirms it, then it goes to
NetBSD (send-pr / gnats, or the tech-kern / tech-userlevel lists).

Found while building UDF 2.50 images with makefs on Linux. It was found and
drafted with the help of an AI assistant, and every claim below comes with the
command that shows it.

## Version

| | |
|---|---|
| Source | NetBSD src trunk, git mirror https://github.com/NetBSD/src |
| Commit | `477d71b4d1b73a66b61a03b5f6d3dc9212d4f888` (2026-09-30) |
| Files | `usr.sbin/makefs/udf.c` **v1.31** (2023/12/28)<br>`sbin/newfs_udf/udf_core.c` **v1.14** (2024/02/05)<br>`sys/fs/udf/ecma167-udf.h` v1.17 (2022/03/18) |

Line numbers below are for those revisions.

---

## Bug 1: udf_copy_file writes past the end of its read buffer into the image

**Severity:** memory-safety bug (out-of-bounds heap read). Heap contents are
written into the image in the unused tail of each file's last sector.

### Where

`usr.sbin/makefs/udf.c`, `udf_copy_file()`:

```c
837: 	chunk = MIN(sz, UDF_MAX_CHUNK_SIZE);
838: 	data = malloc(MAX(chunk, context.sector_size));
 ...
844: 		rd = read(f, data, chunk);
 ...
854: 		udf_append_file_contents(dscr, &data_icb, data, chunk);
```

`udf_append_file_contents()` (same file) then writes **whole sectors**:

```c
653: 	sectors  = udf_datablocks(flen);        /* = UDF_ROUNDUP(flen, sector_size) / sector_size */
655: 	return udf_write_virt(fdata, location, vpart, sectors);
```

`udf_write_virt()` → `udf_write_phys()` (`sbin/newfs_udf/udf_core.c:4052`) →
`udf_write_sector()` copies `context.sector_size` bytes per sector
(`udf_core.c:3672`, `memcpy(... sector, context.sector_size)`).

### Why it is wrong

The buffer holds `MAX(chunk, sector_size)` bytes. Take a file of 3,000,001
bytes (one chunk, since `UDF_MAX_CHUNK_SIZE` is 4 MiB):
- The buffer is 3,000,001 bytes, but 1,465 sectors (3,000,320 bytes) are written.
- So 319 bytes past the end of the `malloc`ed buffer are read and written into the image.

**When it happens:** a file, or the last 4 MiB chunk of a file, is larger than one sector and not a multiple of the sector size. Nearly every file that isn't stored inside its node qualifies.

For files over 4 MiB, the last chunk is shorter than the buffer. The padding is then taken from the previous chunk's data. This is not out of bounds, but it is still stale data in the image.

### Evidence

`repro/repro.sh` builds the unmodified files above on Linux (glibc, gcc) and runs
`makefs -t udf -o T=bdrom,v=2.50,V=2.50 image dir` on 40 test files:

- **AddressSanitizer** reports a heap-buffer-overflow. The stack is:
  - `memcpy`
  - `udf_write_sector` (udf_core.c:3672)
  - `udf_write_phys` (udf_core.c:4052)
  - `udf_write_virt` (udf_core.c:4112)
  - `udf_append_file_contents` (udf.c:655)
  - `udf_copy_file` (udf.c:854)
- **Without the sanitizer, in the image:** the bytes between each file's end and its sector boundary should be zero. They are non-zero for **40 of 40** files. Some of the bytes look like malloc chunk headers, and with `MALLOC_PERTURB_` set they show glibc's freed-memory fill pattern. No other file's data was seen in these runs, but nothing prevents it.

### Suggested fix

Allocate whole sectors and zero the tail after each read. See `patches/01-udf_copy_file-padding-overread.patch`:

```c
-	data = malloc(MAX(chunk, context.sector_size));
+	/* whole sectors: the last, partial sector is written out in full */
+	data = calloc(1, UDF_ROUNDUP(MAX(chunk, context.sector_size), context.sector_size));
 ...
+		/* zero the padding after a short last chunk (not old data or heap) */
+		memset(data + chunk, 0, UDF_ROUNDUP(chunk, context.sector_size) - chunk);
```

With this, ASAN is clean and 0 of 40 files have non-zero padding.

---

## Bug 2: udf_set_regid strcpy writes one byte past regid.id

**Severity:** low. It is undefined behaviour, but the written byte is already
zero, so the image is unaffected. It does abort the program on toolchains that
check field bounds.

### Where

`sbin/newfs_udf/udf_core.c`:

```c
774: udf_set_regid(struct regid *regid, char const *name)
775: {
776: 	memset(regid, 0, sizeof(*regid));
777: 	regid->flags    = 0;		/* not dirty and not protected */
778: 	strcpy((char *) regid->id, name);
779: }
```

`sys/fs/udf/ecma167-udf.h:255-260`: `id` is `uint8_t id[UDF_REGID_ID_SIZE]`, and
`UDF_REGID_ID_SIZE` is 23. It is followed by `id_suffix[8]`.

Two callers pass names that are exactly 23 characters, so the terminating NUL
lands in `id_suffix[0]`:
- `udf_core.c:1403`: `"*UDF Metadata Partition"`. This is the metadata partition, used for UDF 2.50 and later.
- `udf_core.c:1312`: `"*UDF Sparable Partition"`. This is the sparable partition, used for rewritable disc types.

### Evidence

With glibc `-D_FORTIFY_SOURCE=2`, which checks the size of the `id` field itself,
`makefs -t udf -o T=bdrom,v=2.50` aborts with `*** buffer overflow detected ***`.
The stack is:
- `strcpy`
- `udf_set_regid` (udf_core.c:778)
- `udf_add_logvol_part_meta` (udf_core.c:1403)
- `udf_create_logical_dscr`

This reproduction was only run on Linux/glibc. NetBSD's own fortify checks the
whole object, so it probably doesn't abort there.

### Suggested fix

`patches/02-udf_set_regid-strcpy-overrun.patch`:

```c
-	strcpy((char *) regid->id, name);
+	/* ids may fill the field exactly ("*UDF Metadata Partition" is 23 of 23 bytes): no NUL */
+	memcpy(regid->id, name, MIN(strlen(name), sizeof(regid->id)));
```

---

## Bug 3: file names longer than a UDF name can hold silently corrupt the image

**Severity:** high for users: `makefs` exits 0, but the image has a wrong file
name or cannot be opened at all.

### Where

`sbin/newfs_udf/udf_core.c`, `unix_to_udf_name()`, called for every file
identifier from `udf_create_fid()`:

```c
747: 			bits=16;                                /* any character above U+00FF */
757: 		udf_chars = udf_CompressUnicode(udf_chars, bits, ...);   /* bytes: 1 + n or 1 + 2n */
767: 	*result_len = udf_chars;                            /* result_len is &fid->l_fi */
```

`sys/fs/udf/ecma167-udf.h:651`: `uint8_t l_fi;`. The file identifier length is one byte.

### Why it is wrong

A name takes one compression byte plus one byte per character, or two bytes
per character once any character is above U+00FF. So the most a name can hold
is 254 characters, or 127 with any wide character. Nothing checks this, and
the length wraps modulo 256:
- 255 Latin-1 characters encode to 256 bytes, so `l_fi` becomes 0;
- 128 wide characters encode to 257 bytes, so `l_fi` becomes 1: only the compression byte,
  so the name is empty.

Names like this are legal on the source filesystem (Linux allows 255 bytes).

### Evidence

`makefs -t udf -o T=bdrom,v=2.50,V=2.50` on a folder with one file:

| Name | makefs | Result (7-Zip) |
|---|---|---|
| 254 × `a` | exit 0 | correct |
| 255 × `a` | exit 0 | "Cannot open the file as archive" |
| 127 × `a` + `日` (128 characters) | exit 0 | an empty name (7-Zip lists it as `[]`) |
| 200 × `a` + `日` | exit 0 | "Cannot open the file as archive" |

### Suggested fix

Refuse the name, since there is no correct truncation for an image builder.
See `patches/03-unix_to_udf_name-l_fi-overflow.patch`:

```c
+	/* l_fi is one byte: a longer name would wrap and corrupt the directory */
+	if (udf_chars > 255)
+		errx(EXIT_FAILURE, "file name too long for UDF (%d bytes encoded, at most 255): %.*s",
+		    udf_chars, name_len, name);
 	*result_len = udf_chars;
```

---

## Also noticed (not bugs in the strict sense; worth mentioning)

- **Characters beyond U+FFFF.** `wget_utf8()` in `sbin/newfs_udf/unicode.h`
  (lines 77-98) has no entry for 4-byte UTF-8 sequences (`_utf_count[0xf]` is 0).
  So an emoji is stored as its four UTF-8 bytes taken as Latin-1 characters:
  `photo 😀 ok.txt` reads back as `photo ð\x9f\x98\x80 ok.txt`. UDF's OSTA
  Unicode stores 16-bit units, so refusing such names, or at least warning, would
  be kinder than writing a different name.
- **Several source directories with `-t udf`.** `makefs` accepts extra
  directories and `walk.c:363` records each node's `root`, and `ffs.c:822` uses it
  to open files. The UDF backend builds paths from the first directory only
  (`udf.c:1043`/`1059`), so files from the second directory fail to open ("Can't
  open file … for reading") and `udf_populate_walk` then trips
  `assert(dirlen == ddoff)` (`udf.c:1012`).

---

## How to check this yourself

```sh
sudo apt install build-essential git python3 gdb   # gdb optional
upstream/netbsd-makefs/repro/repro.sh /tmp/udf-repro
```

The script:
1. fetches NetBSD src at the commit above (sparse: `usr.sbin/makefs`,
   `usr.sbin/mtree`, `sbin/newfs_udf`, `sbin/fsck`, `sys/fs/udf`);
2. copies out the 17 files makefs -t udf uses, **unmodified**;
3. builds them with the Linux glue in `upstream/netbsd-makefs/` (compat headers, stubs for the other filesystems, `main` renamed). None of the glue touches the UDF code;
4. runs the four checks on unmodified upstream, then with each patch in `patches/` on
   its own (it should fix only its own bug), then with all of them.

Expected output: the details of each run, then this summary (`shows`: the bug shows):

```
                       1 fortify    2 asan       3 padding    4 long name
                       (bug 2)      (bug 1)      (bug 1)      (bug 3)
upstream               shows        shows        shows        shows
only-01                shows        ok           ok           shows
only-02                ok           shows        shows        shows
only-03                shows        shows        shows        ok
all-patches            ok           ok           ok           ok
```

For unmodified upstream, the details include:

```
1. fortified build: FAILED: *** buffer overflow detected ***: terminated
   ... udf_set_regid ... udf_core.c:778 / udf_add_logvol_part_meta ... udf_core.c:1403
2. AddressSanitizer: ERROR: AddressSanitizer: heap-buffer-overflow
   ... udf_write_sector udf_core.c:3672 ... udf_copy_file udf.c:854
3. padding after file data: 40 of 40 files have non-zero padding
4. 255-character name: makefs exit 0; image cannot be read
```

The patches are independent. Each applies to unmodified upstream on its own, and
all three apply in any order (at most a line offset, no fuzz).

You can also confirm by reading the code alone:
- **Bug 1:** compare the `malloc` size at udf.c:838 with the byte count written through udf.c:653-655.
- **Bug 2:** `"*UDF Metadata Partition"` has 23 characters, and `strcpy` writes 24 bytes into the 23-byte field.
- **Bug 3:** `udf_chars` at udf_core.c:767 can exceed 255, and it is stored into the `uint8_t l_fi`.

Before sending, check that trunk hasn't changed these files since the commit above (last
checked 2026-10-07, trunk `5601fdc`: all 17 unchanged):

```sh
cd /tmp/udf-repro/netbsd-git
git fetch -q --depth 1 --filter=blob:none origin trunk
for f in $(git ls-tree -r --name-only 477d71b4d1b73a66b61a03b5f6d3dc9212d4f888 -- usr.sbin/makefs/udf.c \
           usr.sbin/makefs/walk.c sbin/newfs_udf sys/fs/udf); do
    [ "$(git rev-parse 477d71b4d1b73a66b61a03b5f6d3dc9212d4f888:$f)" = "$(git rev-parse FETCH_HEAD:$f)" ] || echo "changed: $f"
done
```
