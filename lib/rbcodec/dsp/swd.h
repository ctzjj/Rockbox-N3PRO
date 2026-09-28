#include <stdbool.h>
void dsp_set_swd_enable(bool enable);
void dsp_set_swd_params(long spread, long center, long focus,
                          long definition, long bass);
void dsp_set_swd_mode(int mode);
void dsp_set_swd_tbeq(int index);
void dsp_set_swd_gains(int ing, int outg);
void dsp_set_swd_limiter(int mode);
