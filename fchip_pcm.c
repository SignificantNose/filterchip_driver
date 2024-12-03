#include "fchip_pcm.h"
#include "fchip_posfix.h"
#include "fchip.h"


static const struct snd_pcm_hardware fchip_pcm_hw = {
	.info =			(SNDRV_PCM_INFO_MMAP |
				 SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_BLOCK_TRANSFER |
				 SNDRV_PCM_INFO_MMAP_VALID |
				 /* No full-resume yet implemented */
				 /* SNDRV_PCM_INFO_RESUME |*/
				 SNDRV_PCM_INFO_PAUSE |
				 SNDRV_PCM_INFO_SYNC_START |
				 SNDRV_PCM_INFO_HAS_WALL_CLOCK | /* legacy */
				 SNDRV_PCM_INFO_HAS_LINK_ATIME |
				 SNDRV_PCM_INFO_NO_PERIOD_WAKEUP),
	.formats =		SNDRV_PCM_FMTBIT_S16_LE,
	.rates =		SNDRV_PCM_RATE_48000,
	.rate_min =		48000,
	.rate_max =		48000,
	.channels_min =		2,
	.channels_max =		2,
	.buffer_bytes_max =	AZX_MAX_BUF_SIZE,
	.period_bytes_min =	128,
	.period_bytes_max =	AZX_MAX_BUF_SIZE / 2,
	.periods_min =		2,
	.periods_max =		AZX_MAX_FRAG,
	.fifo_size =		0,
};

// static struct snd_pcm_hardware snd_filterchip_playback_hw = {
//     .info = (SNDRV_PCM_INFO_MMAP |
//                 SNDRV_PCM_INFO_INTERLEAVED |
//                 SNDRV_PCM_INFO_BLOCK_TRANSFER |
//                 SNDRV_PCM_INFO_MMAP_VALID),
//                 /*
// 				  SNDRV_PCM_INFO_RESUME | // not fully implemented?
// 				 SNDRV_PCM_INFO_PAUSE |
// 				 SNDRV_PCM_INFO_SYNC_START |
// 				 SNDRV_PCM_INFO_HAS_WALL_CLOCK | // legacy
// 				 SNDRV_PCM_INFO_HAS_LINK_ATIME |
// 				 SNDRV_PCM_INFO_NO_PERIOD_WAKEUP),
//                  */
//     .formats =          SNDRV_PCM_FMTBIT_S16_LE,
//     .rates =            SNDRV_PCM_RATE_48000,
//     .rate_min =         48000,
//     .rate_max =         48000,
//     .channels_min =     2,
//     .channels_max =     2,
//     .buffer_bytes_max = 32768,  // 4*1024*1024
//     .period_bytes_min = 4096,   // 128
//     .period_bytes_max = 32768,  // 2*1024*1024
//     .periods_min =      1,      // 2
//     .periods_max =      1024,   // 32 in hda_controller
// }; 


static inline struct hda_pcm_stream *
to_hda_pcm_stream(struct snd_pcm_substream *substream)
{
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	return &apcm->info->stream[substream->stream];
}

static inline struct azx_dev *fchip_get_azx_dev(struct snd_pcm_substream *substream)
{
	return ((struct fchip_runtime_pr*)(substream->runtime->private_data))->dev;
}

static inline struct azx_dev *fchip_assign_device(struct fchip_azx *fchip_azx, struct snd_pcm_substream *substream)
{
	struct hdac_stream *s;

	s = snd_hdac_stream_assign(azx_to_hda_bus(fchip_azx), substream);
	if (!s){
		return NULL;
    }
	return hdac_stream_to_azx_dev(s);
}

static inline void fchip_release_device(struct azx_dev *azx_dev)
{
	snd_hdac_stream_release(azx_dev_to_hdac_stream(azx_dev));
}

static unsigned int fchip_pcm_get_position(struct fchip_azx *chip,
			      struct azx_dev *azx_dev)
{
	struct snd_pcm_substream *substream = azx_dev->core.substream;
	unsigned int pos;
	int stream = substream->stream;
	int delay = 0;

