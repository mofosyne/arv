# Draft bug report: makefs -t udf (NetBSD)

Status: **draft, not yet sent.** Confirm it yourself with `repro/repro.sh`
(below) before sending to NetBSD (send-pr / gnats, or the tech-kern / tech-userlevel lists).

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

Allocate whole sectors and zero the tail after each read. See `proposed.patch`:

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

```c
-	strcpy((char *) regid->id, name);
+	/* ids may fill the field exactly ("*UDF Metadata Partition" is 23 of 23 bytes): no NUL */
+	memcpy(regid->id, name, MIN(strlen(name), sizeof(regid->id)));
```

---

## How to check this yourself

```sh
sudo apt install build-essential libbsd-dev git python3 gdb   # gdb optional
third_party/netbsd-makefs-udf/repro/repro.sh /tmp/udf-repro
```

The script:
1. fetches NetBSD src at the commit above (sparse: `usr.sbin/makefs`,
   `usr.sbin/mtree`, `sbin/newfs_udf`, `sbin/fsck`, `sys/fs/udf`);
2. copies out the 17 files makefs -t udf uses, **unmodified**;
3. builds them with the Linux glue in `lib/udfmake/` (compat headers, stubs for the other filesystems, `main` renamed). None of the glue touches the UDF code;
4. runs the three checks, then repeats them with `proposed.patch` applied.

Expected output (abridged):

```
=== upstream: NetBSD src 477d71b4..., files unmodified
1. fortified build: FAILED: *** buffer overflow detected ***: terminated
   ... udf_set_regid ... udf_core.c:778 / udf_add_logvol_part_meta ... udf_core.c:1403
2. AddressSanitizer: ERROR: AddressSanitizer: heap-buffer-overflow
   ... udf_write_sector udf_core.c:3672 ... udf_copy_file udf.c:854
3. padding after file data: 40 of 40 files have non-zero padding

=== proposed: NetBSD src 477d71b4..., with proposed.patch
1. fortified build: OK
2. AddressSanitizer: no errors
3. padding after file data: 0 of 40 files have non-zero padding
```

You can also confirm by reading the code alone:
- **Bug 1:** compare the `malloc` size at udf.c:838 with the byte count written through udf.c:653-655.
- **Bug 2:** `"*UDF Metadata Partition"` has 23 characters, and `strcpy` writes 24 bytes into the 23-byte field.

Before sending, check that trunk hasn't changed these lines since the commit above:

```sh
git -C /tmp/udf-repro/netbsd-git log --oneline -1   # the pinned commit
# compare with current trunk: https://github.com/NetBSD/src/blob/trunk/usr.sbin/makefs/udf.c
```
