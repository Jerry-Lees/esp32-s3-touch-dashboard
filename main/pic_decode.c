/*
 * PNG and GIF readers (see pic_decode.h).
 */
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "miniz.h"

#include "pic_decode.h"

#define MAX_SIDE     30000
#define MAX_PIXELS   (80 * 1000 * 1000)  /* at 1/8 that's still ~1.2 MP */
#define MAX_SHIFT    3

static void *ps_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void *ps_calloc(size_t n, size_t s) { return heap_caps_calloc(n, s, MALLOC_CAP_SPIRAM); }

static inline uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
    return (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3;
}

/* ---------- shrinking row sink: full-size RGB888 rows in, img out ---------- */

typedef struct {
    pic_img_t *img;
    int        src_w, src_h, step;
    uint32_t  *acc;                /* img->w * 3 sums for the current output row */
    int        row;                /* source rows received */
} sink_t;

static bool sink_begin(sink_t *s, pic_img_t *img, int w, int h, pic_scale_fn scale,
                       const void *arg, char *err)
{
    memset(s, 0, sizeof(*s));
    int sh = scale(w, h, arg);
    if (sh < 0) sh = 0;
    if (sh > MAX_SHIFT) sh = MAX_SHIFT;
    s->img = img;
    s->src_w = w;
    s->src_h = h;
    s->step = 1 << sh;
    img->w = (w + s->step - 1) / s->step;
    img->h = (h + s->step - 1) / s->step;
    img->px = ps_calloc((size_t)img->w * img->h, 2);
    s->acc = ps_calloc((size_t)img->w * 3, sizeof(uint32_t));
    if (!img->px || !s->acc) {
        strcpy(err, "too big for memory");
        return false;
    }
    return true;
}

static void sink_row(sink_t *s, const uint8_t *rgb)
{
    if (s->row >= s->src_h) return;
    for (int x = 0; x < s->src_w; x++) {
        uint32_t *a = &s->acc[(x / s->step) * 3];
        a[0] += rgb[x * 3];
        a[1] += rgb[x * 3 + 1];
        a[2] += rgb[x * 3 + 2];
    }
    s->row++;
    if (s->row % s->step == 0 || s->row == s->src_h) {
        int rows = (s->row - 1) % s->step + 1;
        int oy = (s->row - 1) / s->step;
        uint16_t *out = s->img->px + (size_t)oy * s->img->w;
        for (int ox = 0; ox < s->img->w; ox++) {
            int cols = s->src_w - ox * s->step;
            if (cols > s->step) cols = s->step;
            uint32_t n = (uint32_t)rows * cols, *a = &s->acc[ox * 3];
            out[ox] = rgb565(a[0] / n, a[1] / n, a[2] / n);
        }
        memset(s->acc, 0, (size_t)s->img->w * 3 * sizeof(uint32_t));
    }
    if (s->row % 128 == 0) vTaskDelay(1);   /* big pictures take a while: let others run */
}

static void sink_end(sink_t *s)
{
    free(s->acc);
    s->acc = NULL;
}

/* ======================= PNG ======================= */

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

typedef struct {
    int      w, h, depth, ctype, chans, bpp;   /* bpp = bytes per pixel for filters (>= 1) */
    size_t   stride;                           /* bytes per row, without the filter byte */
    uint8_t  pal[256][3], alpha[256];
    int      npal;
    bool     has_key;                          /* tRNS colour key (grey / RGB) */
    uint16_t key[3];
    uint8_t *cur, *prev, *rgb;                 /* cur/prev: stride + 1 (filter byte first) */
    size_t   fill;                             /* bytes of cur filled */
    int      rows;                             /* rows finished */
    sink_t   sink;
    bool     bad;
} png_t;

