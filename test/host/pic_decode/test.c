#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pic_decode.h"
#include "esp_heap_caps.h"
static int sh;
static int scale(int w, int h, const void *a) { (void)w; (void)h; (void)a; return sh; }
int main(int argc, char **argv)
{
    sh = atoi(argv[2]);
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    pic_img_t img = {0};
    char err[64] = "";
    const char *dot = strrchr(argv[1], '.');
    bool ok = !strcmp(dot, ".png") ? pic_load_png(f, &img, scale, NULL, err)
                                   : pic_load_gif(f, &img, scale, NULL, err);
    fclose(f);
    if (!ok) { printf("ERR %s\n", err); free(img.px); return 1; }
    if (argc > 3) {
        FILE *o = fopen(argv[3], "wb");
        fprintf(o, "P6 %d %d 255\n", img.w, img.h);
        for (int i = 0; i < img.w * img.h; i++) {
            uint16_t p = img.px[i];
            uint8_t c[3] = { (p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3 };
            fwrite(c, 1, 3, o);
        }
        fclose(o);
    }
    printf("OK %dx%d\n", img.w, img.h);
    free(img.px);
    return 0;
}
