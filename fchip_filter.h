#pragma once

#define FCHIP_FPARAM_FILTERTYPE_NOCHANGE -1
#define FCHIP_FPARAM_SAMPLERATE_NOCHANGE -1.0f
#define FCHIP_FPARAM_CUTOFF_NOCHANGE -1.0f

typedef float fchip_float_t;

enum fchip_filter_type{
    FCHIP_FILTER_NONE,
    FCHIP_FILTER_LOWPASS,
    FCHIP_FILTER_HIPASS,
    FCHIP_FILTER_BANDPASS,
};

// a biquad convolution table is used 
// in the current implementation
struct fchip_conv_table
{
    fchip_float_t b0;
    fchip_float_t b1;
    fchip_float_t b2;

    fchip_float_t a1;
    fchip_float_t a2;
};

struct fchip_channel_filter
{
    struct fchip_conv_table coeffs;
    fchip_float_t raw[3];
    fchip_float_t processed[3];

    enum fchip_filter_type filter_type;
    int sample_rate;
    fchip_float_t cutoff_freq;
};


struct fchip_channel_filter* fchip_filter_create(enum fchip_filter_type filter_type, int sample_rate, fchip_float_t cutoff_freq);
void fchip_filter_change_params(struct fchip_channel_filter *filter, enum fchip_filter_type filter_type, fchip_float_t sample_rate, fchip_float_t cutoff_freq);

inline fchip_float_t fchip_filter_process(struct fchip_channel_filter* filter, fchip_float_t sample);