static inline int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static bool png_unfilter(png_t *p)
{
    uint8_t *c = p->cur + 1;
    const uint8_t *u = p->prev + 1;
    size_t n = p->stride, bpp = p->bpp;
    switch (p->cur[0]) {
    case 0: break;
    case 1: for (size_t i = bpp; i < n; i++) c[i] += c[i - bpp]; break;
    case 2: for (size_t i = 0; i < n; i++) c[i] += u[i]; break;
    case 3:
        for (size_t i = 0; i < n; i++) c[i] += ((i >= bpp ? c[i - bpp] : 0) + u[i]) >> 1;
        break;
    case 4:
        for (size_t i = 0; i < n; i++) {
            c[i] += i >= bpp ? paeth(c[i - bpp], u[i], u[i - bpp]) : paeth(0, u[i], 0);
        }
        break;
    default: return false;
    }
    return true;
}

static inline unsigned png_bits(const uint8_t *row, int x, int d)
{
    size_t bit = (size_t)x * d;
    return (row[bit >> 3] >> (8 - d - (bit & 7))) & ((1u << d) - 1);
}

static void png_to_rgb(const png_t *p, const uint8_t *s, uint8_t *o)
{
    int d = p->depth;
    for (int x = 0; x < p->w; x++, o += 3) {
        unsigned r, g, b, a = 255;
        if (p->ctype == 3) {                         /* palette */
            unsigned i = d == 8 ? s[x] : png_bits(s, x, d);
            if ((int)i < p->npal) {
                r = p->pal[i][0]; g = p->pal[i][1]; b = p->pal[i][2]; a = p->alpha[i];
            } else {
                r = g = b = 0;
            }
        } else if (d == 16) {
            const uint8_t *q = s + (size_t)x * p->chans * 2;
            unsigned v0 = q[0] << 8 | q[1];
            if (p->ctype == 0 || p->ctype == 4) {    /* grey (+ alpha) */
                r = g = b = q[0];
                if (p->ctype == 4) a = q[2];
                else if (p->has_key && v0 == p->key[0]) a = 0;
            } else {                                 /* RGB (+ alpha) */
                r = q[0]; g = q[2]; b = q[4];
                if (p->ctype == 6) a = q[6];
                else if (p->has_key && v0 == p->key[0] && (unsigned)(q[2] << 8 | q[3]) == p->key[1] &&
                         (unsigned)(q[4] << 8 | q[5]) == p->key[2]) a = 0;
            }
        } else if (p->ctype == 0) {                  /* grey, 1-8 bits */
            unsigned v = d == 8 ? s[x] : png_bits(s, x, d);
            r = g = b = v * 255 / ((1u << d) - 1);
            if (p->has_key && v == p->key[0]) a = 0;
        } else {                                     /* 8-bit grey+alpha, RGB, RGBA */
            const uint8_t *q = s + (size_t)x * p->chans;
            if (p->ctype == 4) {
                r = g = b = q[0]; a = q[1];
            } else {
                r = q[0]; g = q[1]; b = q[2];
                if (p->ctype == 6) a = q[3];
                else if (p->has_key && r == p->key[0] && g == p->key[1] && b == p->key[2]) a = 0;
            }
        }
        if (a != 255) {                              /* on black */
            r = r * a / 255; g = g * a / 255; b = b * a / 255;
        }
        o[0] = r; o[1] = g; o[2] = b;
    }
}

/* Inflated bytes in: cut into rows. */
static void png_consume(png_t *p, const uint8_t *data, size_t n)
{
    while (n && !p->bad && p->rows < p->h) {
        size_t take = p->stride + 1 - p->fill;
        if (take > n) take = n;
        memcpy(p->cur + p->fill, data, take);
        p->fill += take;
        data += take;
        n -= take;
        if (p->fill == p->stride + 1) {
            if (!png_unfilter(p)) {
                p->bad = true;
                return;
            }
            png_to_rgb(p, p->cur + 1, p->rgb);
            sink_row(&p->sink, p->rgb);
            uint8_t *t = p->prev; p->prev = p->cur; p->cur = t;
            p->fill = 0;
            p->rows++;
        }
    }
}