	if (chip->get_position[stream])
		pos = chip->get_position[stream](chip, azx_dev);
	else /* use the position buffer as default */
		pos = fchip_get_pos_posbuf(chip, azx_dev);

	if (pos >= azx_dev->core.bufsize)
		pos = 0;

	if (substream->runtime) {
		struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
		struct hda_pcm_stream *hinfo = to_hda_pcm_stream(substream);

		if (chip->get_delay[stream])
			delay += chip->get_delay[stream](chip, azx_dev, pos);
		if (hinfo->ops.get_delay)
			delay += hinfo->ops.get_delay(hinfo, apcm->codec,
						      substream);
		substream->runtime->delay = delay;
	}

	// trace_azx_get_position(chip, azx_dev, pos, delay);
	return pos;
}

inline void fchip_filter_process_region(snd_pcm_uframes_t total_frames, void *data_ptr, struct fchip_runtime_pr *pr){
	int channels = pr->filter_channels;
	int32_t *casted_data_ptr = (int32_t*)data_ptr; 
	struct fchip_channel_filter *filters = pr->filters;
	int bit_shift = pr->bit_shift;
	int sample_max_value = pr->sample_max_value;
	
    snd_pcm_uframes_t frame;
	int channel_idx;

	float raw;
	float processed;
	int32_t raw_i;
	int32_t processed_i;


    for(frame = 0; frame<total_frames; frame++){
        for(channel_idx = 0; channel_idx<channels; channel_idx++){

            raw_i = (*casted_data_ptr)>>bit_shift;
            raw = (raw_i*1.0f)/sample_max_value; 

            processed = fchip_filter_process(&filters[channel_idx], raw);
            processed_i = (int32_t)(processed*sample_max_value);
            
            *casted_data_ptr = (processed_i<<bit_shift); 
            casted_data_ptr++;
        }
    }	
}


snd_pcm_uframes_t fchip_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	struct fchip_azx *chip = apcm->chip;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct fchip_runtime_pr *runtime_pr = runtime->private_data;

	snd_pcm_uframes_t sw_ptr = runtime->control->appl_ptr % runtime->buffer_size;
	snd_pcm_uframes_t filter_ptr = runtime_pr->filter_ptr % runtime->buffer_size;
	unsigned char *dma_area = runtime->dma_area;
	unsigned int buffer_size;
	ssize_t frame_in_bytes;
	snd_pcm_uframes_t res;

	
	res = bytes_to_frames(runtime, fchip_pcm_get_position(chip, runtime_pr->dev));
	buffer_size = runtime->buffer_size;
	frame_in_bytes = runtime->frame_bits / 8;

	sw_ptr = runtime->control->appl_ptr % runtime->buffer_size;
	filter_ptr = runtime_pr->filter_ptr % runtime->buffer_size;
	dma_area = runtime->dma_area;

	if (filter_ptr != sw_ptr) {
        if (filter_ptr < sw_ptr) {
			fchip_filter_process_region(sw_ptr-filter_ptr, dma_area+filter_ptr*frame_in_bytes, runtime_pr);
		} 
		else {
			fchip_filter_process_region(buffer_size-filter_ptr, dma_area+filter_ptr*frame_in_bytes, runtime_pr);
			fchip_filter_process_region(sw_ptr, dma_area+filter_ptr*frame_in_bytes, runtime_pr);
        }
        runtime_pr->filter_ptr = sw_ptr;
    }

	return res;
}

static struct fchip_runtime_pr *fchip_runtime_private_init(struct azx_dev *azx_dev, int channel_count){
	
	struct fchip_runtime_pr *runtime_pr = kmalloc(sizeof(*runtime_pr), GFP_KERNEL);
	if(!runtime_pr){
		return NULL;
	}
	runtime_pr->dev = azx_dev;
	runtime_pr->filter_ptr = 0;
	runtime_pr->filter_channels = 0;
	runtime_pr->filter_count = channel_count;
	runtime_pr->filters = kmalloc(sizeof(struct fchip_channel_filter)*channel_count, GFP_KERNEL); 
	if(!runtime_pr->filters){
		kfree(runtime_pr);
		return NULL;
	}

