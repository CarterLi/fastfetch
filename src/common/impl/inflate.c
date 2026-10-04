#include "common/inflate.h"

#include "common/debug.h"

// The decoder behind the wrappers below is either the vendored miniz (ENABLE_MINIZ=ON, the
// default) or the system zlib loaded at run time (ENABLE_MINIZ=OFF). The option exists because a
// distribution may prefer its own zlib over a vendored copy of one; the interface, and everything
// the callers observe, are the same either way.
//
// The gzip container is not part of the choice: ffInflateGzip() reads the member header itself and
// hands the DEFLATE stream to the backend, so neither backend verifies the trailer's CRC32. See
// doc/zlib-vs-inflate.md.

#if FF_HAVE_MINIZ

    #include "miniz.h"

// miniz's tinfl, vendored in src/3rdparty/miniz/. It is a bit-at-a-time decoder with a 10 bit
// lookup table and a 64 bit bit buffer, which on a real 6.2 MB classes.dex decodes at 491 MB/s --
// 1.2x the 416 MB/s of the system zlib, so on a large, high entropy stream it is also faster than
// the library the Android dex reader would otherwise dlopen. See doc/zlib-vs-inflate.md.
//
// Two properties matter more than the speed:
//
//  - It stops at the end of the input it was given. A decoder with no input bound reads past the
//    buffer when handed a truncated stream; that is survivable while every caller passes a blob
//    generated at configure time, and stops being survivable once src/common/android/dex.c hands
//    it bytes out of a mapped jar.
//  - It reports failure rather than writing whatever it managed to decode, so a corrupt stream
//    cannot be mistaken for a short but valid one.
[[gnu::hot]]
uint32_t ffInflate(const char* deflateData, uint32_t deflateSize, uint32_t rawSize, char* output, uint32_t outputSize) {
    if (rawSize == 0 || rawSize >= outputSize || deflateSize == 0) return 0;

    // About 8 KB, and all of the decoder's working state lives in it, so it cannot be shared:
    // fastfetch detects modules on parallel threads and more than one of them reaches this.
    // tinfl_init() only resets the state index, so there is no 8 KB clear on the way in.
    tinfl_decompressor decompressor;
    tinfl_init(&decompressor);

    size_t inSize = deflateSize;
    size_t outSize = rawSize;
    const tinfl_status status = tinfl_decompress(
        &decompressor,
        (const mz_uint8*) deflateData, &inSize,
        (mz_uint8*) output, (mz_uint8*) output,
        &outSize,
        // The whole stream has to fit in `output`, which it does by construction: rawSize is the
        // length of the blob being restored. Without this flag tinfl wants a 32 KB dictionary
        // window and wraps the output around it.
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF
    );

    if (status != TINFL_STATUS_DONE || outSize != rawSize) return 0;

    output[rawSize] = '\0';
    return rawSize;
}

// Reads until the stream ends, growing the output whenever tinfl reports it ran out of room.
//
// Every grow restarts the decode instead of resuming it. Resuming works -- tinfl keeps its write
// position as an offset rather than a pointer, so it survives the buffer moving -- but only in
// the NON_WRAPPING mode used here, and it makes the loop's correctness depend on an internal
// detail of the vendored copy. A re-decode costs one pass over data that is already in cache,
// and only ever happens when the caller's guess was too small.
uint32_t ffInflateGrowing(const char* deflateData, uint32_t deflateSize, FFstrbuf* output, uint32_t initialCapacity, uint32_t maxSize) {
    if (deflateSize == 0 || maxSize == 0) return 0;

    // Below a kilobyte a grow saves nothing and costs a full re-decode, so the floor is worth
    // more than honouring a tiny hint.
    uint32_t capacity = initialCapacity < 1024 ? 1024 : initialCapacity;
    if (capacity > maxSize) capacity = maxSize;

    ffStrbufClear(output);

    for (;;) {
        ffStrbufEnsureFree(output, capacity);

        // About 8 KB, and all of the decoder's working state lives in it, so it cannot be shared:
        // fastfetch detects modules on parallel threads and more than one of them reaches this.
        // tinfl_init() only resets the state index, so there is no 8 KB clear on the way in.
        tinfl_decompressor decompressor;
        tinfl_init(&decompressor);

        size_t inSize = deflateSize;
        size_t outSize = capacity;
        const tinfl_status status = tinfl_decompress(
            &decompressor,
            (const mz_uint8*) deflateData, &inSize,
            (mz_uint8*) output->chars, (mz_uint8*) output->chars,
            &outSize,
            TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF
        );

        if (status == TINFL_STATUS_DONE) {
            output->length = (uint32_t) outSize;
            output->chars[output->length] = '\0';
            return output->length;
        }

        if (status != TINFL_STATUS_HAS_MORE_OUTPUT) {
            FF_DEBUG("Inflate failed with tinfl status %d after %u bytes of output", (int) status, (uint32_t) outSize);
            return 0;
        }

        if (capacity >= maxSize) {
            FF_DEBUG("Inflate needs more than the %u byte limit", maxSize);
            return 0;
        }

        FF_DEBUG("Inflate ran out of room at %u bytes, retrying with a larger buffer", capacity);
        capacity = capacity > maxSize / 2 ? maxSize : capacity * 2;
    }
}