static bool png_header(png_t *p, const uint8_t *h, char *err)
{
    p->w = (int)be32(h);
    p->h = (int)be32(h + 4);
    p->depth = h[8];
    p->ctype = h[9];
    if (be32(h) == 0 || be32(h + 4) == 0 || be32(h) > MAX_SIDE || be32(h + 4) > MAX_SIDE ||
        (int64_t)p->w * p->h > MAX_PIXELS) {
        strcpy(err, "PNG size not supported");
        return false;
    }
    if (h[10] != 0 || h[11] != 0) {
        strcpy(err, "not a standard PNG");
        return false;
    }
    if (h[12] != 0) {
        strcpy(err, "interlaced PNG (not supported)");
        return false;
    }
    int d = p->depth;
    bool ok;
    switch (p->ctype) {
    case 0: p->chans = 1; ok = d == 1 || d == 2 || d == 4 || d == 8 || d == 16; break;
    case 2: p->chans = 3; ok = d == 8 || d == 16; break;
    case 3: p->chans = 1; ok = d == 1 || d == 2 || d == 4 || d == 8; break;
    case 4: p->chans = 2; ok = d == 8 || d == 16; break;
    case 6: p->chans = 4; ok = d == 8 || d == 16; break;
    default: ok = false;
    }
    if (!ok) {
        strcpy(err, "damaged PNG (bad colour format)");
        return false;
    }
    p->stride = ((size_t)p->w * p->chans * d + 7) / 8;
    p->bpp = p->chans * d / 8 < 1 ? 1 : p->chans * d / 8;
    return true;
}

