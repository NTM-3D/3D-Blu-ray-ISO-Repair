# 3D Blu-ray ISO Repair

3D Blu-ray ISO Repair repairs UDF allocation tables in 3D Blu-ray ISO images. It finds 3D clip pairs whose `.m2ts` view files and SSIF containers no longer agree, then rewrites the affected file entries, growing the ISO when the metadata needs more room.

It repairs three related defects:

- Truncated or missing `.m2ts` allocation descriptors
- SSIF extents that cut through interleaved chunks
- `.m2ts` view files that point at standalone data instead of the interleaved SSIF data

Analysis is read-only by default. Nothing is written to the ISO until `-apply` is given.

## Building

With a C11 compiler on `PATH`:

```
build.bat                 # Windows convenience wrapper
```

or with `make`:

```
make
```

or manually:

```
windres src/version.rc -O coff -o src/version_rc.o
gcc -O2 -std=c11 -Wall -Wextra -o 3d_bluray_iso_repair.exe \
    src/udf.c src/udf_repair.c src/clpi.c src/repair.c src/report.c src/main.c src/version_rc.o
```

On Windows the executable carries a `VERSIONINFO` resource from `src/version.rc`. `build.bat` needs `gcc` and `windres` on `PATH`, or the `CC` and `RC` environment variables pointing to them.

## Usage

```
3d_bluray_iso_repair.exe <input> [options]
```

Run with no arguments, `-h` or `-help` to print the built-in help.

### Options

| Option | Description |
| --- | --- |
| `-apply` | Write the repairs (default: dry run) |
| `-undo` | Restore the original sectors of a single ISO |
| `-order DB\|BD` | Force the interleaving chunk order instead of auto-detecting it |
| `-chatty` | Show detailed technical output (single ISO mode) |
| `-no-data-check` | Skip the MPEG-TS sync-byte probes |
| `-show-all` | In folder mode, also list ISOs that have no issues |
| `-h`, `-help` | Print help |

### Single ISO

The default run reports the image size, the number of clips, playlists and 3D clip pairs, and a summary of what can be repaired:

```
test.iso (42.86 GiB)
72 clips, 62 playlists, 10 3D clip pairs.

Can repair: 10 clip pair(s).
  SSIF extent alignment repair: 1
  Stream file table repair: 9

Run again with -apply to write the repairs.
```

`-chatty` adds the per-pair analysis, including the detected interleaving order and each file's expected and actual allocation.

With `-apply`, the repaired file entries are written and the result is verified by re-reading the image:

```
Repairs written successfully.

Verifying the repaired image...
All 3D clip pairs are consistent now.
```

### Undo

An `-apply` run writes a journal next to the ISO:

```
test.iso.udffix-undo
```

`-undo` writes the original sectors back, truncates the image to its original size, and renames the journal to `.udffix-undo.done`:

```
3d_bluray_iso_repair.exe test.iso -undo
Restored 99 sector(s). Image is back to its previous state.
```

Keep the journal until the repaired image has been tested. An existing journal prevents a second `-apply` from overwriting it.

### Folder mode

A folder path processes every `.iso` file directly inside it and prints one row per ISO with issues:

```
ISO           Issues  Status
------------  ------  ----------------------------
a_broken.iso      10  Fixable: SSIF extent alignment repair: 1; Stream file table repair: 9
```

Without `-apply`, the status is `Fixable` or `Not fixable`. With `-apply`, fixable ISOs are repaired and verified; unfixable ISOs are left untouched and reported. Clean ISOs are skipped unless `-show-all` is given.

### Interleaving order

The chunk order is detected from MPEG-TS sync bytes in the streams. `-order` forces `DB` or `BD`; `-no-data-check` skips the probes.

## Safety

- Always work on a copy of the ISO.
- Keep the generated `.udffix-undo` journal until playback has been verified.
- Use `-undo` to revert changes and restore the original image size.
