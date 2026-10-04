#include "fastfetch.h"
#include "common/inflate.h"
#include "common/networking.h"
#include "common/strutil.h"
#include "common/debug.h"

// Upper bound on a decompressed response body. The receive loop already caps the compressed body
// at FF_NETWORKING_MAX_RESPONSE_SIZE, so this is the ratio the decompressor will accept -- and it
// is what keeps a corrupt or hostile stream from driving an allocation the process cannot back:
// FFstrbuf aborts above 2 GiB, while a gzip trailer is four bytes of whatever the sender chose.
#define FF_NETWORKING_MAX_DECOMPRESSED_SIZE (64u * 1024u * 1024u)

// Decompress gzip content
bool ffNetworkingDecompressGzip(FFstrbuf* buffer, char* headerEnd) {
    // `headerEnd` itself is non-null per the `nonnull(2)` contract; what it points to is not expressible as an attribute
    assert(*headerEnd == '\r');

    // Calculate header size
    uint32_t headerSize = (uint32_t) (headerEnd - buffer->chars);

    *headerEnd = '\0'; // Replace delimiter with null character for easier processing
    // Ensure Content-Encoding is in response headers, not in response body
    bool hasGzip = strcasestr(buffer->chars, "\nContent-Encoding: gzip") != nullptr;
    *headerEnd = '\r'; // Restore delimiter

    if (!hasGzip) {
        FF_DEBUG("No gzip compressed content detected, skipping decompression");
        return true;
    }

    FF_DEBUG("Gzip compressed content detected, preparing for decompression");

    const char* bodyStart = headerEnd + 4; // Skip delimiter

    if (buffer->length <= headerSize + 4) {
        // No content to decompress
        FF_DEBUG("Compressed content size is 0, skipping decompression");
        return true;
    }

    // Calculate compressed content size
    uint32_t compressedSize = buffer->length - headerSize - 4;

    // Reads the member's own header, then decodes until the DEFLATE stream ends. Everything
    // specific to the container -- the variable length header, the trailer, the fact that ISIZE is
    // only a hint -- lives in the decoder, so this call site does not have to know about it.
    FF_STRBUF_AUTO_DESTROY decompressedBuffer = ffStrbufCreate();
    if (ffInflateGzip(bodyStart, compressedSize, &decompressedBuffer, FF_NETWORKING_MAX_DECOMPRESSED_SIZE) == 0) {
        FF_DEBUG("Decompression failed");
        return false;
    }

    FF_DEBUG("Successfully decompressed %u bytes compressed data to %u bytes", compressedSize, decompressedBuffer.length);

    // Modify Content-Length header and remove Content-Encoding header
    FF_STRBUF_AUTO_DESTROY newBuffer = ffStrbufCreateA(headerSize + decompressedBuffer.length + 64);

    char* line = nullptr;
    size_t len = 0;
    while (ffStrbufGetline(&line, &len, buffer)) {
        if (ffStrStartsWithIgnCase(line, "Content-Encoding:")) {
            continue;
        } else if (ffStrStartsWithIgnCase(line, "Content-Length:")) {
            ffStrbufAppendF(&newBuffer, "Content-Length: %u\r\n", decompressedBuffer.length);
            continue;
        } else if (line[0] == '\r') {
            ffStrbufAppendS(&newBuffer, "\r\n");
            ffStrbufGetlineRestore(&line, &len, buffer);
            break;
        }

        ffStrbufAppendS(&newBuffer, line); // Including the trailing \r
        ffStrbufAppendC(&newBuffer, '\n');
    }

    ffStrbufAppend(&newBuffer, &decompressedBuffer);
    ffStrbufDestroy(buffer);
    ffStrbufInitMove(buffer, &newBuffer);

    return true;
}

const char* ffNetworkingFindHeader(const char* headers, uint32_t headerEnd, const char* name, uint32_t* valueLen) {
    uint32_t nameLen = (uint32_t) strlen(name);
    uint32_t pos = 0;

    while (pos < headerEnd) {
        uint32_t eol = pos;
        while (eol < headerEnd && headers[eol] != '\n') {
            ++eol;
        }
        uint32_t lineEnd = (eol > pos && headers[eol - 1] == '\r') ? (eol - 1) : eol;

        // Only match at the beginning of a line, so that a value can never be mistaken
        // for a field name (obs-fold continuation lines included)
        if (lineEnd - pos >= nameLen && strncasecmp(headers + pos, name, nameLen) == 0) {
            uint32_t valueStart = pos + nameLen;
            while (valueStart < lineEnd && (headers[valueStart] == ' ' || headers[valueStart] == '\t')) {
                ++valueStart;
            }
            *valueLen = lineEnd - valueStart;
            return headers + valueStart;
        }

        if (eol >= headerEnd) {
            break;
        }
        pos = eol + 1;
    }

    return nullptr;
}

FFNetworkingTransferEncoding ffNetworkingParseTransferEncoding(const char* value, uint32_t valueLen) {
    // The value is a comma-separated list of transfer codings (RFC 9112 6.1)
    uint32_t codingCount = 0;
    const char* coding = nullptr;
    uint32_t codingLen = 0;

    for (uint32_t i = 0, start = 0; i <= valueLen; ++i) {
        if (i < valueLen && value[i] != ',') {
            continue;
        }

        // trim the optional whitespace around the coding
        uint32_t from = start;
        uint32_t to = i;
        while (from < to && (value[from] == ' ' || value[from] == '\t')) {
            ++from;
        }
        while (to > from && (value[to - 1] == ' ' || value[to - 1] == '\t')) {
            --to;
        }

        if (to > from) {
            ++codingCount;
            coding = value + from;
            codingLen = to - from;
        }
        start = i + 1;
    }

    if (codingCount == 0) {
        return FF_NETWORKING_TE_NONE;
    }

    if (codingCount == 1 && codingLen == 7 && strncasecmp(coding, "chunked", 7) == 0) {
        return FF_NETWORKING_TE_CHUNKED;
    }

    return FF_NETWORKING_TE_UNSUPPORTED;
}

