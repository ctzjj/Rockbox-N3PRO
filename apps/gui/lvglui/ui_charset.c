/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   ____  _______  ___
 *   Source      |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \ \/\  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware |____|   /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/    \/    \/            \/
 *
 * Charset helper for the LVGL UI, built on the vendored musl iconv
 * (iconv_musl/, MIT).  Music tags on this device arrive as raw legacy
 * bytes (usually GBK; sometimes Big5 or Latin-1), while every LVGL text
 * is decoded as UTF-8.  shell_cs() detects the encoding per string:
 *   1. valid UTF-8 passes through untouched,
 *   2. GBK / BIG5 / GB18030 are tried in strict mode; when several
 *      decode cleanly the winner is picked by common-hanzi frequency,
 *   3. anything else falls back to ISO-8859-1 via iconv.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "config.h"
#ifdef HAVE_LVGL_UI

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include "lvgl.h"
#include "lvgl_ui.h"
#include "string-extra.h"
#include "iconv_musl/lv_iconv.h"

/* convert with musl iconv; strict mode fails on any illegal sequence,
 * lossy mode replaces bad bytes with '?' */
static bool cs_conv(const char *from, const char *src, size_t slen,
                    char *dst, size_t dstsz, bool lossy)
{
    lv_iconv_t cd = lv_iconv_open("UTF-8", from);
    if (!cd || cd == (void *)-1)
        return false;
    char *ip = (char *)src;
    size_t ib = slen;
    char *op = dst;
    size_t ob = dstsz - 1;
    bool ok = true;
    while (ib)
    {
        size_t r = lv_iconv(cd, &ip, &ib, &op, &ob);
        if (r != (size_t)-1)
            break;                     /* input fully consumed */
        if (errno == E2BIG)
            break;                     /* dst full: truncate, still valid */
        if (!lossy)
        {
            ok = false;                /* EILSEQ / EINVAL: wrong charset */
            break;
        }
        ip++;                          /* skip the offending byte */
        ib--;
        if (ob)
        {
            *op++ = '?';
            ob--;
        }
    }
    *op = '\0';
    lv_iconv_close(cd);
    return ok;
}

