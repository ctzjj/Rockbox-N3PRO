/***************************************************************************
 *             __________               __   ___.
 *
 * UI theme: every colour / opacity / radius / font / shadow / spacing
 * used by the LVGL UI lives here so the whole look can be swapped by
 * providing another ui_theme_fill() implementation.  Screens (ui_*.c)
 * must never hard-code a style value -- reference th-> only.
 *
 * 语义命名约定:
 *   bg_*   page / surface backgrounds     fg_*   text colours
 *   *_dim  secondary / muted              acc*   accent highlights
 *   sh_*   shadow stack (3 dark + 3 halo) bd_*   wallpaper
 **************************************************************************/
#ifndef UI_THEME_H
#define UI_THEME_H

#include "lvgl.h"

#define TH_BLOBS 5              /* wallpaper glow blobs */

typedef struct
{
    /* ---- text ------------------------------------------------------- */
    lv_color_t fg_text;        /* primary text            (0x222b3d) */
    lv_color_t fg_dim;         /* secondary text          (0x6b7690) */
    lv_color_t fg_link;        /* links / actions         (0x4a6cf7) */
    lv_color_t fg_ink;         /* dark text on light keys (0x2a3358) */
    lv_color_t fg_inverse;     /* light text on dark cards(0xe6ecf7) */
    lv_color_t fg_placeholder; /* art fallback glyph      (0xffe9d6) */

    /* ---- accents ---------------------------------------------------- */
    lv_color_t accent;         /* warm highlight          (0xffb84d) */
    lv_color_t accent2;        /* pink                    (0xff6a88) */
    lv_color_t accent_blue;    /* primary buttons         (0x4a6cf7) */

    /* ---- surfaces --------------------------------------------------- */
    lv_color_t glass_bg;       /* light glass card        (0xf4f9ff) */
    lv_color_t menu_bg;        /* overlay card surface    (0xf4f9ff) */
    lv_color_t scrim;          /* full-screen dimmer      (0xdfe9f5) */
    lv_color_t scrim_input;    /* input dialog dimmer     (0xe4edf7) */
    lv_color_t btn_secondary;  /* white buttons           (0xffffff) */
    lv_color_t bar_track;      /* slider/bar groove       (0xffffff) */

    /* ---- player page ------------------------------------------------ */
    lv_color_t art_fallback;   /* square-cover placeholder(0x2b1a44) */
    lv_color_t art_sun_in;     /* placeholder inner ring  (0xffe08a) */
    lv_color_t art_sun_mid;    /* placeholder mid ring    (0xff9e6b) */
    lv_color_t art_sun_out;    /* placeholder outer ring  (0xc9618b) */
    lv_color_t art_tt_text;    /* album name on banner    (0xffe9d6) */
    lv_color_t progress;       /* seek indicator          (0xffd97d) */
    lv_color_t play_btn_bg;    /* big play button         (0xff9e6b) */
    lv_color_t play_btn_fg;    /* big play button glyph   (0x2a1020) */
    lv_color_t lrc_active;     /* current lyric line      (0xffffff) */
    lv_color_t lrc_dim;        /* other lyric lines       (0xe6ecf7) */

    /* ---- misc -------------------------------------------------------- */
    lv_color_t border_white;   /* hairlines               (0xffffff) */
    lv_color_t row_icon_muted; /* file-browser misc icon  (0xb8c4dd) */

    /* ---- wallpaper --------------------------------------------------- */
    lv_color_t bd_grad_top;    /* backdrop gradient top   (0x6b7edc) */
    lv_color_t bd_grad_bot;    /* backdrop gradient bottom(0x4e54c4) */
    struct { lv_color_t col; int x, y, r; int str; } bd_blob[TH_BLOBS];

    /* ---- shadow stack (soft_shadow) ----------------------------------- */
    lv_color_t sh_dark;             /* ground shadow colour    (0x3a4356) */
    uint8_t    sh_dark_opa[3];      /* outer -> inner          6/9/13     */
    lv_coord_t sh_spread[3];        /*                         8/5/3      */
    lv_coord_t sh_dy;               /* downward offset         3          */
    uint8_t    sh_halo_opa[3];      /* bright rim, outer->inner 26/36/50 */
    lv_coord_t sh_halo_spread[3];   /*                         6/4/3      */

    /* ---- opacities ---------------------------------------------------- */
    uint8_t card_opa;          /* glass card bg           (95)  */
    uint8_t card_border_opa;   /* glass card border       (220) */
    uint8_t menu_card_opa;     /* overlay card bg         (150) */
    uint8_t menu_border_opa;   /* overlay card border     (200) */
    uint8_t menu_row_opa;      /* overlay row bg          (70)  */
    uint8_t scrim_opa;         /* page dimmer             (110) */
    uint8_t scrim_input_opa;   /* input dialog dimmer     (130) */
    uint8_t btn_secondary_opa; /* secondary buttons       (90)  */
    uint8_t dock_opa;          /* home mini-player dock   (110) */
    uint8_t chip_opa;          /* drawer chips            (70)  */
    uint8_t chip_border_opa;   /* drawer chip border      (120) */
    uint8_t bar_track_opa;     /* slider groove           (50)  */
    uint8_t kbd_card_opa;      /* input card              (200) */

    /* ---- geometry ----------------------------------------------------- */
    lv_coord_t radius;         /* generic card corner     (20)  */
    lv_coord_t radius_big;     /* overlay cards           (28)  */
    lv_coord_t menu_row_h;     /* context menu row        (40)  */
    lv_coord_t menu_gap;       /* context row spacing     (4)   */
    lv_coord_t menu_card_w;    /* context card width      (320)  */
    lv_coord_t row_h;          /* library list row        (48)   */
    lv_coord_t card_row_h;     /* inner row height        (40)   */
    lv_coord_t pad_screen;     /* page side margin        (12)   */
    lv_coord_t gap_row;        /* generic vertical gap    (8)    */

    /* ---- fonts --------------------------------------------------------- */
    const lv_font_t *font_cjk;   /* body */
    const lv_font_t *font_cn;    /* full GB2312 (input + candidates) */
    const lv_font_t *font_title; /* large title */    /* notosans_sc_16       */
    const lv_font_t *font_symbol; /* montserrat_16        */
    const lv_font_t *font_mid;    /* montserrat_28        */
    const lv_font_t *font_big;    /* montserrat_48        */
} ui_theme_t;

extern const ui_theme_t *const th;

void ui_theme_init(void);   /* fill the theme before any UI is built */

#endif /* UI_THEME_H */