	for(int i=0; i<channel_count; i++){
		// init cutoff and filter types here once, do not change 
		// them later (pass the corresponding parameters)
		fchip_filter_change_params(&runtime_pr->filters[i], FCHIP_FILTER_HIPASS, 48000, 300);
	}
	return runtime_pr;
}

static void fchip_runtime_private_free(struct fchip_runtime_pr *runtime_pr){
	kfree(runtime_pr->filters);
	kfree(runtime_pr);
}

int fchip_pcm_open(struct snd_pcm_substream *substream)
{
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	struct hda_pcm_stream *hinfo = to_hda_pcm_stream(substream);
	struct fchip_azx *fchip_azx = apcm->chip;
	struct azx_dev *azx_dev;
	struct snd_pcm_runtime *runtime = substream->runtime;
    struct fchip_runtime_pr *runtime_pr;
	int err;
	int buff_step;
	int channel_count;

    printk(KERN_INFO "fchip: pcm open called\n");

	snd_hda_codec_pcm_get(apcm->info);
	mutex_lock(&fchip_azx->open_mutex);
	azx_dev = fchip_assign_device(fchip_azx, substream);
	if (azx_dev == NULL) {
		err = -EBUSY;
		goto unlock;
	}

	runtime_pr = fchip_runtime_private_init(azx_dev, channel_count = hinfo->channels_max);
	if(!runtime_pr){
		err = -ENOMEM;
		goto unlock;
	}
	runtime->private_data = runtime_pr;

	runtime->hw = fchip_pcm_hw;
	if (fchip_azx->gts_present){
		runtime->hw.info |= SNDRV_PCM_INFO_HAS_LINK_SYNCHRONIZED_ATIME;
    }

	runtime->hw.channels_min = hinfo->channels_min;
	runtime->hw.channels_max = hinfo->channels_max;
	runtime->hw.formats = hinfo->formats;
	runtime->hw.rates = hinfo->rates;
	snd_pcm_limit_hw_rates(runtime);
	snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);

	/* avoid wrap-around with wall-clock */
	snd_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_BUFFER_TIME,
				     20,
				     178000000);

	if (fchip_azx->align_buffer_size){
		/* constrain buffer sizes to be multiple of 128
		   bytes. This is more efficient in terms of memory
		   access but isn't required by the HDA spec and
		   prevents users from specifying exact period/buffer
		   sizes. For example for 44.1kHz, a period size set
		   to 20ms will be rounded to 19.59ms. */
		buff_step = 128;
    }
	else{
		/* Don't enforce steps on buffer sizes, still need to
		   be multiple of 4 bytes (HDA spec). Tested on Intel
		   HDA controllers, may not work on all devices where
		   option needs to be disabled */
		buff_step = 4;
    }

	snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, buff_step);
	snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, buff_step);
	snd_hda_power_up(apcm->codec);
	if (hinfo->ops.open){
		err = hinfo->ops.open(hinfo, apcm->codec, substream);
    }
	else{
		err = -ENODEV;
    }
	if (err < 0) {
		fchip_release_device(azx_dev);
		goto powerdown;
	}
	snd_pcm_limit_hw_rates(runtime);
	/* sanity check */
	if (snd_BUG_ON(!runtime->hw.channels_min) ||
	    snd_BUG_ON(!runtime->hw.channels_max) ||
	    snd_BUG_ON(!runtime->hw.formats) ||
	    snd_BUG_ON(!runtime->hw.rates))     
    {
		fchip_release_device(azx_dev);
		if (hinfo->ops.close){
			hinfo->ops.close(hinfo, apcm->codec, substream);
        }
		err = -EINVAL;
		goto powerdown;
	}

	/* disable LINK_ATIME timestamps for capture streams
	   until we figure out how to handle digital inputs */
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		runtime->hw.info &= ~SNDRV_PCM_INFO_HAS_WALL_CLOCK; /* legacy */
		runtime->hw.info &= ~SNDRV_PCM_INFO_HAS_LINK_ATIME;
	}

	snd_pcm_set_sync(substream);
	mutex_unlock(&fchip_azx->open_mutex);
	return 0;

 powerdown:
	snd_hda_power_down(apcm->codec);
 unlock:
	mutex_unlock(&fchip_azx->open_mutex);
	snd_hda_codec_pcm_put(apcm->info);
	return err;
}

