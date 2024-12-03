#include <linux/slab.h>
#include "fchip_filter.h"

#define M_PI 3.14159265358979323846f
#define M_SQRT2 1.414213562373095f

static fchip_float_t fchip_filter_transform_frequency(fchip_float_t sample_rate, fchip_float_t freq){
    // the original filter had a tan function here.
    // an approximation is used here: tan(x) ~ x + (x^3)/3 
    fchip_float_t f = freq * M_PI / sample_rate;
    return f + (f * f * f) / 3;
}

static void fchip_calculate_convolution_table(
    struct fchip_channel_filter *filter
)
{
    fchip_float_t w;
    fchip_float_t d, w1, w2, w0sqr, wd; // for bandpass filter
    fchip_float_t a0, a1, a2, b0, b1, b2;
    switch(filter->filter_type){
        case FCHIP_FILTER_LOWPASS:
            w = fchip_filter_transform_frequency(filter->sample_rate, filter->cutoff_freq);
            
            a0 = 1 + M_SQRT2*w + w*w;
            a1 = -2 + 2*w*w;
            a2 = 1 - M_SQRT2*w + w*w;

            b0 = w*w;
            b1 = 2*w*w;
            b2 = w*w;
    
            break;

        case FCHIP_FILTER_HIPASS:
            w = fchip_filter_transform_frequency(filter->sample_rate, filter->cutoff_freq);
            
            a0 = 1 + M_SQRT2*w + w*w;
            a1 = -2 + 2*w*w;
            a2 = 1 - M_SQRT2*w + w*w;
            
            b0 = 1;
            b1 = -2;
            b2 = -1;
            break;

        case FCHIP_FILTER_BANDPASS:
            w = filter->cutoff_freq;
            
            d = w/4;    // band width
            w1 = (w-d) > 0 ? (w-d) : 0;
            w1 = fchip_filter_transform_frequency(filter->sample_rate, w1);
            w2 = (w+d) < filter->sample_rate ? (w+d) : filter->sample_rate;
            w2 = fchip_filter_transform_frequency(filter->sample_rate, w2);

            w0sqr = w1*w2;
            wd = w2-w1;

            a0 = -1 - wd - w0sqr;
            a1 = 2 - 2*w0sqr;
            a2 = -1 + wd - w0sqr;
            
            b0 = -wd;
            b1 = 0;
            b2 = wd;
            break;

        // case FCHIP_FILTER_NONE:
        default:
            b0 = 1;
            b1 = 0;
            b2 = 0;
            
            a0 = 1;
            a1 = 0;
            a2 = 0;

    }
    filter->coeffs.b0 = b0 / a0;
    filter->coeffs.b1 = b1 / a0;
    filter->coeffs.b2 = b2 / a0;

    filter->coeffs.a1 = a1 / a0;
    filter->coeffs.a2 = a2 / a0;
}

struct fchip_channel_filter* fchip_filter_create(enum fchip_filter_type filter_type, int sample_rate, fchip_float_t cutoff_freq)
{
    struct fchip_channel_filter *filter = kzalloc(sizeof(struct fchip_channel_filter), GFP_KERNEL);
    filter->filter_type = filter_type;
    filter->cutoff_freq = cutoff_freq;
    filter->sample_rate = sample_rate;
    fchip_calculate_convolution_table(filter);
    return filter;
}

void fchip_filter_change_params(
    struct fchip_channel_filter *filter, 
    enum fchip_filter_type filter_type, 
    fchip_float_t sample_rate,
    fchip_float_t cutoff_freq
    )
{
    if (filter_type != FCHIP_FPARAM_FILTERTYPE_NOCHANGE){
        filter->filter_type = filter_type;
    }
    if(sample_rate != FCHIP_FPARAM_SAMPLERATE_NOCHANGE){
        filter->sample_rate = sample_rate;
    }
    if(cutoff_freq != FCHIP_FPARAM_CUTOFF_NOCHANGE){
        filter->cutoff_freq = cutoff_freq;
    }

    
    for(int i = 0; i < 3; i++){
        filter->raw[i] = 0;
        filter->processed[i] = 0;
    }

    fchip_calculate_convolution_table(filter);
}


inline fchip_float_t fchip_filter_process(
    struct fchip_channel_filter* filter, 
    fchip_float_t sample
)
{
    filter->raw[2] = filter->raw[1];
    filter->raw[1] = filter->raw[0];
    filter->raw[0] = sample;

    filter->processed[2] = filter->processed[1];
    filter->processed[1] = filter->processed[0];
    
    filter->processed[0] = 
        filter->coeffs.b0 * filter->raw[0]          // b0 * raw0 
      + filter->coeffs.b1 * filter->raw[1]          // b1 * raw1
      + filter->coeffs.b2 * filter->raw[2]          // b2 * raw2

      - filter->coeffs.a1 * filter->processed[1]    // a1 * proc1
      - filter->coeffs.a2 * filter->processed[2]    // a2 * proc2
    ;

    return filter->processed[0];
}