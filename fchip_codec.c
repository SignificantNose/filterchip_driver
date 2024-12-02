#include "fchip_codec.h"
#include "fchip_hda_bus.h"
#include "fchip_pcm.h"

static inline struct hda_pcm_stream *
to_hda_pcm_stream(struct snd_pcm_substream *substream)
{
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	return &apcm->info->stream[substream->stream];
}

static inline struct azx_dev *get_azx_dev(struct snd_pcm_substream *substream)
{
	return substream->runtime->private_data;
}

static int probe_codec(struct fchip_azx* fchip_azx, int addr)
{
	unsigned int cmd = (addr << 28) | (AC_NODE_ROOT << 20) |
		(AC_VERB_PARAMETERS << 8) | AC_PAR_VENDOR_ID;
	struct hdac_bus *bus = azx_to_hda_bus(fchip_azx);
	int err;
	unsigned int res = -1;

	mutex_lock(&bus->cmd_mutex);
	fchip_azx->probing = 1;
	fchip_send_cmd(bus, cmd);
	err = fchip_get_response(bus, addr, &res);
	fchip_azx->probing = 0;
	mutex_unlock(&bus->cmd_mutex);
	
	if (err < 0 || res == -1){
		return -EIO;
	}

	printk(KERN_DEBUG "fchip: Codec #%d probed OK\n", addr);
	return 0;
}

int fchip_probe_codecs(struct fchip_azx* fchip_azx, unsigned int max_slots)
{
	struct hdac_bus *bus = azx_to_hda_bus(fchip_azx);
	int c, codecs, err;

	codecs = 0;
	if (!max_slots){
		max_slots = AZX_DEFAULT_CODECS;
	}

	/* First try to probe all given codec slots */
	for (c = 0; c < max_slots; c++) {
		if ((bus->codec_mask & (1 << c)) & fchip_azx->codec_probe_mask) {
			if (probe_codec(fchip_azx, c) < 0) {
				/* Some BIOSen give you wrong codec addresses
				 * that don't exist
				 */
				printk(KERN_WARNING "fchip: Codec #%d probe error; disabling it...\n", c);
				bus->codec_mask &= ~(1 << c);
				/* no codecs */
				if (bus->codec_mask == 0){
					break;
				}
				/* More badly, accessing to a non-existing
				 * codec often screws up the controller chip,
				 * and disturbs the further communications.
				 * Thus if an error occurs during probing,
				 * better to reset the controller chip to
				 * get back to the sanity state.
				 */
				fchip_stop_chip(fchip_azx);
				fchip_init_chip(fchip_azx, true);
			}
		}
	}

	/* Then create codec instances */
	for (c = 0; c < max_slots; c++) {
		if ((bus->codec_mask & (1 << c)) & fchip_azx->codec_probe_mask) {
			struct hda_codec *codec;
			err = snd_hda_codec_new(&fchip_azx->bus, fchip_azx->card, c, &codec);
			if (err < 0){
				continue;
			}

			codec->jackpoll_interval = fchip_azx->jackpoll_interval;
			codec->beep_mode = fchip_azx->beep_mode;
			codec->ctl_dev_id = fchip_azx->ctl_dev_id;
			codecs++;
		}
	}
	if (!codecs) {
		printk(KERN_ERR "fchip: No codecs initialized\n");
		return -ENXIO;
	}
	return 0;
}

// int coppy(struct snd_pcm_substream *substream, int channel,
// 		    unsigned long pos, struct iov_iter *iter, unsigned long bytes)
// {
// 	printk(KERN_INFO "fchip: copy called\n");
// 	return 0;	
// }
// snd_pcm_uframes_t ppointer(struct snd_pcm_substream *substream){
// 	printk(KERN_INFO "fchip: pointer called\n");
// 	return 0;
// }

// int my_ack(struct snd_pcm_substream *substream){
// 	printk(KERN_INFO "fchip: ack called\n");
// 	return 0;
// }



