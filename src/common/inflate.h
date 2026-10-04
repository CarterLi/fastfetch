#pragma once

#include "fastfetch.h"

/**
 * Inflates a raw DEFLATE stream (RFC 1951 - no zlib wrapper and no checksum).
 *
 * The decoder behind it is either the vendored miniz (`ENABLE_MINIZ=ON`, the default) or the
 * system zlib loaded at run time (`ENABLE_MINIZ=OFF`). The build picks one; the interface and
 * what the callers observe are the same either way. See `src/common/impl/inflate.c`.
 *
 * It exists to expand three kinds of blob, none of which is a general purpose archive: the
 * compressed ascii logos and help text that `scripts/compress-logos.py` and
 * `scripts/compress-help.py` embed into the binary, a deflated `classes.dex` read out of an
 * Android jar, and the body of an HTTP response that arrived `Content-Encoding: gzip`. The
 * first two are generated at configure time from files in this repository, the third comes off
 * a device and the fourth off the network, so the input is not assumed to be well formed.
 *
 * @param deflateData  the compressed stream
 * @param deflateSize  length of `deflateData`; the stream is not read past it
 * @param rawSize      the expected uncompressed length, also used as a sanity bound
 * @param output       where the result is written, NUL terminated on success
 * @param outputSize   capacity of `output`, terminator included; must be greater than `rawSize`
 *
 * @return the number of bytes written to `output`, or 0 on failure
 */
[[gnu::nonnull(1, 4)]] uint32_t ffInflate(const char* deflateData, uint32_t deflateSize, uint32_t rawSize, char* output, uint32_t outputSize);

/**
 * Inflates a raw DEFLATE stream whose uncompressed length is not known up front.
 *
 * ffInflate() demands the exact length, which is fine for a blob generated at configure time and
 * wrong for a gzip member: the ISIZE field in its trailer is only a hint. It is absent from a
 * concatenated member, wraps modulo 2^32 once the data exceeds 4 GiB, and is simply whatever the
 * last four bytes happened to hold when the response was truncated. So this variant reads until
 * the stream ends, growing `output` whenever the decoder reports it ran out of room. Each grow
 * restarts the decode rather than resuming it: a re-decode costs one pass over data that is
 * already in cache, while resuming would lean on the decoder's internal buffer offset staying
 * meaningful across a `realloc`, and the first attempt fits whenever `initialCapacity` was a good
 * guess.
 *
 * @param deflateData     the compressed stream
 * @param deflateSize     length of `deflateData`; the stream is not read past it
 * @param output          receives the result, NUL terminated on success; previous contents are
 *                        discarded and the capacity is grown as needed
 * @param initialCapacity first output size to try; raised to a floor when it is smaller
 * @param maxSize         upper bound on the uncompressed length, so that a hostile stream cannot
 *                        drive an allocation the process cannot back
 *
 * @return the number of bytes written to `output`, or 0 on failure
 */
[[gnu::nonnull(1, 3)]] uint32_t ffInflateGrowing(const char* deflateData, uint32_t deflateSize, FFstrbuf* output, uint32_t initialCapacity, uint32_t maxSize);

/**
 * Inflates a complete gzip member (RFC 1952): the header, then the DEFLATE stream, stopping before
 * the trailer.
 *
 * Anything handed a `.gz` as-is goes through here rather than skipping the header itself -- an HTTP
 * response body and the man page the SDDM version is read out of are both that shape. The header
 * is variable length (FEXTRA, FNAME, FCOMMENT and FHCRC all add to it), so the parsing is kept in
 * one place instead of being repeated, and therefore drifting, at every call site.
 *
 * The trailer is not read. It holds a CRC32 and an ISIZE, neither of which is needed to decode:
 * the decoder stops at the end of the DEFLATE stream on its own. Not verifying that CRC32 is the
 * one thing this gives up against a full gzip decoder, which refuses a member whose checksum does
 * not match.
 *
 * ISIZE is used, but only to size the first attempt: it is exact for a single member and a hint
 * otherwise (absent from a concatenated one, wrapped modulo 2^32 past 4 GiB, and whatever the
 * sender left there when the input was truncated).
 *
 * @param gzipData  the member, starting at its magic
 * @param gzipSize  length of `gzipData`; the member is not read past it
 * @param output    receives the result, NUL terminated on success; previous contents are discarded
 * @param maxSize   upper bound on the uncompressed length, so that a hostile member cannot drive
 *                  an allocation the process cannot back
 *
 * @return the number of bytes written to `output`, or 0 when the member cannot be decoded
 */
[[gnu::nonnull(1, 3)]] uint32_t ffInflateGzip(const char* gzipData, uint32_t gzipSize, FFstrbuf* output, uint32_t maxSize);
