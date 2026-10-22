/* Second instantiation of the core JPEG decoder, built with
 * JPEG_FROM_MEM so DLNA album art can be decoded straight from RAM.
 */

#include "config.h"

#define JPEG_FROM_MEM
#define JPEG_DECODE_OVERHEAD dlna_jpeg_overhead_local

#include "../recorder/jpeg_load.c"
