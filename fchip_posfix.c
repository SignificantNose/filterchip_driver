#include "fchip_posfix.h"

// allow-deny list for position fix
const struct snd_pci_quirk position_fix_list[] = {
	SND_PCI_QUIRK(0x1028, 0x01cc, "Dell D820", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x1028, 0x01de, "Dell Precision 390", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x103c, 0x306d, "HP dv3", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x1043, 0x813d, "ASUS P5AD2", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x1043, 0x81b3, "ASUS", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x1043, 0x81e7, "ASUS M2V", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x104d, 0x9069, "Sony VPCS11V9E", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x10de, 0xcb89, "Macbook Pro 7,1", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x1297, 0x3166, "Shuttle", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x1458, 0xa022, "ga-ma770-ud3", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x1462, 0x1002, "MSI Wind U115", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x1565, 0x8218, "Biostar Microtech", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x1849, 0x0888, "775Dual-VSTA", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x8086, 0x2503, "DG965OT AAD63733-203", POS_FIX_LPIB),
	{}
};

unsigned int fchip_get_pos_lpib(struct fchip_azx *chip, struct azx_dev *azx_dev)
{
	return snd_hdac_stream_get_pos_lpib(azx_dev_to_hdac_stream(azx_dev));
}

unsigned int fchip_get_pos_posbuf(struct fchip_azx *chip, struct azx_dev *azx_dev)
{
	return snd_hdac_stream_get_pos_posbuf(azx_dev_to_hdac_stream(azx_dev));
}

static unsigned int fchip_via_get_position(struct fchip_azx *chip,
					 struct azx_dev *azx_dev)
{
	unsigned int link_pos, mini_pos, bound_pos;
	unsigned int mod_link_pos, mod_dma_pos, mod_mini_pos;
	unsigned int fifo_size;

	link_pos = snd_hdac_stream_get_pos_lpib(azx_dev_to_hdac_stream(azx_dev));
	if (azx_dev->core.substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		// Playback, no problem using link position
		return link_pos;
	}

	// Capture 
	// For new chipset,
	// use mod to get the DMA position just like old chipset
	// 
	mod_dma_pos = le32_to_cpu(*azx_dev->core.posbuf);
	mod_dma_pos %= azx_dev->core.period_bytes;

	fifo_size = azx_dev_to_hdac_stream(azx_dev)->fifo_size;

	if (azx_dev->insufficient) {
		// Link position never gather than FIFO size 
		if (link_pos <= fifo_size){
			return 0;
        }

		azx_dev->insufficient = 0;
	}

	if (link_pos <= fifo_size){
		mini_pos = azx_dev->core.bufsize + link_pos - fifo_size;
    }
	else{
		mini_pos = link_pos - fifo_size;    
    }

	// Find nearest previous boudary
	mod_mini_pos = mini_pos % azx_dev->core.period_bytes;
	mod_link_pos = link_pos % azx_dev->core.period_bytes;
	if (mod_link_pos >= fifo_size){
		bound_pos = link_pos - mod_link_pos;
    }
	else if (mod_dma_pos >= mod_mini_pos){
		bound_pos = mini_pos - mod_mini_pos;
    }
	else {
		bound_pos = mini_pos - mod_mini_pos + azx_dev->core.period_bytes;
		if (bound_pos >= azx_dev->core.bufsize){
			bound_pos = 0;
        }
	}

	// Calculate real DMA position we want
	return bound_pos + mod_dma_pos;
}

static unsigned int fchip_get_pos_fifo(struct fchip_azx *chip, struct azx_dev *azx_dev)
{
	struct snd_pcm_substream *substream = azx_dev->core.substream;
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int pos, delay;

	pos = snd_hdac_stream_get_pos_lpib(azx_dev_to_hdac_stream(azx_dev));
	if (!runtime){
		return pos;
    }

	runtime->delay = AMD_FIFO_SIZE;
	delay = frames_to_bytes(runtime, AMD_FIFO_SIZE);
	if (azx_dev->insufficient) {
		if (pos < delay) {
			delay = pos;
			runtime->delay = bytes_to_frames(runtime, pos);
		} else {
			azx_dev->insufficient = 0;
		}
	}

	// correct the DMA position for capture stream
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		if (pos < delay){
			pos += azx_dev->core.bufsize;
        }
		pos -= delay;
	}

	return pos;
}