int fchip_pcm_close(struct snd_pcm_substream *substream)
{
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	struct hda_pcm_stream *hinfo = to_hda_pcm_stream(substream);
	struct fchip_azx *fchip_azx = apcm->chip;
	struct fchip_runtime_pr *runtime_pr = substream->runtime->private_data;

	printk(KERN_DEBUG "fchip: close called\n");

	mutex_lock(&fchip_azx->open_mutex);
	fchip_release_device(runtime_pr->dev);

    // de-allocate filter
	if (hinfo->ops.close){
		hinfo->ops.close(hinfo, apcm->codec, substream);
    }
	snd_hda_power_down(apcm->codec);
	fchip_runtime_private_free(runtime_pr);

	mutex_unlock(&fchip_azx->open_mutex);
	snd_hda_codec_pcm_put(apcm->info);
	return 0;
}


int fchip_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *hw_params)
{
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	struct fchip_azx *fchip_azx = apcm->chip;
	struct azx_dev *azx_dev = fchip_get_azx_dev(substream);
	struct hdac_stream *hdas = azx_dev_to_hdac_stream(azx_dev);
	int ret = 0;

	dsp_lock(azx_dev);
	if (dsp_is_locked(azx_dev)) {
		ret = -EBUSY;
		goto unlock;
	}

	/* Set up BDLEs here, return -ENOMEM if too many BDLEs are required */
	hdas->bufsize = params_buffer_bytes(hw_params);
	hdas->period_bytes = params_period_bytes(hw_params);
	hdas->format_val = 0;
	hdas->no_period_wakeup =
		(hw_params->info & SNDRV_PCM_INFO_NO_PERIOD_WAKEUP) &&
		(hw_params->flags & SNDRV_PCM_HW_PARAMS_NO_PERIOD_WAKEUP);
	if (snd_hdac_stream_setup_periods(hdas) < 0)
		ret = -ENOMEM;

unlock:
	dsp_unlock(azx_dev);
	return ret;
}


int fchip_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	struct azx_dev *azx_dev = fchip_get_azx_dev(substream);
	struct hda_pcm_stream *hinfo = to_hda_pcm_stream(substream);

	/* reset BDL address */
	dsp_lock(azx_dev);
	if (!dsp_is_locked(azx_dev))
		snd_hdac_stream_cleanup(azx_dev_to_hdac_stream(azx_dev));

	snd_hda_codec_cleanup(apcm->codec, hinfo, substream);

	azx_dev_to_hdac_stream(azx_dev)->prepared = 0;
	dsp_unlock(azx_dev);
	return 0;
}

static void fchip_filter_prepare(struct fchip_runtime_pr *runtime_pr, int bits, int channels, int sample_rate){
	runtime_pr->bit_depth = bits;
	runtime_pr->bytes_per_sample = 4; // weak one
	runtime_pr->bit_shift = runtime_pr->bytes_per_sample<<3 - runtime_pr->bit_depth;
	// signed only; hda spec does not say anything about unsigned -> bit_depth-1
	runtime_pr->sample_max_value = (runtime_pr->bit_depth-1)<<3; 
	runtime_pr->filter_channels = channels;
	for(int i=0; i<runtime_pr->filter_channels; i++){
		fchip_filter_change_params(&runtime_pr->filters[i], FCHIP_FPARAM_FILTERTYPE_NOCHANGE, sample_rate, FCHIP_FPARAM_CUTOFF_NOCHANGE);
	}
}