bool pic_load_png(FILE *f, pic_img_t *img, pic_scale_fn scale, const void *arg, char *err)
{
    static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    uint8_t hdr[8];
    if (fread(hdr, 1, 8, f) != 8 || memcmp(hdr, SIG, 8) != 0) {
        strcpy(err, "not a PNG file");
        return false;
    }
    png_t *p = ps_calloc(1, sizeof(png_t));
    tinfl_decompressor *z = ps_malloc(sizeof(tinfl_decompressor));
    uint8_t *dict = ps_malloc(TINFL_LZ_DICT_SIZE);
    uint8_t *in = ps_malloc(4096);
    bool ok = false, have_hdr = false, started = false, z_done = false;
    size_t dict_ofs = 0;
    if (!p || !z || !dict || !in) {
        strcpy(err, "out of memory");
        goto out;
    }
    memset(p->alpha, 255, sizeof(p->alpha));
    tinfl_init(z);

    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) {
            strcpy(err, "damaged PNG (cut short)");
            goto out;
        }
        uint32_t len = be32(ch);
        if (len > 0x40000000) {
            strcpy(err, "damaged PNG (bad chunk)");
            goto out;
        }
        bool crc_follows = true;
        if (!memcmp(ch + 4, "IHDR", 4)) {
            uint8_t h[13];
            if (have_hdr || len != 13 || fread(h, 1, 13, f) != 13) {
                strcpy(err, "damaged PNG (bad header)");
                goto out;
            }
            if (!png_header(p, h, err)) goto out;
            have_hdr = true;
        } else if (!have_hdr) {
            strcpy(err, "damaged PNG (no header)");
            goto out;
        } else if (!memcmp(ch + 4, "PLTE", 4) && !started) {
            if (len % 3 || len > 768) {
                strcpy(err, "damaged PNG (bad palette)");
                goto out;
            }
            p->npal = len / 3;
            if (fread(p->pal, 1, len, f) != len) {
                strcpy(err, "damaged PNG (cut short)");
                goto out;
            }
        } else if (!memcmp(ch + 4, "tRNS", 4) && !started) {
            uint8_t t[256];
            if (len > 256 || fread(t, 1, len, f) != len) {
                strcpy(err, "damaged PNG (bad transparency)");
                goto out;
            }
            if (p->ctype == 3) {
                memcpy(p->alpha, t, len);
            } else if (p->ctype == 0 && len >= 2) {
                p->key[0] = t[0] << 8 | t[1];
                p->has_key = true;
            } else if (p->ctype == 2 && len >= 6) {
                for (int i = 0; i < 3; i++) p->key[i] = t[i * 2] << 8 | t[i * 2 + 1];
                if (p->depth == 8) {               /* compare against the 8-bit samples */
                    for (int i = 0; i < 3; i++) p->key[i] &= 0xFF;
                }
                p->has_key = true;
            }
        } else if (!memcmp(ch + 4, "IDAT", 4)) {
            if (!started) {
                if (p->ctype == 3 && p->npal == 0) {
                    strcpy(err, "damaged PNG (no palette)");
                    goto out;
                }
                p->cur = ps_calloc(p->stride + 1, 1);
                p->prev = ps_calloc(p->stride + 1, 1);     /* row "above" the first: zeros */
                p->rgb = ps_malloc((size_t)p->w * 3);
                if (!p->cur || !p->prev || !p->rgb) {
                    strcpy(err, "too big for memory");
                    goto out;
                }
                if (!sink_begin(&p->sink, img, p->w, p->h, scale, arg, err)) goto out;
                started = true;
            }
            while (len) {
                size_t n = len > 4096 ? 4096 : len;
                if (fread(in, 1, n, f) != n) {
                    strcpy(err, "damaged PNG (cut short)");
                    goto out;
                }
                len -= n;
                const uint8_t *q = in;
                while (n && !z_done) {
                    size_t in_sz = n, out_sz = TINFL_LZ_DICT_SIZE - dict_ofs;
                    tinfl_status st = tinfl_decompress(z, q, &in_sz, dict, dict + dict_ofs, &out_sz,
                                                       TINFL_FLAG_PARSE_ZLIB_HEADER |
                                                       TINFL_FLAG_HAS_MORE_INPUT);
                    if (in_sz > n) in_sz = n;
                    q += in_sz;
                    n -= in_sz;
                    png_consume(p, dict + dict_ofs, out_sz);
                    dict_ofs = (dict_ofs + out_sz) & (TINFL_LZ_DICT_SIZE - 1);
                    if (st < 0 || p->bad) {
                        strcpy(err, "damaged PNG (bad image data)");
                        goto out;
                    }
                    if (st == TINFL_STATUS_DONE) z_done = true;
                    else if (in_sz == 0 && out_sz == 0) break;   /* wants more input */
                }
                if (p->rows >= p->h) break;
            }
            if (p->rows >= p->h || z_done) break;      /* the rest of the file isn't needed */
        } else if (!memcmp(ch + 4, "IEND", 4)) {
            break;
        } else {
            crc_follows = false;                       /* skip the data and the CRC */
            if (fseek(f, (long)len + 4, SEEK_CUR) != 0) {
                strcpy(err, "damaged PNG (cut short)");
                goto out;
            }
        }
        if (crc_follows) fseek(f, 4, SEEK_CUR);        /* CRCs aren't checked */
    }
    if (p->rows < p->h) {
        strcpy(err, started ? "damaged PNG (picture incomplete)" : "damaged PNG (no image data)");
        goto out;
    }
    ok = true;
out:
    if (p) {
        sink_end(&p->sink);
        free(p->cur);
        free(p->prev);
        free(p->rgb);
    }
    free(p);
    free(z);
    free(dict);
    free(in);
    return ok;
}

/* ======================= GIF (first frame) ======================= */

typedef struct {
    FILE    *f;
    bool     eof;
    int      block_left;           /* bytes left in the current data sub-block */
    bool     blocks_done;          /* hit the 0-length terminator */
    uint32_t bits;
    int      nbits;
} gif_rd_t;

static int rd_byte(gif_rd_t *r)
{
    int c = fgetc(r->f);
    if (c == EOF) r->eof = true;
    return c == EOF ? 0 : c;
}

static int rd_u16(gif_rd_t *r)
{
    int lo = rd_byte(r);
    return lo | rd_byte(r) << 8;
}

static bool skip_sub_blocks(gif_rd_t *r)
{
    for (;;) {
        int n = rd_byte(r);
        if (r->eof) return false;
        if (n == 0) return true;
        if (fseek(r->f, n, SEEK_CUR) != 0) return false;
    }
}