int ffNetworkingChunkedComplete(const char* body, uint32_t bodyLen, uint32_t* consumed) {
    uint32_t pos = 0;

    for (;;) {
        // chunk-size [ chunk-ext ] CRLF
        uint32_t eol = pos;
        while (eol < bodyLen && body[eol] != '\n') {
            ++eol;
        }
        if (eol >= bodyLen) {
            return 0; // the chunk-size line is not complete yet
        }

        if (eol == pos || !isxdigit((unsigned char) body[pos])) {
            return -1;
        }

        char* stop = nullptr;
        unsigned long size = strtoul(body + pos, &stop, 16);
        if (stop == body + pos) {
            return -1;
        }
        pos = eol + 1; // skips the chunk extension, which ends at the CRLF

        if (size == 0) {
            // last-chunk, followed by an optional trailer section and an empty line
            uint32_t p = pos;
            while (p < bodyLen) {
                uint32_t e = p;
                while (e < bodyLen && body[e] != '\n') {
                    ++e;
                }
                if (e >= bodyLen) {
                    return 0;
                }
                if (e == p || (e == p + 1 && body[p] == '\r')) {
                    *consumed = e + 1;
                    return 1;
                }
                p = e + 1;
            }
            return 0;
        }

        if (size > (unsigned long) (bodyLen - pos)) {
            return 0; // the chunk data is not complete yet
        }
        pos += (uint32_t) size;

        if (bodyLen - pos < 2) {
            return 0;
        }
        if (body[pos] != '\r' || body[pos + 1] != '\n') {
            return -1;
        }
        pos += 2;
    }
}

// Keeps the status line and every header except `dropHeader` and `Content-Length`,
// then emits a `Content-Length` matching the (already decoded) body.
// `body` may point into `buffer->chars`; it is copied before `buffer` is released.
//
// Returns the offset of the `\r` that opens the terminating CRLF CRLF of the new response. The
// rebuild drops a header line and rewrites another, so the header it produces is shorter than the
// one that went in: the offset the caller held before is no longer the one that describes this
// buffer, and handing it on would point into the middle of the body.
static uint32_t rebuildResponse(FFstrbuf* buffer, uint32_t headerEnd, const char* body, uint32_t bodyLen, const char* dropHeader) {
    FF_STRBUF_AUTO_DESTROY newBuffer = ffStrbufCreateA(headerEnd + bodyLen + 64);

    uint32_t pos = 0;
    while (pos < headerEnd) {
        uint32_t eol = pos;
        while (eol < headerEnd && buffer->chars[eol] != '\n') {
            ++eol;
        }
        uint32_t lineLen = (eol < headerEnd) ? (eol - pos + 1) : (headerEnd - pos);

        if (!ffStrStartsWithIgnCase(buffer->chars + pos, "Content-Length:") &&
            !ffStrStartsWithIgnCase(buffer->chars + pos, dropHeader)) {
            ffStrbufAppendNS(&newBuffer, lineLen, buffer->chars + pos);
        }

        pos += lineLen;
    }

    ffStrbufAppendF(&newBuffer, "Content-Length: %u\r\n\r\n", bodyLen);
    ffStrbufAppendNS(&newBuffer, bodyLen, body);

    ffStrbufDestroy(buffer);
    ffStrbufInitMove(buffer, &newBuffer);

    // The terminating CRLF CRLF is the four bytes in front of the body
    return buffer->length - bodyLen - 4;
}

bool ffNetworkingDecodeChunked(FFstrbuf* buffer, uint32_t* headerEnd) {
    assert(buffer->allocated > 0);

    // Held in a local because the rebuild below hands a different value back
    const uint32_t headerLength = *headerEnd;

    // The header block is terminated by CR LF CR LF
    if (headerLength + 4 > buffer->length) {
        return false;
    }

    char* body = buffer->chars + headerLength + 4;
    uint32_t bodyLen = buffer->length - headerLength - 4;

    uint32_t consumed = 0;
    if (ffNetworkingChunkedComplete(body, bodyLen, &consumed) != 1) {
        FF_DEBUG("Incomplete or malformed chunked body");
        return false;
    }

    // Decoding in place is safe: the encoded form is never shorter than the payload
    char* out = body;
    uint32_t outLen = 0;
    uint32_t pos = 0;

    while (pos < bodyLen) {
        uint32_t eol = pos;
        while (eol < bodyLen && body[eol] != '\n') {
            ++eol;
        }

        unsigned long size = strtoul(body + pos, nullptr, 16);
        pos = eol + 1;
        if (size == 0) {
            break; // last-chunk; trailers are dropped
        }

        memmove(out + outLen, body + pos, size);
        outLen += (uint32_t) size;
        pos += (uint32_t) size + 2; // trailing CRLF
    }

    FF_DEBUG("Decoded chunked body: %u bytes encoded, %u bytes decoded", bodyLen, outLen);
    *headerEnd = rebuildResponse(buffer, headerLength, out, outLen, "Transfer-Encoding:");
    return true;
}
