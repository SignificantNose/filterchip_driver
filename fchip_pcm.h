#pragma once
#include "fchip.h"
#include "fchip_filter.h"
#include <sound/pcm.h>
#include <sound/pcm_params.h>


#define dsp_lock(dev)		snd_hdac_dsp_lock(azx_dev_to_hdac_stream(dev))
#define dsp_unlock(dev)		snd_hdac_dsp_unlock(azx_dev_to_hdac_stream(dev))
#define dsp_is_locked(dev)	snd_hdac_stream_is_locked(azx_dev_to_hdac_stream(dev))


struct azx_pcm {
	struct fchip_azx *chip;
	struct snd_pcm *pcm;
	struct hda_codec *codec;
	struct hda_pcm *info;
	struct list_head list;
};

struct fchip_runtime_pr
{
    struct azx_dev *dev;
	
    snd_pcm_uframes_t filter_ptr;
    struct fchip_channel_filter *filters; 
    int filter_channels;    // amount of actually present filters
	int filter_count;       // max filters available

	int bytes_per_sample;
	int bit_depth;
	
	// bit shift describes the amount of excessive bits 
	// that do not possess any value (must be zero, or 
	// sign-extended in the conversion process) 
	int bit_shift; 			// aggregating field; equal to bytes_for<<3-bit_depth
	int sample_max_value; 	// aggregating field; equal to (bit_depth-1)<<3;
};


void fchip_pcm_validate_filter_params(void);

int fchip_pcm_open(struct snd_pcm_substream *substream);
int fchip_pcm_close(struct snd_pcm_substream *substream);
snd_pcm_uframes_t fchip_pcm_pointer(struct snd_pcm_substream *substream);


int fchip_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *hw_params);
int fchip_pcm_hw_free(struct snd_pcm_substream *substream);
int fchip_pcm_prepare(struct snd_pcm_substream *substream);
int fchip_pcm_trigger(struct snd_pcm_substream *substream, int cmd);
int fchip_pcm_get_time_info(struct snd_pcm_substream *substream,
			struct timespec64 *system_ts, struct timespec64 *audio_ts,
			struct snd_pcm_audio_tstamp_config *audio_tstamp_config,
			struct snd_pcm_audio_tstamp_report *audio_tstamp_report);