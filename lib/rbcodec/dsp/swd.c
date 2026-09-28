/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * SWD DSP wrapper for the Rockbox DSP chain.
 *
 * The processing core (swd21.c) is a fixed-point transcription of the
 * original Windows Mobile driver.  It addresses all of its memory through
 * 32-bit virtual addresses; we replicate the verified device harness
 * layout here so the dumped instance image needs no relocation:
 *
 *   0x1000c000 .. 0x1000dfff   driver data image (coefficients, read-only)
 *   0x30000000 .. 0x30000fff   DSP state block (4 KiB)
 *   0x32000000 .. 0x32000fff   coefficient dest pool (4 KiB)
 *   0x34000000 ..             one interleaved stereo frame buffer
 *
 * Rockbox's internal sample format is int32 with WORD_FRACBITS == 27;
 * the core wants sign-extended int16 input and produces int16 output,
 * so each frame crosses the bridge with >>12 / <<12.
 *
 * Personal-use port: never submit upstream.
 *
 ****************************************************************************/

#include "swd.h"
#include "config.h"
#include <string.h>
#include <stdint.h>
#include "fixedpoint.h"
#include "fracmul.h"
#include "settings.h"
#include "dsp_core.h"
#include "dsp_proc_entry.h"
#include "swd21.h"
#include "swd_memimg.h"
#include "swd_devdata.h"
#include "swd_devdata48.h"

#define SWD_IMG_BASE   0x1000c000u
#define SWD_STATE_BASE 0x30000000u
#define SWD_DEST_BASE  0x32000000u
#define SWD_BUF_BASE   0x34000000u

static uint8_t swd_state[0x1000] __attribute__((aligned(4)));
static uint8_t swd_dest[0x1000]  __attribute__((aligned(4)));
static uint8_t swd_sink[4];          /* catch-all for unmapped addresses */
static int32_t swd_buf[2];           /* interleaved L,R sample buffer    */

static bool swd_enable = false;
static bool swd_ready = false;
static bool swd_rate_ok = false;     /* coefficients exist for 44.1k/48k */
static int32_t swd_cur_rate = 0;     /* rate of the loaded instance     */
static int32_t swd_shift = 12;       /* frac_bits - 15: buffer -> int16 */

/* SWD widening gain table, indexed by (3D level / 10) */
static const int32_t swd_3dtab[11] =
{
    0x7fffffff, 0x782363b2, 0x70ae5365, 0x69a0cf17,
    0x62fad6cc, 0x5cbc6a7f, 0x56e58a33, 0x517635e7,
    0x4c6e6d9b, 0x47ce3151, 0x43958106,
};

/* Input/output gain taps, index = -dB (0 .. 12) */
static const int32_t swd_gain13[13] =
{
    0x40000000, 0x38f5c28f, 0x328f5c29, 0x2d70a3d7,
    0x2851eb85, 0x23d70a3d, 0x20000000, 0x1ccccccd,
    0x1999999a, 0x16b851ec, 0x14395810, 0x120c49ba,
    0x1010624e,
};

