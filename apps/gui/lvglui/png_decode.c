/* LVGL bundled lodepng + PNG decoder, compiled into the app for album art. */
#include "../../../lib/lvgl/src/extra/libs/png/lodepng.c"
#include "../../../lib/lvgl/src/extra/libs/png/lv_png.c"

#include <stdlib.h>

/* decode a PNG blob into a malloc'd RGBA buffer; returns NULL on error and
 * stores the intrinsic size in *w / *h.  Lives here so ui_backend.c can stay
 * free of LVGL-tree headers (see the backend boundary in docs/lvgl-ui.md). */
unsigned char *ui_png_decode_rgba(const unsigned char *data, size_t len,
                                  int *w, int *h)
{
    unsigned pw = 0, ph = 0;
    unsigned char *rgba = NULL;
    if (lodepng_decode32(&rgba, &pw, &ph, data, len) != 0 || !rgba ||
        !pw || !ph)
    {
        free(rgba);
        return NULL;
    }
    *w = (int)pw;
    *h = (int)ph;
    return rgba;
}
