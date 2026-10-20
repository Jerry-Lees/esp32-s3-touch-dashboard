/* Feeds stdin through json_skip in random-sized chunks; prints the result.
 * Usage: test SEED NAME... < in.json > out.json */
#include <stdio.h>
#include <stdlib.h>

#include "json_skip.h"

int main(int argc, char **argv)
{
    (void)argc;
    static char in[4 << 20], out[4 << 20];
    size_t n = fread(in, 1, sizeof(in), stdin), len = 0;
    srand(atoi(argv[1]));
    json_skip_t js;
    json_skip_init(&js, (const char *const *)argv + 2);   /* argv is NULL-terminated */
    for (size_t i = 0; i < n;) {
        size_t k = 1 + rand() % 700;
        if (k > n - i) k = n - i;
        json_skip_feed(&js, in + i, k, out, &len, sizeof(out));
        i += k;
    }
    if (js.overflow) return 2;
    fwrite(out, 1, len, stdout);
    return 0;
}