/* Next LZW code of `size` bits from the image data sub-blocks; -1 at the end. */
static int rd_code(gif_rd_t *r, int size)
{
    while (r->nbits < size) {
        if (r->block_left == 0) {
            if (r->blocks_done) return -1;
            r->block_left = rd_byte(r);
            if (r->eof || r->block_left == 0) {
                r->blocks_done = true;
                return -1;
            }
        }
        r->bits |= (uint32_t)rd_byte(r) << r->nbits;
        r->nbits += 8;
        r->block_left--;
        if (r->eof) return -1;
    }
    int c = r->bits & ((1u << size) - 1);
    r->bits >>= size;
    r->nbits -= size;
    return c;
}

/* Row index in the frame of the n-th row stored (interlaced: 4 passes). */
static int gif_row(int n, int h, bool interlaced)
{
    if (!interlaced) return n;
    static const int start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };
    for (int pass = 0; pass < 4; pass++) {
        int rows = start[pass] < h ? (h - start[pass] + step[pass] - 1) / step[pass] : 0;
        if (n < rows) return start[pass] + n * step[pass];
        n -= rows;
    }
    return h - 1;
}

typedef struct {
    uint16_t prefix[4096];
    uint8_t  suffix[4096];
    uint8_t  stack[4097];
} lzw_t;

/* Decodes the frame's colour indexes into idx (fw * fh). */
static bool gif_lzw(gif_rd_t *r, uint8_t *idx, int fw, int fh, bool interlaced, char *err)
{
    int min = rd_byte(r);
    if (r->eof || min < 1 || min > 8) {
        strcpy(err, "damaged GIF (bad image data)");
        return false;
    }
    lzw_t *t = ps_malloc(sizeof(lzw_t));
    if (!t) {
        strcpy(err, "out of memory");
        return false;
    }
    int clear = 1 << min, eoi = clear + 1, next = clear + 2, size = min + 1;
    int old = -1, first = 0;
    for (int i = 0; i < clear; i++) {
        t->prefix[i] = 0xFFFF;
        t->suffix[i] = (uint8_t)i;
    }
    size_t total = (size_t)fw * fh, pos = 0;
    while (pos < total) {
        int code = rd_code(r, size);
        if (code < 0 || code == eoi) break;
        if (code == clear) {
            next = clear + 2;
            size = min + 1;
            old = -1;
            continue;
        }
        int sp = 0, in = code;
        if (old < 0) {                                  /* first code after a clear */
            if (code >= clear) break;                   /* invalid: stop here */
            t->stack[sp++] = (uint8_t)code;
            first = code;
        } else {
            if (code > next) break;                     /* invalid code */
            if (code == next) {                         /* "KwKwK" case */
                if (next >= 4096) break;
                t->stack[sp++] = (uint8_t)first;
                code = old;
            }
            while (code >= clear) {                     /* walk back to a root */
                if (sp >= 4096 || code >= next) goto bad;
                t->stack[sp++] = t->suffix[code];
                code = t->prefix[code];
            }
            first = code;
            t->stack[sp++] = (uint8_t)first;
            if (next < 4096) {
                t->prefix[next] = (uint16_t)old;
                t->suffix[next] = (uint8_t)first;
                next++;
                if (next == (1 << size) && size < 12) size++;
            }
        }
        old = in;
        while (sp > 0 && pos < total) {                 /* stack holds the string reversed */
            int y = gif_row((int)(pos / fw), fh, interlaced);
            idx[(size_t)y * fw + pos % fw] = t->stack[--sp];
            pos++;
        }
    }
    free(t);
    /* A frame that ends early keeps index 0 in the missing part: shown anyway. */
    if (!r->blocks_done) skip_sub_blocks(r);
    return true;
bad:
    free(t);
    strcpy(err, "damaged GIF (bad image data)");
    return false;
}