static int fchip_get_delay_from_fifo(struct fchip_azx *chip, struct azx_dev *azx_dev,
				   unsigned int pos)
{
	struct snd_pcm_substream *substream = azx_dev->core.substream;

	// just read back the calculated value in the above 
	return substream->runtime->delay;
}

static int fchip_get_delay_from_lpib(struct fchip_azx *chip, struct azx_dev *azx_dev,
				   unsigned int pos)
{
	struct snd_pcm_substream *substream = azx_dev->core.substream;
	int stream = substream->stream;
	unsigned int lpib_pos = fchip_get_pos_lpib(chip, azx_dev);
	int delay;

	if (stream == SNDRV_PCM_STREAM_PLAYBACK){
		delay = pos - lpib_pos;
    }
	else{
		delay = lpib_pos - pos;
    }
	if (delay < 0) {
		if (delay >= azx_dev->core.delay_negative_threshold){
			delay = 0;
        }
		else{
			delay += azx_dev->core.bufsize;
        }
	}

	if (delay >= azx_dev->core.period_bytes) {
		printk(KERN_INFO "fchip: Unstable LPIB (%d >= %d); disabling LPIB delay counting\n",
			 delay, azx_dev->core.period_bytes);
		delay = 0;
		chip->driver_caps &= ~AZX_DCAPS_COUNT_LPIB_DELAY;
		chip->get_delay[stream] = NULL;
	}

	return bytes_to_frames(substream->runtime, delay);
}

void assign_position_fix(struct fchip_azx *chip, int fix)
{
    static const azx_get_pos_callback_t callbacks[] = {
		[POS_FIX_AUTO] = NULL,
		[POS_FIX_LPIB] = fchip_get_pos_lpib,
		[POS_FIX_POSBUF] = fchip_get_pos_posbuf,
		[POS_FIX_VIACOMBO] = fchip_via_get_position,
		[POS_FIX_COMBO] = fchip_get_pos_lpib,
		[POS_FIX_SKL] = fchip_get_pos_posbuf,
		[POS_FIX_FIFO] = fchip_get_pos_fifo,
	};

	chip->get_position[0] = chip->get_position[1] = callbacks[fix];

	// combo mode uses LPIB only for playback
	if (fix == POS_FIX_COMBO){
		chip->get_position[1] = NULL;
    }

	if ((fix == POS_FIX_POSBUF || fix == POS_FIX_SKL) &&
	    (chip->driver_caps & AZX_DCAPS_COUNT_LPIB_DELAY)) {
		chip->get_delay[0] = chip->get_delay[1] = fchip_get_delay_from_lpib;
	}

	if (fix == POS_FIX_FIFO)
		chip->get_delay[0] = chip->get_delay[1] = fchip_get_delay_from_fifo;
}




int check_position_fix(struct fchip_azx *chip, int fix)
{
    const struct snd_pci_quirk *q;

	switch (fix) {
	case POS_FIX_AUTO:
	case POS_FIX_LPIB:
	case POS_FIX_POSBUF:
	case POS_FIX_VIACOMBO:
	case POS_FIX_COMBO:
	case POS_FIX_SKL:
	case POS_FIX_FIFO:
		return fix;
	}

	q = snd_pci_quirk_lookup(chip->pci, position_fix_list);
	if (q) {
		printk(KERN_INFO "fchip: position_fix set to %d for device %04x:%04x\n",
			 q->value, q->subvendor, q->subdevice);
		return q->value;
	}

	// Check VIA/ATI HD Audio Controller exist
	if (chip->driver_type == AZX_DRIVER_VIA) {
		printk(KERN_DEBUG "fchip: Using VIACOMBO position fix\n");
		return POS_FIX_VIACOMBO;
	}
	if (chip->driver_caps & AZX_DCAPS_AMD_WORKAROUND) {
		printk(KERN_DEBUG "fchip: Using FIFO position fix\n");
		return POS_FIX_FIFO;
	}
	if (chip->driver_caps & AZX_DCAPS_POSFIX_LPIB) {
		printk(KERN_DEBUG "fchip: Using LPIB position fix\n");
		return POS_FIX_LPIB;
	}
	if (chip->driver_type == AZX_DRIVER_SKL) {
		printk(KERN_DEBUG "fchip: Using SKL position fix\n");
		return POS_FIX_SKL;
	}
	return POS_FIX_AUTO;
}

