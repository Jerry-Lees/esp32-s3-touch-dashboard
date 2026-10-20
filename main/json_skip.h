#pragma once
/*
 * Streaming JSON filter: copies JSON through, but replaces the value of any
 * member whose name is in a list with null, so large parts we don't need
 * (e.g. Jellyfin's 800 KB play queue) never have to fit in memory. Works
 * on any chunk sizes. Plain C, no ESP code (tested on the PC:
 * test/host/json_skip).
 */
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *const *names;   /* NULL-terminated list of member names to drop */
    bool   in_str, esc;         /* inside a string / after a backslash */
    bool   after_str;           /* a string just ended (maybe a member name) */
    char   name[40];            /* the last string's start */
    size_t name_len;            /* its full length */
    int    skip;                /* 0 = copying; 1 = before the dropped value; 2 = in it */
    int    nest;                /* {} [] depth inside a dropped value */
    bool   skip_str;            /* the dropped value is a string */
    bool   overflow;            /* out was too small */
} json_skip_t;

void json_skip_init(json_skip_t *js, const char *const *names);

/* Filters n bytes of in and appends the result to out, which holds *len
 * bytes of cap; keeps out NUL-terminated (cap includes the NUL). */
void json_skip_feed(json_skip_t *js, const char *in, size_t n, char *out, size_t *len,
                    size_t cap);
