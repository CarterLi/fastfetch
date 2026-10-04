# miniz (tinfl subset, vendored)

The decompressor from [miniz](https://github.com/richgel999/miniz) **3.1.2**, used by
`src/common/impl/inflate.c` to expand the compressed ascii logos, the compressed help text, a
deflated `classes.dex` inside an Android jar, and any HTTP response that arrives
`Content-Encoding: gzip`. It is the decoder behind the `ENABLE_MINIZ=ON` default; with
`-DENABLE_MINIZ=OFF` `inflate.c` loads the system zlib instead.

## What is here

miniz 3.1.2 is released as an amalgamated pair, `miniz.c` + `miniz.h`. Both are vendored, but not
both in full:

| file | origin |
|---|---|
| `miniz.h` | **verbatim** miniz 3.1.2 `miniz.h` |
| `miniz_tinfl.c` | miniz 3.1.2 `miniz.c`, the "Low-level Decompression" section (lines 2278-3027) |
| `LICENSE` | **verbatim** miniz 3.1.2 `LICENSE` |
| `README.md`, `repo.json` | ours |

⚠️ The release's own `miniz.c` is **not** vendored, and must not be dropped in here as a
reference copy: it is 350 KB of `tdefl`, ZIP and zlib-wrapper code that nothing compiles, and
having it sit next to `miniz_tinfl.c` makes the directory look like it ships two implementations.
Keep the upstream archive under `.workbuddy-ai/scratch/` instead — that is where the copy this
section was cut from lives.

`miniz.h` is declaration-only, so vendoring it whole costs nothing at run time — and it is what
supplies the platform blocks (`MINIZ_HAS_64BIT_REGISTERS`, `MINIZ_USE_UNALIGNED_LOADS_AND_STORES`,
`MINIZ_LITTLE_ENDIAN`), the `mz_*` typedefs and the `MZ_*` macros the decompressor needs. Upstream's
repository splits those into `miniz_common.h` and a generated `miniz_export.h`, but **that split
only exists on `master`**; the 3.1.2 release has neither file. Taking them from `master` would mean
vendoring two revisions at once, so the release's own `miniz.h` is used instead.

The compressor (`tdefl`), the ZIP reader/writer and the zlib-compatible wrapper are left out. They
are declared by `miniz.h` but never defined here and never referenced by fastfetch, so nothing of
them reaches the binary.

### What `miniz_tinfl.c` changes

Two things, both structural:

1. The MIT block that *follows* the section in `miniz.c` (lines 3028-3053) is moved to the top of
   the file as a header, and the stray leading space on its first line is dropped.
2. `#include "miniz.h"` is added, because the section no longer sits inside `miniz.c`.

No code was changed, and the MIT block is upstream's own — including its `Copyright 2016 Martin
Raiber` line. If a fix is ever needed, take it from upstream rather than editing here.

### Updating

Re-cut the section from a new release rather than patching this copy. The section starts at the
first `#ifndef MINIZ_NO_INFLATE_APIS` *after* the `tdefl` block and ends at its matching `#endif`;
in 3.1.2 that is `miniz.c` lines 2278-3027, followed by its MIT block on 3028-3053. The `miniz.h`
copy is simply replaced wholesale.

## Configuration

`CMakeLists.txt` compiles `miniz_tinfl.c` and puts `src/3rdparty/miniz` on the include path when
`ENABLE_MINIZ` is on, which is the default. `src/common/impl/inflate.c` is the only file that
includes `miniz.h`.

It is on by default because `src/common/impl/networking_common.c` is always compiled and it
expands any HTTP response that arrives `Content-Encoding: gzip` with `ffInflateGrowing()`. Doing
that by loading zlib at run time cost about 1.5 ms per process on every `publicip` and `weather`
detection, on the module's critical path, which is why the dependency is gone — see
`doc/zlib-vs-inflate.md`. The other two users are `ENABLE_STATIC_TEXT_COMPRESSION`, which stores
the built-in logos and help text compressed, and the Android dex reader, which expands deflated
entries inside a jar.

`-DENABLE_MINIZ=OFF` exists for a distribution that would rather load its own zlib than carry a
vendored copy. It drops the source file from the build, forces
`ENABLE_STATIC_TEXT_COMPRESSION=OFF` with it — a compressed blob only pays for itself when the
decoder is already in the binary — and makes `inflate.c` `dlopen()` the system zlib on the first
call and keep it loaded. `--list-features` reports which of the two ended up in the binary.

`miniz.h` picks `MINIZ_HAS_64BIT_REGISTERS` from the target and leaves
`MINIZ_USE_UNALIGNED_LOADS_AND_STORES` at 0, which is what upstream hard-codes for x86/x64 — the
comment above it still says "Set to 1", but the macro itself is 0 in both 3.1.2 and `master`. That
is upstream's business, not ours; do not "fix" it.

`CMakeLists.txt` defines the `MINIZ_NO_*` macros that reduce the vendored pair to a decompressor
and nothing else: `MINIZ_NO_ARCHIVE_APIS`, `MINIZ_NO_DEFLATE_APIS`, `MINIZ_NO_ZLIB_APIS`,
`MINIZ_NO_STDIO`, `MINIZ_NO_TIME` and `MINIZ_NO_MALLOC`. `MINIZ_NO_MALLOC` is what drops
`tinfl_decompressor_alloc()` / `_free()` — fastfetch keeps the 8 KB decompressor state on the
stack, the way miniz's own `tinfl_decompress_mem_to_mem()` does.

`MINIZ_NO_INFLATE_APIS` must **not** be defined; that is the one that would remove `tinfl`.

The remaining `tinfl_decompress_mem_to_heap()` / `_mem_to_callback()` helpers stay compiled. They
are unreachable from `tinfl_decompress()` and unused here, and this project does not build with
`-ffunction-sections` / `--gc-sections`, so they do sit in `.text`. They are a few hundred bytes;
that is cheaper than carrying a local diff against upstream.