#elif FF_HAVE_ZLIB

    #include "common/library.h"

    #include <zlib.h>

// The system zlib, loaded on the first call and never unloaded.
//
// Loading it once is what makes this backend worth having at all. A process that maps the library,
// calls into it and unmaps it again pays the cold map on every call -- measured at ~1.5 ms on
// Windows (doc/zlib-vs-inflate.md 9.8) -- while loading a library that is already mapped costs
// three orders of magnitude less. The handle is therefore leaked rather than kept: fastfetch links
// libraries with `BINARY_LINK_TYPE=dlopen`, so the function pointers below would dangle the moment
// the library was unloaded.
//
// fastfetch detects modules on parallel threads and more than one of them decompresses (an HTTP
// gzip body, an Android dex), so the load is guarded by a plain flag. Two threads arriving at once
// both see `inited == false` and both load; dlopen() is itself thread safe and the second one gets
// the same handle, so all that can be observed is a symbol pointer being written twice with the
// same value.
struct FFZlibData {
    FF_LIBRARY_SYMBOL(inflateInit2_)
    FF_LIBRARY_SYMBOL(inflate)
    FF_LIBRARY_SYMBOL(inflateEnd)

    bool inited;
} zlibData;

static bool loadZlib(void) {
    if (!zlibData.inited) {
        zlibData.inited = true;
        FF_LIBRARY_LOAD(zlib, false, "libz" FF_LIBRARY_EXTENSION, 2, "zlib1" FF_LIBRARY_EXTENSION, 2)
        FF_LIBRARY_LOAD_SYMBOL_VAR(zlib, zlibData, inflateInit2_, false)
        FF_LIBRARY_LOAD_SYMBOL_VAR(zlib, zlibData, inflate, false)
        FF_LIBRARY_LOAD_SYMBOL_VAR(zlib, zlibData, inflateEnd, false)
        zlib = nullptr; // don't auto dlclose: the symbols above point into it
    }
    return zlibData.ffinflateEnd != nullptr;
}

uint32_t ffInflate(const char* deflateData, uint32_t deflateSize, uint32_t rawSize, char* output, uint32_t outputSize) {
    if (rawSize == 0 || rawSize >= outputSize || deflateSize == 0) return 0;
    if (!loadZlib()) return 0;

    z_stream stream = {};
    stream.next_in = (Bytef*) deflateData;
    stream.avail_in = (uInt) deflateSize;
    stream.next_out = (Bytef*) output;
    stream.avail_out = (uInt) rawSize;

    // `uncompress` is not usable here: a zip entry holds a raw deflate stream, without the two
    // byte zlib header that entry point insists on. A negative window size tells inflate to skip
    // the header, which is what the zip format expects.
    if (zlibData.ffinflateInit2_(&stream, -MAX_WBITS, ZLIB_VERSION, (int) sizeof(z_stream)) != Z_OK) {
        FF_DEBUG("inflateInit2() failed");
        return 0;
    }
    const int status = zlibData.ffinflate(&stream, Z_FINISH);
    zlibData.ffinflateEnd(&stream);

    if (status != Z_STREAM_END || stream.total_out != rawSize) {
        FF_DEBUG("Inflate failed with zlib status %d after %lu bytes of output", status, stream.total_out);
        return 0;
    }

    output[rawSize] = '\0';
    return rawSize;
}

// Reads until the stream ends, growing the output whenever the decoder reports it ran out of room.
//
// Every grow restarts the decode instead of resuming it, matching the tinfl backend: a re-decode
// costs one pass over data that is already in cache and only ever happens when the caller's guess
// was too small, while resuming would mean keeping a z_stream alive across a `realloc` and
// depending on the buffer not having moved.
static uint32_t inflateGrowing(const char* deflateData, uint32_t deflateSize, FFstrbuf* output, uint32_t initialCapacity, uint32_t maxSize) {
    // Below a kilobyte a grow saves nothing and costs a full re-decode, so the floor is worth
    // more than honouring a tiny hint.
    uint32_t capacity = initialCapacity < 1024 ? 1024 : initialCapacity;
    if (capacity > maxSize) capacity = maxSize;

    ffStrbufClear(output);

    for (;;) {
        ffStrbufEnsureFree(output, capacity);

        z_stream stream = {};
        stream.next_in = (Bytef*) deflateData;
        stream.avail_in = (uInt) deflateSize;
        stream.next_out = (Bytef*) output->chars;
        stream.avail_out = (uInt) ffStrbufGetFree(output);

        if (zlibData.ffinflateInit2_(&stream, -MAX_WBITS, ZLIB_VERSION, (int) sizeof(z_stream)) != Z_OK) {
            FF_DEBUG("inflateInit2() failed");
            return 0;
        }
        const int status = zlibData.ffinflate(&stream, Z_FINISH);
        const uint32_t produced = (uint32_t) stream.total_out;
        zlibData.ffinflateEnd(&stream);

        if (status == Z_STREAM_END) {
            output->length = produced;
            output->chars[produced] = '\0';
            return produced;
        }

        // A buffer that is not full means the input ran out before the stream ended -- the stream
        // is truncated or corrupt, and a larger buffer cannot change that. Only a full buffer is
        // worth another, larger attempt; without this the loop below would re-decode the same
        // bytes at every size up to `maxSize`.
        if (stream.avail_out != 0) {
            FF_DEBUG("Inflate failed with zlib status %d after %u bytes of output", status, produced);
            return 0;
        }

        if (capacity >= maxSize) {
            FF_DEBUG("Inflate needs more than the %u byte limit", maxSize);
            return 0;
        }

        FF_DEBUG("Inflate ran out of room at %u bytes, retrying with a larger buffer", capacity);
        capacity = capacity > maxSize / 2 ? maxSize : capacity * 2;
    }
}

