#pragma once

#define FCHIP_FPARAM_FILTERTYPE_NOCHANGE -1
#define FCHIP_FPARAM_SAMPLERATE_NOCHANGE -1
#define FCHIP_FPARAM_CUTOFF_NOCHANGE -1

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
    double b0;
    double b1;
    double b2;

    double a1;
    double a2;
};

struct fchip_channel_filter
{
    struct fchip_conv_table coeffs;
    double raw[3];
    double processed[3];

    enum fchip_filter_type filter_type;
    int sample_rate;
    double cutoff_freq;
};


struct fchip_channel_filter* fchip_filter_create(enum fchip_filter_type filter_type, int sample_rate, double cutoff_freq);
void fchip_filter_change_params(struct fchip_channel_filter *filter, enum fchip_filter_type filter_type, double sample_rate, double cutoff_freq);

inline double fchip_filter_process(struct fchip_channel_filter* filter, double sample);