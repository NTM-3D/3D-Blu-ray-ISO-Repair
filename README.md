# 3D Blu-ray ISO Repair

3D Blu-ray ISO Repair repairs UDF allocation tables in 3D Blu-ray ISO images. It finds 3D clip pairs whose `.m2ts` view files and SSIF containers no longer agree, then rewrites the affected file entries, growing the ISO when the metadata needs more room.

It repairs three related defects:

- Truncated or missing `.m2ts` allocation descriptors
- SSIF extents that cut through interleaved chunks
- `.m2ts` view files that point at standalone data instead of the interleaved SSIF data

## Playback problems this fixes

When the allocation tables disagree, the player reads the wrong data from the interleaved base and dependent streams. Depending on the player, the same defect can show up as:

- The picture freezing at a point and never recovering.
- Stuttering or dropped frames during playback.
- The player skipping ahead, jumping over scenes or to the end of the movie.
- Playback failing completely, with the player returning to the menu or showing an error.

The video data itself is usually intact; the problem is in the ISO's file system metadata. Once the `.m2ts` and SSIF allocations agree again, the image can play normally on players and virtual drives that previously failed.

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

When metadata must grow, the tool relocates the small metadata partition and its mirror into contiguous extents, placing the mirror before the primary metadata. It updates the volume integrity sizes and keeps the end anchors outside the physical partition. This avoids readers losing access to allocation descriptors in a second metadata extent, but readers that cannot follow allocation extent descriptors may still not support the repaired stream files. Growth is refused if the existing metadata mirror or integrity information is inconsistent.

The tool detects an end anchor overlapping an empty mirror block alongside inconsistent integrity sizes. It validates the remaining metadata before staging a contiguous relocation. Other fragmented metadata layouts are reported as potentially incompatible and are not automatically compacted.

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
| `-realtime-streams` | Set `.m2ts` and `.ssif` UDF file types to `RealtimeFile` (249); single ISO mode only |
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

`-realtime-streams` sets the UDF file type of `.m2ts` and `.ssif` entries to `RealtimeFile` (249) and updates their descriptor checksums. It does not change media data and works with one ISO at a time.

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
If a `.udffix-undo.done` file already exists, archive it before running `-undo`; the tool will not overwrite the previous journal.

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

## Credits

The following projects provided reference implementations and validation tools:

- [tsMuxer, teaching-droid fork](https://github.com/teaching-droid/tsMuxer) — reference for Blu-ray 3D metadata, interleaving, and UDF stream file types.
- [libudfread](https://code.videolan.org/videolan/libudfread) — independent UDF reader used to validate file reads.
- [edge264-mvc](https://github.com/jens-duttke/edge264-mvc) — MVC decoder used to verify both video views.
- [NanaZip](https://github.com/M2Team/NanaZip) — independent UDF compatibility checks.
- [FFmpeg](https://ffmpeg.org/) and [libbluray](https://code.videolan.org/videolan/libbluray) — media and Blu-ray navigation probing.