int fchip_codec_configure(struct fchip_azx* fchip_azx)
{
	struct hda_codec* codec, * next;
	struct hda_pcm* codec_pcm;
	struct fchip_runtime_pr *runtime_pr;
	// struct azx_dev *dev;
	int success = 0;

	static struct snd_pcm_ops myops = {};

	list_for_each_codec(codec, &fchip_azx->bus) {
		if (!snd_hda_codec_configure(codec)){
			success++;
		
			list_for_each_entry(codec_pcm, &codec->pcm_list_head, list) {
				for (int dir = 0; dir < 2; dir++) {
					if (codec_pcm->stream[dir].substreams){
						// snd_pcm_set_ops(codec_pcm->pcm, s, &azx_pcm_ops);
						
						// a dirty-dirty approach. the goal is to override one operation,
						// while the ops field of the substream is a const field.
						struct snd_pcm_str* stream = &codec_pcm->pcm->streams[dir];
						struct snd_pcm_substream* substream;
						for (substream = stream->substream; substream != NULL; substream = substream->next){
							myops.pointer = substream->ops->pointer;
							myops.open = substream->ops->open;
							myops.close = substream->ops->close;
							myops.hw_params = substream->ops->hw_params;
							myops.hw_free = substream->ops->hw_free;
							myops.prepare = substream->ops->prepare;
							myops.trigger = substream->ops->trigger;
							myops.pointer = substream->ops->pointer;
							myops.get_time_info = substream->ops->get_time_info;
							
							myops.open = fchip_pcm_open;
							myops.pointer = fchip_pcm_pointer;
							myops.close = fchip_pcm_close;
							myops.hw_params = fchip_pcm_hw_params;
							myops.hw_free = fchip_pcm_hw_free;
							myops.prepare = fchip_pcm_prepare;
							myops.trigger = fchip_pcm_trigger;
							myops.get_time_info = fchip_pcm_get_time_info;

							// runtime_pr = kzalloc(sizeof(struct fchip_runtime_pr), GFP_KERNEL);
							// // dev = substream->runtime->private_data;
							// printk(KERN_INFO "fchip: before copy\n");
							// printk(KERN_INFO "fchip: address here\n");
							// memcpy(runtime_pr, substream->runtime->private_data, sizeof(struct azx_dev));
							// printk(KERN_INFO "fchip: after copy\n");
							
							// // allocate filter
							// runtime_pr->filter_ptr = 0;
							// substream->runtime->private_data = runtime_pr;
							
							
							// myops.copy = coppy;
							// myops.pointer = ppointer;
							// myops.ack = my_ack;


							substream->ops = &myops;
							printk(KERN_INFO "fchip: override occurred\n");
							// substream->ops->copy = coppy;
						}
					}
				}
				// if (cpcm->pcm)
				// 	continue; /* already attached */
				// if (!cpcm->stream[0].substreams && !cpcm->stream[1].substreams)
				// 	continue; /* no substreams assigned */

				// dev = get_empty_pcm_device(bus, cpcm->pcm_type);
				// if (dev < 0) {
				// 	cpcm->device = SNDRV_PCM_INVALID_DEVICE;
				// 	continue; /* no fatal error */
				// }
				// cpcm->device = dev;
				// err =  snd_hda_attach_pcm_stream(bus, codec, cpcm);
				// if (err < 0) {
				// 	codec_err(codec,
				// 		"cannot attach PCM stream %d for codec #%d\n",
				// 		dev, codec->core.addr);
				// 	continue; /* no fatal error */
				// }
			}
			
		}
	}

	if (success) {
		/* unregister failed codecs if any codec has been probed */
		list_for_each_codec_safe(codec, next, &fchip_azx->bus) {
			if (!codec->configured) {
				printk(KERN_ERR "fchip: Unable to configure codec #%d, disabling\n", codec->addr);
				snd_hdac_device_unregister(&codec->core);
			}
		}
	}

	return success ? 0 : -ENODEV;
}