static int swd_clamp(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

uint8_t *swd_map(uint32_t addr)
{
    if (addr >= SWD_STATE_BASE &&
        addr < SWD_STATE_BASE + sizeof(swd_state))
        return &swd_state[addr - SWD_STATE_BASE];
    if (addr >= SWD_DEST_BASE &&
        addr < SWD_DEST_BASE + sizeof(swd_dest))
        return &swd_dest[addr - SWD_DEST_BASE];
    if (addr >= SWD_IMG_BASE &&
        addr < SWD_IMG_BASE + sizeof(SWD_IMG0))
        return (uint8_t *)SWD_IMG0 + (addr - SWD_IMG_BASE);
    if (addr >= SWD_BUF_BASE &&
        addr < SWD_BUF_BASE + sizeof(swd_buf))
        return (uint8_t *)swd_buf + (addr - SWD_BUF_BASE);
    return swd_sink;
}

static void wput16(uint32_t addr, uint16_t v)
{
    uint8_t *p = swd_map(addr);
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wput32(uint32_t addr, int32_t v)
{
    uint8_t *p = swd_map(addr);
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* percent (0..100) -> Q31 */
static int32_t swd_pct_q31(long pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return (int32_t)(((int64_t)pct * 0x7fffffff) / 100);
}

/* Push the user settings into the state block.  The two derived fields
 * are mandatory: without them the Bass and widening stages go
 * near-silent in the original algorithm. */
static void swd_apply_settings(void)
{
    if (!swd_ready)
        return;

    long spread = global_settings.swd_3d;
    long ctr = global_settings.swd_center;
    long foc = global_settings.swd_focus;
    long dfn = global_settings.swd_definition;
    long tb  = global_settings.swd_bass;
    int32_t q;

    q = swd_pct_q31(spread);                        /* 3D widening */
    wput32(SWD_STATE_BASE + 0x18, q);
    wput32(SWD_STATE_BASE + 0x154, swd_3dtab[spread / 10]);

    q = swd_pct_q31(tb);                         /* Bass */
    wput32(SWD_STATE_BASE + 0x1c, q);
    wput32(SWD_STATE_BASE + 0x140,
           (int32_t)(0x08000000 + ((int64_t)q * 3) / 16));

    wput32(SWD_STATE_BASE + 0x14c, swd_pct_q31(ctr)); /* Center */
    wput32(SWD_STATE_BASE + 0x0c,  swd_pct_q31(foc)); /* Focus */
    wput32(SWD_STATE_BASE + 0x150, swd_pct_q31(dfn)); /* Definition */
}

/* Build a fresh instance from the dumped default-preset image.  Image
 * choice mirrors the original rate scheduler (doc §7.2): a threshold
 * cascade with no upper bound that caps at the 48k set, so any rate
 * >= 46091 runs the 48k coefficients (corner frequencies shift up
 * accordingly, exactly like the stock driver) and anything below takes
 * the 44.1k set (per-rate images for 8k..32k were never dumped). */
/* Limiter workpoint.  The real device's low-frequency "澎湃" character
 * comes from running the limiter with the polynomial amount
 * state[0x108] cleared (documented workpoint glitch, doc 6.8): the driver
 * only rebuilds 0x108 on a full init, never when 0xfc is re-enabled, so
 * enabling the limiter leaves it at 0 and the spectrum tilts to the low
 * end.  The reference/web player reproduces this by poking
 * 0xfc=1 + 0x108=0; we do the same.  Never rebuild the instance for a
 * limiter change or this character is lost. */
static void swd_write_limiter(int mode)
{
    switch (swd_clamp(mode, 0, 2))
    {
    case 0:                                    /* off */
        wput32(SWD_STATE_BASE + 0xfc, 0);
        wput32(SWD_STATE_BASE + 0x100, 0);
        break;
    case 2:                                    /* on + AGC floor */
        wput32(SWD_STATE_BASE + 0xfc, 1);
        wput32(SWD_STATE_BASE + 0x100, 0x40000000);
        wput32(SWD_STATE_BASE + 0x108, 0);   /* -> 澎湃 workpoint */
        break;
    default:                                   /* on (transparent floor) */
        wput32(SWD_STATE_BASE + 0xfc, 1);
        wput32(SWD_STATE_BASE + 0x100, 0);
        wput32(SWD_STATE_BASE + 0x108, 0);   /* -> 澎湃 workpoint */
        break;
    }
}

static void swd_init(int32_t frequency)
{
    const unsigned char *st_img;
    const unsigned char *de_img;

    if (frequency >= 46091)
    {
        st_img = SWD_DEV_STATE48;
        de_img = SWD_DEV_DEST48;
    }
    else
    {
        st_img = SWD_DEV_STATE;
        de_img = SWD_DEV_DEST;
    }

    memset(swd_state, 0, sizeof(swd_state));
    memcpy(swd_state, st_img, 1024);
    memset(swd_dest, 0, sizeof(swd_dest));
    memcpy(swd_dest, de_img, 1024);

    swd_cur_rate = frequency;

    /* Output mode: headphones (3D/Center active) or speaker. */
    wput32(SWD_STATE_BASE + 0x144, global_settings.swd_mode ? 1 : 0);
    wput32(SWD_STATE_BASE + 0x14,
           global_settings.swd_mode ? 0 : 1);           /* selA */

    wput32(SWD_STATE_BASE + 0x10,                       /* Bass EQ */
           swd_clamp(global_settings.swd_tbeq, 0, 7));

    wput32(SWD_STATE_BASE + 0x110,                      /* input gain */
           swd_gain13[swd_clamp(global_settings.swd_ingain, 0, 12)]);
    wput32(SWD_STATE_BASE + 0x114,                      /* output gain */
           swd_gain13[swd_clamp(global_settings.swd_outgain, 0, 12)]);

    /* Limiter: 0 = off, 1 = on (transparent floor), 2 = on with AGC
     * floor 0x40000000.  Limiter on additionally clears state[0x108]
     * to reproduce the device's low-frequency "澎湃" workpoint. */
    swd_write_limiter(global_settings.swd_limiter);

    wput32(SWD_STATE_BASE + 4, 1);              /* processing on */

    swd21_coef_load(SWD_STATE_BASE);
    swd_apply_settings();
}

void dsp_set_swd_enable(bool enable)
{
    swd_enable = enable;

    struct dsp_config *dsp = dsp_get_config(CODEC_IDX_AUDIO);
    dsp_proc_enable(dsp, DSP_PROC_SWD, enable);

    if (swd_enable && !dsp_proc_active(dsp, DSP_PROC_SWD))
        dsp_proc_activate(dsp, DSP_PROC_SWD, true);
    if (!swd_enable && dsp_proc_active(dsp, DSP_PROC_SWD))
        dsp_proc_activate(dsp, DSP_PROC_SWD, false);
}

void dsp_set_swd_params(long spread, long center, long focus,
                          long definition, long bass)
{
    /* Values are read from global_settings by swd_apply_settings(). */
    (void)spread; (void)center; (void)focus;
    (void)definition; (void)bass;

    swd_apply_settings();
}

void dsp_set_swd_mode(int mode)
{
    if (!swd_ready)
        return;

    wput32(SWD_STATE_BASE + 0x144, mode ? 1 : 0);
    wput32(SWD_STATE_BASE + 0x14, mode ? 0 : 1);   /* selA */
    swd21_coef_load(SWD_STATE_BASE);
}

void dsp_set_swd_tbeq(int index)
{
    if (!swd_ready)
        return;

    wput32(SWD_STATE_BASE + 0x10, swd_clamp(index, 0, 7));
    swd21_coef_load(SWD_STATE_BASE);
}

void dsp_set_swd_gains(int ing, int outg)
{
    if (!swd_ready)
        return;

    wput32(SWD_STATE_BASE + 0x110,
           swd_gain13[swd_clamp(ing, 0, 12)]);
    wput32(SWD_STATE_BASE + 0x114,
           swd_gain13[swd_clamp(outg, 0, 12)]);
}

void dsp_set_swd_limiter(int mode)
{
    if (!swd_ready)
        return;

    /* Bare store exactly like the driver's setter (0x10008edc): never
     * rebuild the instance here, or the 澎湃 workpoint is lost. */
    swd_write_limiter(mode);
}

static void swd_process(struct dsp_proc_entry *this, struct dsp_buffer **buf_p)
{
    struct dsp_buffer *buf = *buf_p;
    int count = buf->remcount;
    int i;

    if (!swd_ready || !swd_rate_ok)
        return;                    /* passthrough: not inited or rate > 48k */

    for (i = 0; i < count; i++)
    {
        int32_t l = buf->p32[0][i];
        int32_t r = buf->p32[1][i];

        l >>= swd_shift;         /* frac_bits -> int16 domain */
        r >>= swd_shift;
        if (l > 0x7fff)     l = 0x7fff;
        else if (l < -0x8000) l = -0x8000;
        if (r > 0x7fff)     r = 0x7fff;
        else if (r < -0x8000) r = -0x8000;

        swd_buf[0] = l;
        swd_buf[1] = r;

        wput16(SWD_STATE_BASE, 1);           /* frame count = 1 */
        swd21_process(SWD_STATE_BASE, SWD_BUF_BASE);

        buf->p32[0][i] = swd_buf[0] << swd_shift;   /* int16 -> frac_bits */
        buf->p32[1][i] = swd_buf[1] << swd_shift;
    }

    (void)this;
}

static intptr_t swd_configure(struct dsp_proc_entry *this,
                                struct dsp_config *dsp,
                                unsigned int setting, intptr_t value)
{
    switch (setting)
    {
    case DSP_PROC_INIT:
        if (value == 0)
        {
            /* Coming online: value == 0 means the stage was just enabled
             * (Rockbox convention, cf. afr.c/pga.c/crossfeed.c).  Must set
             * ready *before* init so the init also applies user settings.
             * A rate already chosen by NEW_FORMAT is kept; NEW_FORMAT swaps
             * in the per-rate image for every track anyway. */
            swd_ready = true;
            swd_init(swd_cur_rate ? swd_cur_rate : 44100);
        }
        this->process = swd_process;
        break;
    case DSP_PROC_NEW_FORMAT:
    {
        const struct sample_format *fmt = (const struct sample_format *)value;
        int32_t fb = fmt->frac_bits;
        int32_t f = fmt->frequency;
        swd_rate_ok = true;          /* every rate maps to a set (§7.2) */
        if (fb < 16) fb = 16;          /* keep the int16 bridge sane */
        if (fb > 31) fb = 31;
        swd_shift = fb - 15;
        if (f != swd_cur_rate)
            swd_init(f);              /* swap the per-rate instance */
        break;
    }
    case DSP_FLUSH:
        swd_init(swd_cur_rate);     /* reset filters between tracks */
        break;
    case DSP_PROC_CLOSE:
        swd_ready = false;
        break;
    }

    return 1;
    (void)dsp;
}

/* Database entry */
DSP_PROC_DB_ENTRY(
    SWD,
    swd_configure);