int fchip_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	struct fchip_azx *fchip_azx = apcm->chip;
	struct azx_dev *azx_dev = fchip_get_azx_dev(substream);
	struct hda_pcm_stream *hinfo = to_hda_pcm_stream(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct fchip_runtime_pr *runtime_pr = runtime->private_data;
	unsigned int format_val, stream_tag, bits;
	int err;
	struct hda_spdif_out *spdif =
		snd_hda_spdif_out_of_nid(apcm->codec, hinfo->nid);
	unsigned short ctls = spdif ? spdif->ctls : 0;

	dsp_lock(azx_dev);
	if (dsp_is_locked(azx_dev)) {
		err = -EBUSY;
		goto unlock;
	}

	snd_hdac_stream_reset(azx_dev_to_hdac_stream(azx_dev));
	bits = snd_hdac_stream_format_bits(runtime->format, SNDRV_PCM_SUBFORMAT_STD, hinfo->maxbps);

	format_val = snd_hdac_spdif_stream_format(runtime->channels, bits, runtime->rate, ctls);
	if (!format_val) {
		printk(KERN_ERR "fchip: invalid format_val, rate=%d, ch=%d, format=%d\n",
			runtime->rate, runtime->channels, runtime->format);
		err = -EINVAL;
		goto unlock;
	}

	fchip_filter_prepare(runtime_pr, bits, runtime->channels, runtime->rate);
	printk(KERN_DEBUG "fchip: bits:%d channels:%d rate:%d fmt_val:%d\n", bits, runtime->channels, runtime->rate, format_val);

	err = snd_hdac_stream_set_params(azx_dev_to_hdac_stream(azx_dev), format_val);
	if (err < 0)
		goto unlock;

	snd_hdac_stream_setup(azx_dev_to_hdac_stream(azx_dev), false);

	stream_tag = azx_dev->core.stream_tag;
	/* CA-IBG chips need the playback stream starting from 1 */
	if ((fchip_azx->driver_caps & AZX_DCAPS_CTX_WORKAROUND) &&
	    stream_tag > fchip_azx->capture_streams)
		stream_tag -= fchip_azx->capture_streams;
	err = snd_hda_codec_prepare(apcm->codec, hinfo, stream_tag,
				     azx_dev->core.format_val, substream);

 unlock:
	if (!err)
		azx_dev_to_hdac_stream(azx_dev)->prepared = 1;
	dsp_unlock(azx_dev);
	return err;
}


int fchip_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	struct fchip_azx *fchip_azx = apcm->chip;
	struct hdac_bus *bus = azx_to_hda_bus(fchip_azx);
	struct azx_dev *azx_dev;
	struct snd_pcm_substream *s;
	struct hdac_stream *hstr;
	bool start;
	int sbits = 0;
	int sync_reg;

	azx_dev = fchip_get_azx_dev(substream);

	hstr = azx_dev_to_hdac_stream(azx_dev);
	if (fchip_azx->driver_caps & AZX_DCAPS_OLD_SSYNC)
		sync_reg = AZX_REG_OLD_SSYNC;
	else
		sync_reg = AZX_REG_SSYNC;

	if (dsp_is_locked(azx_dev) || !hstr->prepared)
		return -EPIPE;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		start = true;
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_STOP:
		start = false;
		break;
	default:
		return -EINVAL;
	}

	snd_pcm_group_for_each_entry(s, substream) {
		if (s->pcm->card != substream->pcm->card)
			continue;
		azx_dev = fchip_get_azx_dev(s);
		sbits |= 1 << azx_dev->core.index;
		snd_pcm_trigger_done(s, substream);
	}

	spin_lock(&bus->reg_lock);

	/* first, set SYNC bits of corresponding streams */
	snd_hdac_stream_sync_trigger(hstr, true, sbits, sync_reg);

	snd_pcm_group_for_each_entry(s, substream) {
		if (s->pcm->card != substream->pcm->card)
			continue;
		azx_dev = fchip_get_azx_dev(s);
		if (start) {
			azx_dev->insufficient = 1;
			snd_hdac_stream_start(azx_dev_to_hdac_stream(azx_dev));
		} else {
			snd_hdac_stream_stop(azx_dev_to_hdac_stream(azx_dev));
		}
	}
	spin_unlock(&bus->reg_lock);

	snd_hdac_stream_sync(hstr, start, sbits);

	spin_lock(&bus->reg_lock);
	/* reset SYNC bits */
	snd_hdac_stream_sync_trigger(hstr, false, sbits, sync_reg);
	if (start)
		snd_hdac_stream_timecounter_init(hstr, sbits);
	spin_unlock(&bus->reg_lock);
	return 0;
}



