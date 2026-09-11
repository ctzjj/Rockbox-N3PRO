#ifndef __HIBYLINUX_CODEC__
#define __HIBYLINUX_CODEC__

#ifdef CAYIN_N3PRO
#define AUDIOHW_CAPS (FILTER_ROLL_OFF_CAP|TUBE_MODE_CAP)
#else
#define AUDIOHW_CAPS (FILTER_ROLL_OFF_CAP)
#endif
AUDIOHW_SETTING(VOLUME, "dB", 1, 5, -102*10,  0, -30*10)
#endif

//#define AUDIOHW_MUTE_ON_STOP
#define AUDIOHW_MUTE_ON_SRATE_CHANGE
//#define AUDIOHW_NEEDS_INITIAL_UNMUTE

AUDIOHW_SETTING(FILTER_ROLL_OFF, "", 0, 1, 0, 4, 0)
#define AUDIOHW_HAVE_SHORT2_ROLL_OFF

#ifdef CAYIN_N3PRO
AUDIOHW_SETTING(TUBE_MODE, "", 0, 1, 0, 2, 0)
#endif

void audiohw_mute(int mute);
void hiby_set_output(int ps);
int hiby_get_outputs(void);
