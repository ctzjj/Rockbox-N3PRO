/* SWD memory space -> host pointer, inlined for the hot DSP core.
 * The transcription addresses everything through 32-bit virtual
 * addresses; a plain call per access was the main cost on slow targets. */
#ifndef SWD_MAP_H
#define SWD_MAP_H

#include <stdint.h>

#define SWD_IMG_BASE   0x1000c000u
#define SWD_STATE_BASE 0x30000000u
#define SWD_DEST_BASE  0x32000000u
#define SWD_BUF_BASE   0x34000000u

#define SWD_STATE_SIZE 0x1000
#define SWD_DEST_SIZE  0x1000
#define SWD_IMG_SIZE   8192

/* frames handed to the core in one call */
#define SWD_BLK        64
#define SWD_BUF_SIZE   (SWD_BLK * 2 * 4)

extern uint8_t swd_state_mem[SWD_STATE_SIZE];
extern uint8_t swd_dest_mem[SWD_DEST_SIZE];
extern uint8_t swd_sink_mem[4];
extern uint8_t swd_buf_mem[SWD_BUF_SIZE];
extern const uint8_t *swd_img0;

static inline __attribute__((always_inline)) uint8_t *swd_map_fast(uint32_t a)
{
    uint32_t d;

    d = a - SWD_STATE_BASE;
    if (d < SWD_STATE_SIZE)
        return swd_state_mem + d;
    d = a - SWD_DEST_BASE;
    if (d < SWD_DEST_SIZE)
        return swd_dest_mem + d;
    d = a - SWD_IMG_BASE;
    if (d < SWD_IMG_SIZE)
        return (uint8_t *)swd_img0 + d;
    d = a - SWD_BUF_BASE;
    if (d < SWD_BUF_SIZE)
        return swd_buf_mem + d;
    return swd_sink_mem;
}

#endif