bool pic_load_gif(FILE *f, pic_img_t *img, pic_scale_fn scale, const void *arg, char *err)
{
    gif_rd_t r = { .f = f };
    uint8_t sig[6];
    if (fread(sig, 1, 6, f) != 6 || (memcmp(sig, "GIF87a", 6) && memcmp(sig, "GIF89a", 6))) {
        strcpy(err, "not a GIF file");
        return false;
    }
    int w = rd_u16(&r), h = rd_u16(&r), flags = rd_byte(&r);
    rd_byte(&r);                                        /* background colour: shown black */
    rd_byte(&r);                                        /* aspect ratio */
    if (r.eof || w == 0 || h == 0 || (int64_t)w * h > MAX_PIXELS) {
        strcpy(err, "damaged GIF (bad header)");
        return false;
    }
    uint8_t (*gct)[3] = ps_calloc(256, 3), (*lct)[3] = ps_calloc(256, 3);
    uint8_t *idx = NULL, *rgb = NULL;
    sink_t sink = {0};
    bool ok = false, transparent = false;
    int tindex = 0;
    if (!gct || !lct) {
        strcpy(err, "out of memory");
        goto out;
    }
    if (flags & 0x80) {
        size_t n = (size_t)3 << ((flags & 7) + 1);
        if (fread(gct, 1, n, f) != n) {
            strcpy(err, "damaged GIF (cut short)");
            goto out;
        }
    }
    for (;;) {
        int sep = rd_byte(&r);
        if (r.eof || sep == ';') {
            strcpy(err, "damaged GIF (no picture in it)");
            goto out;
        }
        if (sep == '!') {
            int label = rd_byte(&r);
            if (label == 0xF9) {                        /* graphic control: transparency */
                int n = rd_byte(&r);
                if (n == 4) {
                    int pf = rd_byte(&r);
                    rd_u16(&r);                         /* delay */
                    tindex = rd_byte(&r);
                    transparent = pf & 1;
                } else if (n > 0) {
                    fseek(f, n, SEEK_CUR);
                }
            }
            if (!skip_sub_blocks(&r)) {
                strcpy(err, "damaged GIF (cut short)");
                goto out;
            }
            continue;
        }
        if (sep != ',') {
            strcpy(err, "damaged GIF (bad block)");
            goto out;
        }
        int fx = rd_u16(&r), fy = rd_u16(&r), fw = rd_u16(&r), fh = rd_u16(&r);
        int ff = rd_byte(&r);
        if (r.eof || fw == 0 || fh == 0) {
            strcpy(err, "damaged GIF (bad frame)");
            goto out;
        }
        uint8_t (*pal)[3] = gct;
        if (ff & 0x80) {
            size_t n = (size_t)3 << ((ff & 7) + 1);
            if (fread(lct, 1, n, f) != n) {
                strcpy(err, "damaged GIF (cut short)");
                goto out;
            }
            pal = lct;
        }
        idx = ps_calloc((size_t)fw * fh, 1);
        rgb = ps_malloc((size_t)w * 3);
        if (!idx || !rgb) {
            strcpy(err, "too big for memory");
            goto out;
        }
        if (!gif_lzw(&r, idx, fw, fh, ff & 0x40, err)) goto out;
        if (!sink_begin(&sink, img, w, h, scale, arg, err)) goto out;
        /* Lay the frame on the (black) logical screen, clipped to it. */
        for (int y = 0; y < h; y++) {
            memset(rgb, 0, (size_t)w * 3);
            int fy_row = y - fy;
            if (fy_row >= 0 && fy_row < fh) {
                const uint8_t *src = idx + (size_t)fy_row * fw;
                for (int x = fx; x < fx + fw && x < w; x++) {
                    int i = src[x - fx];
                    if (transparent && i == tindex) continue;
                    memcpy(rgb + (size_t)x * 3, pal[i], 3);
                }
            }
            sink_row(&sink, rgb);
        }
        ok = true;
        goto out;
    }
out:
    sink_end(&sink);
    free(gct);
    free(lct);
    free(idx);
    free(rgb);
    return ok;
}
