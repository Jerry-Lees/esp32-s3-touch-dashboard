#pragma once
/* Host stand-in for the ESP32-S3 ROM tinfl, built on zlib. */
#include <zlib.h>
#include <stddef.h>
#include <stdint.h>
#define TINFL_LZ_DICT_SIZE 32768
enum { TINFL_FLAG_PARSE_ZLIB_HEADER = 1, TINFL_FLAG_HAS_MORE_INPUT = 2 };
typedef enum { TINFL_STATUS_FAILED = -1, TINFL_STATUS_DONE = 0, TINFL_STATUS_NEEDS_MORE_INPUT = 1,
               TINFL_STATUS_HAS_MORE_OUTPUT = 2 } tinfl_status;
typedef struct { z_stream z; int init; } tinfl_decompressor;
#define tinfl_init(r) do { (r)->init = 0; } while (0)
static tinfl_status tinfl_decompress(tinfl_decompressor *r, const uint8_t *in, size_t *in_sz,
                                     uint8_t *start, uint8_t *out, size_t *out_sz, unsigned flags)
{
    (void)start; (void)flags;
    if (!r->init) { memset(&r->z, 0, sizeof(r->z)); inflateInit(&r->z); r->init = 1; }
    r->z.next_in = (Bytef *)in; r->z.avail_in = *in_sz;
    r->z.next_out = out; r->z.avail_out = *out_sz;
    int e = inflate(&r->z, Z_NO_FLUSH);
    *in_sz -= r->z.avail_in; *out_sz -= r->z.avail_out;
    if (e == Z_STREAM_END) return TINFL_STATUS_DONE;
    if (e != Z_OK && e != Z_BUF_ERROR) return TINFL_STATUS_FAILED;
    return r->z.avail_out == 0 ? TINFL_STATUS_HAS_MORE_OUTPUT : TINFL_STATUS_NEEDS_MORE_INPUT;
}