uint32_t ffInflateGrowing(const char* deflateData, uint32_t deflateSize, FFstrbuf* output, uint32_t initialCapacity, uint32_t maxSize) {
    if (deflateSize == 0 || maxSize == 0) return 0;
    if (!loadZlib()) return 0;
    return inflateGrowing(deflateData, deflateSize, output, initialCapacity, maxSize);
}

#else

uint32_t ffInflate(const char* deflateData, uint32_t deflateSize, uint32_t rawSize, char* output, uint32_t outputSize) {
    FF_UNUSED(deflateData, deflateSize, rawSize, output, outputSize);
    return 0; // Not implemented without zlib
}

#endif

// ---------------------------------------------------------------------------------------------
// The gzip container, shared by both backends
// ---------------------------------------------------------------------------------------------

// Length of the gzip member header (RFC 1952 2.3) at the front of `data`, or 0 when it is
// malformed or runs past the end.
//
// The fixed 10 bytes are ID1, ID2, CM, FLG, MTIME, XFL and OS. Everything after them is optional
// and variable length, which is why this exists at all: a member carrying FNAME or FEXTRA would
// otherwise be handed to the decoder starting inside its own header.
static uint32_t gzipHeaderSize(const uint8_t* data, uint32_t dataSize) {
    if (dataSize < 10) return 0;
    if (data[0] != 0x1f || data[1] != 0x8b || data[2] != 8) return 0; // ID1, ID2, CM = deflate
    const uint8_t flags = data[3];
    if (flags & 0xe0) return 0; // reserved bits, which must be zero

    uint32_t offset = 10;

    if (flags & 0x04) { // FEXTRA
        if (dataSize - offset < 2) return 0;
        const uint32_t extraLen = (uint32_t) data[offset] | ((uint32_t) data[offset + 1] << 8);
        offset += 2;
        if (dataSize - offset < extraLen) return 0;
        offset += extraLen;
    }

    // FNAME (0x08) and FCOMMENT (0x10) are both NUL terminated strings
    for (uint8_t bit = 0x08; bit <= 0x10; bit <<= 1) {
        if (!(flags & bit)) continue;
        while (offset < dataSize && data[offset] != '\0') ++offset;
        if (offset >= dataSize) return 0;
        ++offset; // the terminator itself
    }

    if (flags & 0x02) { // FHCRC
        if (dataSize - offset < 2) return 0;
        offset += 2;
    }

    return offset;
}

// The ISIZE field in the trailer, which is the uncompressed length modulo 2^32 -- so a usable
// starting size for a single member, and a guess otherwise. 0 when there is no trailer to read.
static uint32_t gzipUncompressedSize(const uint8_t* data, uint32_t dataSize) {
    if (dataSize < 18) return 0;

    const uint8_t* tail = data + dataSize - 4;
    return (uint32_t) tail[0] | ((uint32_t) tail[1] << 8u) | ((uint32_t) tail[2] << 16u) | ((uint32_t) tail[3] << 24u);
}

uint32_t ffInflateGzip(const char* gzipData, uint32_t gzipSize, FFstrbuf* output, uint32_t maxSize) {
    if (gzipSize == 0 || maxSize == 0) return 0;

    const uint32_t headerSize = gzipHeaderSize((const uint8_t*) gzipData, gzipSize);
    if (headerSize == 0 || headerSize >= gzipSize) {
        FF_DEBUG("Not a readable gzip member, skipping decompression");
        return 0;
    }

    // Clamping is not optional: ISIZE is four bytes the sender chose, and FFstrbuf aborts rather
    // than fail when asked for an allocation it cannot back.
    uint32_t sizeHint = gzipUncompressedSize((const uint8_t*) gzipData, gzipSize);
    if (sizeHint > maxSize) {
        FF_DEBUG("Gzip trailer claims %u bytes, clamping to the %u byte limit", sizeHint, maxSize);
        sizeHint = maxSize;
    }

    const uint32_t rawSize = ffInflateGrowing(gzipData + headerSize, gzipSize - headerSize, output, sizeHint, maxSize);
    if (rawSize == 0) {
        FF_DEBUG("Decompression failed");
        return 0;
    }

    FF_DEBUG("Successfully decompressed %u bytes of gzip data to %u bytes", gzipSize, rawSize);
    return rawSize;
}
