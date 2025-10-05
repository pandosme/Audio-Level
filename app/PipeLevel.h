#ifndef _PIPELEVEL_H_
#define _PIPELEVEL_H_

#include <stdint.h>
#include <stddef.h>

#define PIPELEVEL_PEAK_CHANNELS 2

// Callback: n_channels, latest peak array (window), ambient (long avg, dBFS), userdata
typedef void (*PipeLevel_PeakCallback)(
    unsigned int n_channels,
    const float *peaks,
    float ambient_dBFS,
    void *userdata);

int PipeLevel_init(PipeLevel_PeakCallback cb, void *userdata);
void PipeLevel_deinit(void);
void PipeLevel_set_interval(unsigned int seconds);

#endif