/*
 * Check whether the current DMA position is acceptable for updating
 * periods.  Returns non-zero if it's OK.
 *
 * Many HD-audio controllers appear pretty inaccurate about
 * the update-IRQ timing.  The IRQ is issued before actually the
 * data is processed.  So, we need to process it afterwords in a
 * workqueue.
 *
 * Returns 1 if OK to proceed, 0 for delay handling, -1 for skipping update
 */
int fchip_position_ok(struct fchip_azx* fchip_azx, struct azx_dev* azx_dev)
{
	struct snd_pcm_substream *substream = azx_dev->core.substream;
	struct snd_pcm_runtime *runtime = substream->runtime;
	int stream = substream->stream;
	u32 wallclk;
	unsigned int pos;
	snd_pcm_uframes_t hwptr, target;

	// The value of the WALLCLK register is always 0
	// on the Loongson controller, so we return directly. 
	if (fchip_azx->driver_type == AZX_DRIVER_LOONGSON){
		return 1;
	}

	wallclk = fchip_readreg_l(fchip_azx, WALLCLK) - azx_dev->core.start_wallclk;
	if (wallclk < (azx_dev->core.period_wallclk * 2) / 3){
		return -1;	// bogus (too early) interrupt
	}

	if (fchip_azx->get_position[stream]){
		pos = fchip_azx->get_position[stream](fchip_azx, azx_dev);
	}
	else { // use the position buffer as default
		pos = fchip_get_pos_posbuf(fchip_azx, azx_dev);
		if (!pos || pos == (u32)-1) {
			printk(KERN_INFO "fchip: Invalid position buffer, using LPIB read method instead.\n");
			fchip_azx->get_position[stream] = fchip_get_pos_lpib;
			if (fchip_azx->get_position[0] == fchip_get_pos_lpib &&
			    fchip_azx->get_position[1] == fchip_get_pos_lpib)
			{
				azx_to_hda_bus(fchip_azx)->use_posbuf = false;
			}

			pos = fchip_get_pos_lpib(fchip_azx, azx_dev);
			fchip_azx->get_delay[stream] = NULL;
		} 
		else {
			fchip_azx->get_position[stream] = fchip_get_pos_posbuf;
			if (fchip_azx->driver_caps & AZX_DCAPS_COUNT_LPIB_DELAY)
			{
				fchip_azx->get_delay[stream] = fchip_get_delay_from_lpib;
			}
		}
	}

	if (pos >= azx_dev->core.bufsize){
		pos = 0;
	}

	if (WARN_ONCE(!azx_dev->core.period_bytes,
		      "hda-intel: zero azx_dev->period_bytes"))
		return -1; // this shouldn't happen!
	if (wallclk < (azx_dev->core.period_wallclk * 5) / 4 &&
	    pos % azx_dev->core.period_bytes > azx_dev->core.period_bytes / 2)
	{
		// NG - it's below the first next period boundary
		return fchip_azx->bdl_pos_adj ? 0 : -1;
	}
	azx_dev->core.start_wallclk += wallclk;

	if (azx_dev->core.no_period_wakeup){
		return 1; // OK, no need to check period boundary
	}

	if (runtime->hw_ptr_base != runtime->hw_ptr_interrupt){
		return 1; // OK, already in hwptr updating process
	}

	// check whether the period gets really elapsed
	pos = bytes_to_frames(runtime, pos);
	hwptr = runtime->hw_ptr_base + pos;
	
	if (hwptr < runtime->status->hw_ptr){
		hwptr += runtime->buffer_size;
	}

	target = runtime->hw_ptr_interrupt + runtime->period_size;
	if (hwptr < target) {
		// too early wakeup, process it later
		return fchip_azx->bdl_pos_adj ? 0 : -1;
	}

	return 1; // OK, it's fine
}
