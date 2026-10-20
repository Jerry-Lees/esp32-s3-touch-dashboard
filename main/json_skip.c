/* Streaming JSON member filter; see json_skip.h. */
#include <string.h>

#include "json_skip.h"

void json_skip_init(json_skip_t *js, const char *const *names)
{
    memset(js, 0, sizeof(*js));
    js->names = names;
}

static void put(json_skip_t *js, char c, char *out, size_t *len, size_t cap)
{
    if (*len + 1 < cap) {
        out[(*len)++] = c;
        out[*len] = '\0';
    } else {
        js->overflow = true;
    }
}

static bool dropped(const json_skip_t *js)
{
    if (js->name_len >= sizeof(js->name)) return false;     /* longer than any we drop */
    for (const char *const *n = js->names; *n; n++) {
        if (strlen(*n) == js->name_len && !memcmp(*n, js->name, js->name_len)) return true;
    }
    return false;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void json_skip_feed(json_skip_t *js, const char *in, size_t n, char *out, size_t *len,
                    size_t cap)
{
    for (size_t i = 0; i < n; i++) {
        char c = in[i];

        if (js->skip == 1) {                     /* waiting for the value to start */
            if (is_space(c)) continue;
            js->skip = 2;
            js->nest = 0;
            js->skip_str = false;
            js->esc = false;
            if (c == '{' || c == '[') {
                js->nest = 1;
                continue;
            }
            if (c == '"') {
                js->skip_str = true;
                continue;
            }
            /* a number, true, false or null: skipped until , } or ] */
        }
        if (js->skip == 2) {
            if (js->skip_str || (js->nest > 0 && js->in_str)) {
                /* inside a string within the dropped value */
                if (js->esc) js->esc = false;
                else if (c == '\\') js->esc = true;
                else if (c == '"') {
                    if (js->skip_str) {
                        js->skip_str = false;
                        js->skip = 0;
                    } else {
                        js->in_str = false;
                    }
                }
                continue;
            }
            if (js->nest > 0) {
                if (c == '"') js->in_str = true;
                else if (c == '{' || c == '[') js->nest++;
                else if (c == '}' || c == ']') {
                    if (--js->nest == 0) js->skip = 0;
                }
                continue;
            }
            if (c != ',' && c != '}' && c != ']') continue;   /* rest of a scalar */
            js->skip = 0;                        /* and copy this character below */
        }

        put(js, c, out, len, cap);
        if (js->in_str) {
            if (js->esc) {
                js->esc = false;
            } else if (c == '\\') {
                js->esc = true;
            } else if (c == '"') {
                js->in_str = false;
                js->after_str = true;
                continue;
            }
            if (js->name_len < sizeof(js->name)) js->name[js->name_len] = c;
            js->name_len++;
            continue;
        }
        if (c == '"') {
            js->in_str = true;
            js->name_len = 0;
        } else if (c == ':' && js->after_str && dropped(js)) {
            put(js, 'n', out, len, cap);
            put(js, 'u', out, len, cap);
            put(js, 'l', out, len, cap);
            put(js, 'l', out, len, cap);
            js->skip = 1;
        }
        if (!is_space(c)) js->after_str = false;
    }
}