#ifdef CONFIG_X86
static u64 fchip_scale64(u64 base, u32 num, u32 den)
{
	u64 rem;

	rem = do_div(base, den);

	base *= num;
	rem *= num;

	do_div(rem, den);

	return base + rem;
}

static int fchip_get_sync_time(ktime_t *device, struct system_counterval_t *system, void *ctx)
{
	struct snd_pcm_substream *substream = ctx;
	struct azx_dev *azx_dev = fchip_get_azx_dev(substream);
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	struct fchip_azx *fchip_azx = apcm->chip;
	struct snd_pcm_runtime *runtime;
	u64 ll_counter, ll_counter_l, ll_counter_h;
	u64 tsc_counter, tsc_counter_l, tsc_counter_h;
	u32 wallclk_ctr, wallclk_cycles;
	bool direction;
	u32 dma_select;
	u32 timeout;
	u32 retry_count = 0;

	runtime = substream->runtime;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		direction = 1;
	else
		direction = 0;

	/* 0th stream tag is not used, so DMA ch 0 is for 1st stream tag */
	do {
		timeout = 100;
		dma_select = (direction << GTSCC_CDMAS_DMA_DIR_SHIFT) |
					(azx_dev->core.stream_tag - 1);
		snd_hdac_chip_writel(azx_to_hda_bus(fchip_azx), GTSCC, dma_select);

		/* Enable the capture */
		snd_hdac_chip_updatel(azx_to_hda_bus(fchip_azx), GTSCC, 0, GTSCC_TSCCI_MASK);

		while (timeout) {
			if (snd_hdac_chip_readl(azx_to_hda_bus(fchip_azx), GTSCC) &
						GTSCC_TSCCD_MASK)
				break;

			timeout--;
		}

		if (!timeout) {
			printk(KERN_ERR "fchip: GTSCC capture Timedout!\n");
			return -EIO;
		}

		/* Read wall clock counter */
		wallclk_ctr = snd_hdac_chip_readl(azx_to_hda_bus(fchip_azx), WALFCC);

		/* Read TSC counter */
		tsc_counter_l = snd_hdac_chip_readl(azx_to_hda_bus(fchip_azx), TSCCL);
		tsc_counter_h = snd_hdac_chip_readl(azx_to_hda_bus(fchip_azx), TSCCU);

		/* Read Link counter */
		ll_counter_l = snd_hdac_chip_readl(azx_to_hda_bus(fchip_azx), LLPCL);
		ll_counter_h = snd_hdac_chip_readl(azx_to_hda_bus(fchip_azx), LLPCU);

		/* Ack: registers read done */
		snd_hdac_chip_writel(azx_to_hda_bus(fchip_azx), GTSCC, GTSCC_TSCCD_SHIFT);

		tsc_counter = (tsc_counter_h << TSCCU_CCU_SHIFT) |
						tsc_counter_l;

		ll_counter = (ll_counter_h << LLPC_CCU_SHIFT) |	ll_counter_l;
		wallclk_cycles = wallclk_ctr & WALFCC_CIF_MASK;

		/*
		 * An error occurs near frame "rollover". The clocks in
		 * frame value indicates whether this error may have
		 * occurred. Here we use the value of 10 i.e.,
		 * HDA_MAX_CYCLE_OFFSET
		 */
		if (wallclk_cycles < HDA_MAX_CYCLE_VALUE - HDA_MAX_CYCLE_OFFSET
					&& wallclk_cycles > HDA_MAX_CYCLE_OFFSET)
			break;

		/*
		 * Sleep before we read again, else we may again get
		 * value near to MAX_CYCLE. Try to sleep for different
		 * amount of time so we dont hit the same number again
		 */
		udelay(retry_count++);

	} while (retry_count != HDA_MAX_CYCLE_READ_RETRY);

	if (retry_count == HDA_MAX_CYCLE_READ_RETRY) {
		dev_err_ratelimited(fchip_azx->card->dev,
			"Error in WALFCC cycle count\n");
		return -EIO;
	}

	*device = ns_to_ktime(fchip_scale64(ll_counter,
				NSEC_PER_SEC, runtime->rate));
	*device = ktime_add_ns(*device, (wallclk_cycles * NSEC_PER_SEC) /
			       ((HDA_MAX_CYCLE_VALUE + 1) * runtime->rate));

	system->cycles = tsc_counter;
	// system->cs_id = CSID_X86_ART;
	system->cs_id = 5;

	return 0;
}

