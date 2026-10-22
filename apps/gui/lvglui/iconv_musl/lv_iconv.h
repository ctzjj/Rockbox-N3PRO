/* minimal iconv API surface for the vendored musl iconv
 * (apps/gui/lvglui/iconv_musl/iconv.c, MIT license, musl libc project) */
#ifndef LV_ICONV_H
#define LV_ICONV_H

#include <stddef.h>

typedef void *lv_iconv_t;

lv_iconv_t lv_iconv_open(const char *to, const char *from);
size_t lv_iconv(lv_iconv_t cd, char **__restrict in, size_t *__restrict inb,
                char **__restrict out, size_t *__restrict outb);
int lv_iconv_close(lv_iconv_t cd);

#endif
