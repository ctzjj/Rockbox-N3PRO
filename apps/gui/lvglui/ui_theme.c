/***************************************************************************
 *             __________               __   ___.
 *
 * Default theme values. Swapping the look = replacing this file (or the
 * ui_theme_fill() body) -- nothing else in the UI layer changes.
 *
 **************************************************************************/
#include "config.h"
#include "lvgl.h"
#include "ui_theme.h"
#include "settings.h"

LV_FONT_DECLARE(notosans_sc_14);
LV_FONT_DECLARE(notosans_sc_16);
LV_FONT_DECLARE(notosans_sc_20);
LV_FONT_DECLARE(notosans_sc_cn16);

static ui_theme_t theme;

const ui_theme_t *const th = &theme;

/* iOS-26 "liquid glass":
 *   - light frosted panels, bright white rims, big continuous corners
 *   - bright-fog scrims (never black)
 *   - shadows: few, huge, almost invisible dark + a strong top halo
 *   - vivid wallpaper so the glass has colour to refract
 * (SW renderer: no live blur, glass is simulated with transparency). */
__attribute__((weak)) void ui_theme_fill(ui_theme_t *t)
{
    if (global_settings.ui_dark_mode)
    {
        /* dark variant: charcoal glass, vivid accents, dimmer wallpaper */
        t->fg_text        = lv_color_hex(0xe8eef8);
        t->fg_dim         = lv_color_hex(0x8a93a8);
        t->fg_link        = lv_color_hex(0x4a9dff);
        t->fg_ink         = lv_color_hex(0x10151d);
        t->fg_inverse     = lv_color_hex(0xf5f9ff);
        t->fg_placeholder = lv_color_hex(0xffe9d6);

        t->accent      = lv_color_hex(0xffb84d);
        t->accent2     = lv_color_hex(0xff6a88);
        t->accent_blue = lv_color_hex(0x4a9dff);

        t->glass_bg      = lv_color_hex(0x1a2130);
        t->menu_bg       = lv_color_hex(0x1a2130);
        t->scrim         = lv_color_hex(0x05070b);
        t->scrim_input   = lv_color_hex(0x070a10);
        t->btn_secondary = lv_color_hex(0x2a3344);
        t->bar_track     = lv_color_hex(0x3a4356);

        t->art_fallback = lv_color_hex(0x2b1a44);
        t->art_sun_in   = lv_color_hex(0xffe08a);
        t->art_sun_mid  = lv_color_hex(0xff9e6b);
        t->art_sun_out  = lv_color_hex(0xc9618b);
        t->art_tt_text  = lv_color_hex(0xffe9d6);
        t->progress     = lv_color_hex(0xffd97d);
        t->play_btn_bg  = lv_color_hex(0xff9e6b);
        t->play_btn_fg  = lv_color_hex(0x2a1020);
        t->lrc_active   = lv_color_hex(0xffffff);
        t->lrc_dim      = lv_color_hex(0x9aa4b8);

        t->border_white   = lv_color_hex(0xffffff);
        t->row_icon_muted = lv_color_hex(0x5f6b85);

        /* dark variant: plain black backdrop, glass panes supply the depth */
        t->bd_grad_top = lv_color_hex(0x0b0b0d);
        t->bd_grad_bot = lv_color_hex(0x000000);
        t->bd_blob[0].col = lv_color_hex(0xff4fa3);
        t->bd_blob[0].x = 310; t->bd_blob[0].y = 30;
        t->bd_blob[0].r = 220; t->bd_blob[0].str = 30;
        t->bd_blob[1].col = lv_color_hex(0x2f8dff);
        t->bd_blob[1].x = 25;  t->bd_blob[1].y = 440;
        t->bd_blob[1].r = 230; t->bd_blob[1].str = 30;
        t->bd_blob[2].col = lv_color_hex(0xffa04d);
        t->bd_blob[2].x = 330; t->bd_blob[2].y = 430;
        t->bd_blob[2].r = 180; t->bd_blob[2].str = 24;
        t->bd_blob[3].col = lv_color_hex(0x9a5cff);
        t->bd_blob[3].x = 55;  t->bd_blob[3].y = 50;
        t->bd_blob[3].r = 190; t->bd_blob[3].str = 24;
        t->bd_blob[4].col = lv_color_hex(0x39d6c8);
        t->bd_blob[4].x = 180; t->bd_blob[4].y = 240;
        t->bd_blob[4].r = 250; t->bd_blob[4].str = 16;

        /* no colour glow on the black wallpaper */
        for (unsigned i = 0; i < TH_BLOBS; i++)
            t->bd_blob[i].r = 0;

        t->sh_dark    = lv_color_hex(0x000000);
        t->sh_dark_opa[0]  = 8;   t->sh_dark_opa[1]  = 12; t->sh_dark_opa[2]  = 18;
        t->sh_spread[0]    = 8;   t->sh_spread[1]    = 5;  t->sh_spread[2]    = 3;
        t->sh_dy           = 3;
        t->sh_halo_opa[0]  = 6;   t->sh_halo_opa[1]  = 9;  t->sh_halo_opa[2]  = 14;
        t->sh_halo_spread[0] = 6; t->sh_halo_spread[1] = 4; t->sh_halo_spread[2] = 3;

        t->card_opa          = 56;      /* white @ 22%: still mostly clear */
        t->card_border_opa   = 110;     /* 1px rgba(255,255,255,.43) */
        t->glass_shadow_opa  = 70;
        t->menu_card_opa     = LV_OPA_COVER;
        t->menu_border_opa   = 110;
        t->menu_row_opa      = 56;
        t->scrim_opa         = 185;
        t->scrim_input_opa   = 190;
        t->btn_secondary_opa = 220;
        t->dock_opa          = 170;
        t->chip_opa          = 130;
        t->chip_border_opa   = 80;
        t->bar_track_opa     = 80;
        t->kbd_card_opa      = 230;
    }
    else
    {
        /* iOS-26 "liquid glass":
         *   - light frosted panels, bright white rims, big continuous corners
         *   - bright-fog scrims (never black)
         *   - shadows: few, huge, almost invisible dark + a strong top halo
         *   - vivid wallpaper so the glass has colour to refract
         * (SW renderer: no live blur, glass is simulated with transparency). */
        t->fg_text        = lv_color_hex(0x1c2430);
        t->fg_dim         = lv_color_hex(0x5f6b85);
        t->fg_link        = lv_color_hex(0x0a84ff);   /* iOS blue */
        t->fg_ink         = lv_color_hex(0x2a3358);
        t->fg_inverse     = lv_color_hex(0xf5f9ff);
        t->fg_placeholder = lv_color_hex(0xffe9d6);

        t->accent      = lv_color_hex(0xffb84d);
        t->accent2     = lv_color_hex(0xff6a88);
        t->accent_blue = lv_color_hex(0x0a84ff);

        t->glass_bg      = lv_color_hex(0xf4f9ff);
        t->menu_bg       = lv_color_hex(0xf4f9ff);
        t->scrim         = lv_color_hex(0x10151d);   /* dark modal dimmer */
        t->scrim_input   = lv_color_hex(0x141a24);
        t->btn_secondary = lv_color_hex(0xffffff);
        t->bar_track     = lv_color_hex(0xffffff);

        t->art_fallback = lv_color_hex(0x2b1a44);
        t->art_sun_in   = lv_color_hex(0xffe08a);
        t->art_sun_mid  = lv_color_hex(0xff9e6b);
        t->art_sun_out  = lv_color_hex(0xc9618b);
        t->art_tt_text  = lv_color_hex(0xffe9d6);
        t->progress     = lv_color_hex(0xffd97d);
        t->play_btn_bg  = lv_color_hex(0xff9e6b);
        t->play_btn_fg  = lv_color_hex(0x2a1020);
        t->lrc_active   = lv_color_hex(0xffffff);
        t->lrc_dim      = lv_color_hex(0xe6ecf7);

        t->border_white   = lv_color_hex(0xffffff);
        t->row_icon_muted = lv_color_hex(0xb8c4dd);

        t->bd_grad_top = lv_color_hex(0x6b7edc);
        t->bd_grad_bot = lv_color_hex(0x4e54c4);
        t->bd_blob[0].col = lv_color_hex(0xff4fa3);  /* hot pink */
        t->bd_blob[0].x = 310; t->bd_blob[0].y = 30;
        t->bd_blob[0].r = 200; t->bd_blob[0].str = 90;
        t->bd_blob[1].col = lv_color_hex(0x2f8dff);  /* electric blue */
        t->bd_blob[1].x = 25;  t->bd_blob[1].y = 440;
        t->bd_blob[1].r = 210; t->bd_blob[1].str = 90;
        t->bd_blob[2].col = lv_color_hex(0xffa04d);  /* orange */
        t->bd_blob[2].x = 330; t->bd_blob[2].y = 430;
        t->bd_blob[2].r = 160; t->bd_blob[2].str = 75;
        t->bd_blob[3].col = lv_color_hex(0x9a5cff);  /* violet */
        t->bd_blob[3].x = 55;  t->bd_blob[3].y = 50;
        t->bd_blob[3].r = 170; t->bd_blob[3].str = 75;
        t->bd_blob[4].col = lv_color_hex(0x39d6c8);  /* teal */
        t->bd_blob[4].x = 180; t->bd_blob[4].y = 240;
        t->bd_blob[4].r = 230; t->bd_blob[4].str = 45;

        t->sh_dark    = lv_color_hex(0x3a4356);
        t->sh_dark_opa[0]  = 6;   t->sh_dark_opa[1]  = 9;  t->sh_dark_opa[2]  = 13;
        t->sh_spread[0]    = 8;   t->sh_spread[1]    = 5;  t->sh_spread[2]    = 3;
        t->sh_dy           = 3;
        t->sh_halo_opa[0]  = 12;  t->sh_halo_opa[1]  = 18; t->sh_halo_opa[2]  = 26;
        t->sh_halo_spread[0] = 6; t->sh_halo_spread[1] = 4; t->sh_halo_spread[2] = 3;

        t->card_opa          = 31;      /* white @ 12%: clear frost */
        t->card_border_opa   = 97;      /* 1px rgba(255,255,255,.38) */
        t->glass_shadow_opa  = 45;
        t->menu_card_opa     = LV_OPA_COVER; /* overlay cards must be readable */
        t->menu_border_opa   = 200;
        t->menu_row_opa      = 31;
        t->scrim_opa         = 165;     /* dark dimmer */
        t->scrim_input_opa   = 175;
        t->btn_secondary_opa = 140;
        t->dock_opa          = 160;
        t->chip_opa          = 110;
        t->chip_border_opa   = 120;
        t->bar_track_opa     = 70;
        t->kbd_card_opa      = 200;
    }

    /* geometry */
    t->radius       = 20;            /* big liquid-glass corners */
    t->radius_big   = 28;
    t->menu_row_h   = 40;
    t->menu_gap     = 6;
    t->menu_card_w  = 320;
    t->row_h        = 48;
    t->card_row_h   = 40;
    t->pad_screen   = 12;
    t->gap_row      = 8;

    /* fonts */
    t->font_cjk    = &notosans_sc_16;    t->font_symbol = &lv_font_montserrat_16;
    t->font_small  = &notosans_sc_14;
    t->font_cn     = &notosans_sc_cn16;
    t->font_title  = &notosans_sc_20;
    t->font_mid    = &lv_font_montserrat_28;
    t->font_big    = &lv_font_montserrat_48;
}

void ui_theme_init(void)
{
    ui_theme_fill(&theme);
    /* honour the persisted UI font size (theme_menu 字体 item) */
    switch (global_settings.ui_font_size)
    {
        case 0:  theme.font_cjk = &notosans_sc_14; break;
        case 2:  theme.font_cjk = &notosans_sc_20; break;
        default: theme.font_cjk = &notosans_sc_16; break;
    }
}