#else
static int fchip_get_sync_time(ktime_t *device, struct system_counterval_t *system, void *ctx)
{
	return -ENXIO;
}
#endif

static u64 fchip_adjust_codec_delay(struct snd_pcm_substream *substream,
				u64 nsec)
{
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	struct hda_pcm_stream *hinfo = to_hda_pcm_stream(substream);
	u64 codec_frames, codec_nsecs;

	if (!hinfo->ops.get_delay)
		return nsec;

	codec_frames = hinfo->ops.get_delay(hinfo, apcm->codec, substream);
	codec_nsecs = div_u64(codec_frames * 1000000000LL,
			      substream->runtime->rate);

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		return nsec + codec_nsecs;

	return (nsec > codec_nsecs) ? nsec - codec_nsecs : 0;
}

static int fchip_get_crosststamp(struct snd_pcm_substream *substream,
			      struct system_device_crosststamp *xtstamp)
{
	return get_device_system_crosststamp(fchip_get_sync_time,
					substream, NULL, xtstamp);
}

static inline bool is_link_time_supported(struct snd_pcm_runtime *runtime,
				struct snd_pcm_audio_tstamp_config *ts)
{
	if (runtime->hw.info & SNDRV_PCM_INFO_HAS_LINK_SYNCHRONIZED_ATIME)
		if (ts->type_requested == SNDRV_PCM_AUDIO_TSTAMP_TYPE_LINK_SYNCHRONIZED)
			return true;

	return false;
}

int fchip_pcm_get_time_info(struct snd_pcm_substream *substream,
			struct timespec64 *system_ts, struct timespec64 *audio_ts,
			struct snd_pcm_audio_tstamp_config *audio_tstamp_config,
			struct snd_pcm_audio_tstamp_report *audio_tstamp_report)
{
	struct azx_dev *azx_dev = fchip_get_azx_dev(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct system_device_crosststamp xtstamp;
	int ret;
	u64 nsec;

	if ((substream->runtime->hw.info & SNDRV_PCM_INFO_HAS_LINK_ATIME) &&
		(audio_tstamp_config->type_requested == SNDRV_PCM_AUDIO_TSTAMP_TYPE_LINK)) {

		snd_pcm_gettime(substream->runtime, system_ts);

		nsec = timecounter_read(&azx_dev->core.tc);
		if (audio_tstamp_config->report_delay)
			nsec = fchip_adjust_codec_delay(substream, nsec);

		*audio_ts = ns_to_timespec64(nsec);

		audio_tstamp_report->actual_type = SNDRV_PCM_AUDIO_TSTAMP_TYPE_LINK;
		audio_tstamp_report->accuracy_report = 1; /* rest of structure is valid */
		audio_tstamp_report->accuracy = 42; /* 24 MHz WallClock == 42ns resolution */

	} else if (is_link_time_supported(runtime, audio_tstamp_config)) {

		ret = fchip_get_crosststamp(substream, &xtstamp);
		if (ret)
			return ret;

		switch (runtime->tstamp_type) {
		case SNDRV_PCM_TSTAMP_TYPE_MONOTONIC:
			return -EINVAL;

		case SNDRV_PCM_TSTAMP_TYPE_MONOTONIC_RAW:
			*system_ts = ktime_to_timespec64(xtstamp.sys_monoraw);
			break;

		default:
			*system_ts = ktime_to_timespec64(xtstamp.sys_realtime);
			break;

		}

		*audio_ts = ktime_to_timespec64(xtstamp.device);

		audio_tstamp_report->actual_type =
			SNDRV_PCM_AUDIO_TSTAMP_TYPE_LINK_SYNCHRONIZED;
		audio_tstamp_report->accuracy_report = 1;
		/* 24 MHz WallClock == 42ns resolution */
		audio_tstamp_report->accuracy = 42;

	} else {
		audio_tstamp_report->actual_type = SNDRV_PCM_AUDIO_TSTAMP_TYPE_DEFAULT;
	}

	return 0;
}