/* is the whole string valid UTF-8? */
static bool valid_utf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p)
    {
        if (*p < 0x80)
        {
            p++;
            continue;
        }
        int n;
        unsigned int cp;
        if ((*p & 0xE0) == 0xC0)      { n = 1; cp = *p & 0x1F; }
        else if ((*p & 0xF0) == 0xE0) { n = 2; cp = *p & 0x0F; }
        else if ((*p & 0xF8) == 0xF0) { n = 3; cp = *p & 0x07; }
        else return false;
        p++;
        while (n--)
        {
            if ((*p & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (*p & 0x3F);
            p++;
        }
        if (cp > 0x10FFFF)
            return false;
    }
    return true;
}

/* frequent hanzi (written Chinese corpus): the correctly decoded variant
 * of a GBK/Big5 string contains many of these, a mis-decode produces
 * rare characters instead */
static const unsigned short cs_common[] =
{
    0x4e00, 0x4e0d, 0x4e86, 0x4e2d, 0x4eba, 0x4ed6, 0x4ee5, 0x4f1a,
    0x4f46, 0x4f60, 0x5019, 0x50cf, 0x5167, 0x518d, 0x51fa, 0x5200,
    0x5230, 0x529b, 0x52a0, 0x5316, 0x5341, 0x53ef, 0x5408, 0x540c,
    0x548c, 0x54c1, 0x56de, 0x56e0, 0x5728, 0x5b50, 0x5bb6, 0x5fc3,
    0x6210, 0x624b, 0x6253, 0x6307, 0x6392, 0x6587, 0x65b9, 0x65e5,
    0x660e, 0x661f, 0x662f, 0x66f4, 0x672c, 0x6b63, 0x6c11, 0x70b9,
    0x7269, 0x7406, 0x751f, 0x7528, 0x767d, 0x7814, 0x793e, 0x7a7a,
    0x7acb, 0x8005, 0x80fd, 0x81ea, 0x8868, 0x897f, 0x8981, 0x89c6,
    0x8aaa, 0x8bf4, 0x8eab, 0x901f, 0x90e8, 0x91cf, 0x9762, 0x97f3,
    0x9891, 0x9a8c, 0x9b54, 0x9ea6, 0x4e1d, 0x4e34, 0x4e45, 0x4ee3,
    0x4f1a, 0x4f7f, 0x505a, 0x5149, 0x5165, 0x5176, 0x51b5, 0x527d,
    0x52d5, 0x5355, 0x53d6, 0x53e3, 0x540d, 0x547d, 0x559c, 0x5668,
    0x56db, 0x578b, 0x57fa, 0x5b8c, 0x5bb3, 0x5c0f, 0x5c45, 0x5c55,
    0x5dee, 0x5df2, 0x5e38, 0x5e73, 0x5ea6, 0x5f0f, 0x5f97, 0x60c5,
    0x610f, 0x611f, 0x6167, 0x61c9, 0x6230, 0x628a, 0x64cd, 0x6536,
    0x6574, 0x65b9, 0x679c, 0x683c, 0x6837, 0x6e90, 0x6f02, 0x70ba,
    0x7121, 0x7236, 0x7269, 0x73fe, 0x7576, 0x76ee, 0x76f4, 0x771f,
    0x77e5, 0x78ba, 0x7968, 0x7a0b, 0x7a2e, 0x7ba1, 0x7cfb, 0x7d71,
    0x7dca, 0x7dda, 0x7f6e, 0x81ea, 0x8209, 0x82e5, 0x8981, 0x8a2a,
    0x8a71, 0x8aaa, 0x8abf, 0x8b58, 0x8ca8, 0x8cc7, 0x8def, 0x8eab,
    0x9032, 0x91cc, 0x9451, 0x9805, 0x984c, 0x99ac, 0x9ad4, 0x9ce5,
    0x90ed, 0x91c7, 0x6f54, 0x6d01, 0x6c6a, 0x82cf, 0x8607, 0x4eea,
    0x5100, 0x739f, 0x5c90, 0x5723, 0x8056, 0x6770, 0x5091, 0x82f1,
    0x62f3, 0x5988, 0x5abd, 0x4f70, 0x60ac, 0x61f8, 0x59b9, 0x60e0,
    0x56e2, 0x5718, 0x513f, 0x5152, 0x98de, 0x98db, 0x8c2d, 0x8b5a,
    0x7ef4, 0x7dad, 0x97e6, 0x97cb, 0x963f, 0x6851, 0x97e9, 0x97d3,
    0x7ea2, 0x7d05, 0x6625, 0x79cb, 0x51ac, 0x590f, 0x96e8, 0x96ea,
    0x98ce, 0x98a8, 0x4e91, 0x96f2, 0x6c5f, 0x6cb3, 0x6d77, 0x5c71,
    0x6797, 0x68ee, 0x77f3, 0x7530, 0x6751, 0x57ce, 0x5dde, 0x4eac,
    0x65b0, 0x8001, 0x4e50, 0x6a02, 0x58f0, 0x8072, 0x5f71, 0x5e74,
    0x534e, 0x83ef, 0x5f20, 0x5f35, 0x9648, 0x9673, 0x9ec4, 0x9ec3,
    0x5218, 0x5289, 0x738b, 0x674e, 0x5c39, 0x5f90, 0x9ad8, 0x6797,
    0x8d75, 0x8da8, 0x6768, 0x6768, 0x5468, 0x5434, 0x90d1, 0x9127
};

/* count code points that belong to frequent hanzi; used to tell the
 * correct Chinese decode apart from a wrong one (matched+2) */
static int score_hanzi(const char *utf8)
{
    const unsigned char *p = (const unsigned char *)utf8;
    int score = 0;
    while (*p)
    {
        unsigned int cp;
        if (*p < 0x80)
        {
            p++;
            continue;
        }
        int n;
        if ((*p & 0xE0) == 0xC0)      { n = 1; cp = *p & 0x1F; }
        else if ((*p & 0xF0) == 0xE0) { n = 2; cp = *p & 0x0F; }
        else                          { n = 3; cp = *p & 0x07; }
        p++;
        while (n-- && (*p & 0xC0) == 0x80)
        {
            cp = (cp << 6) | (*p & 0x3F);
            p++;
        }
        if (cp <= 0xFFFF)
            for (unsigned i = 0; i < sizeof cs_common / sizeof cs_common[0];
                 i++)
                if (cs_common[i] == (unsigned short)cp)
                {
                    score += 2;
                    break;
                }
    }
    return score;
}

/* count kana / hangul code points; only used once Chinese decoding has
 * failed, so that a genuine Korean/Japanese tag is still recognised */
static int score_kana(const char *utf8)
{
    const unsigned char *p = (const unsigned char *)utf8;
    int score = 0;
    while (*p)
    {
        unsigned int cp;
        if (*p < 0x80) { p++; continue; }
        int n;
        if ((*p & 0xE0) == 0xC0)      { n = 1; cp = *p & 0x1F; }
        else if ((*p & 0xF0) == 0xE0) { n = 2; cp = *p & 0x0F; }
        else                          { n = 3; cp = *p & 0x07; }
        p++;
        while (n-- && (*p & 0xC0) == 0x80)
        {
            cp = (cp << 6) | (*p & 0x3F);
            p++;
        }
        if ((cp >= 0x3040 && cp <= 0x30FF) ||   /* kana */
            (cp >= 0x31F0 && cp <= 0x31FF) ||
            (cp >= 0xAC00 && cp <= 0xD7A3))     /* hangul */
            score++;
    }
    return score;
}

/* Chinese candidates come first: a GBK/Big5 library is by far the most
 * likely source, and picking a Korean/Japanese decode of Chinese bytes
 * would yield plausible-looking but wrong hangul/kana. */
static const char *const cs_cn[] = { "GB18030", "BIG5" };
#define NCS_CN (sizeof cs_cn / sizeof cs_cn[0])
static const char *const cs_other[] = { "EUC-KR", "SHIFT_JIS", "EUC-JP" };
#define NCS_OTHER (sizeof cs_other / sizeof cs_other[0])
#define CS_BUF 512

/* Auto-detect the source encoding and convert to UTF-8:
 *   1. a string that already is valid UTF-8 (incl. ASCII) is copied,
 *   2. GB18030 (superset of GBK/GB2312) and BIG5 are tried in strict
 *      mode; whichever decodes and yields the most frequent hanzi wins
 *      (GB18030 on a tie). This is the Chinese path.
 *   3. only if no Chinese decode produces recognisable hanzi are the
 *      Korean/Japanese encodings tried (a genuine non-Chinese tag),
 *   4. finally Latin-1, then the raw bytes as a last resort. */
const char *shell_cs(char *dst, size_t dstsz, const char *src)
{
    if (!dstsz)
        return dst;
    if (!src || !src[0])
    {
        dst[0] = '\0';
        return dst;
    }
    if (valid_utf8(src))
    {
        strlcpy(dst, src, dstsz);
        return dst;                    /* already UTF-8 (or plain ASCII) */
    }

    size_t n = strlen(src);
    if (n >= CS_BUF)
        n = CS_BUF - 1;

    char bufs[NCS_CN][CS_BUF];
    int best = -1, bestscore = -1;
    for (unsigned i = 0; i < NCS_CN; i++)
    {
        if (!cs_conv(cs_cn[i], src, n, bufs[i], CS_BUF, false))
            continue;                  /* illegal sequence: not this one */
        int sc = score_hanzi(bufs[i]);
        if (sc > bestscore)
        {
            bestscore = sc;            /* earlier candidate wins ties */
            best = (int)i;
        }
    }
    if (best >= 0)
    {
        /* GB18030 is a superset of most Chinese encodings and this is a
         * Chinese library: accept it whenever it decodes, even if none of
         * the decoded characters happen to be in the frequency table. */
        strlcpy(dst, bufs[best], dstsz);
        return dst;
    }

    /* no recognisable hanzi: maybe a genuine Korean/Japanese tag */
    char obuf[NCS_OTHER][CS_BUF];
    best = -1;
    bestscore = 0;
    for (unsigned i = 0; i < NCS_OTHER; i++)
    {
        if (!cs_conv(cs_other[i], src, n, obuf[i], CS_BUF, false))
            continue;
        int sc = score_kana(obuf[i]);
        if (sc > bestscore)
        {
            bestscore = sc;
            best = (int)i;
        }
    }
    if (best >= 0)
    {
        strlcpy(dst, obuf[best], dstsz);
        return dst;
    }

    /* nothing decoded cleanly: Western single-byte tags */
    if (cs_conv("ISO-8859-1", src, n, dst, dstsz, true))
        return dst;

    strlcpy(dst, src, dstsz);          /* give up: raw bytes */
    return dst;
}

#endif /* HAVE_LVGL_UI */
