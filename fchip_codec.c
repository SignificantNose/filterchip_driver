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

	// First try to probe all given codec slots
	for (c = 0; c < max_slots; c++) {
		if ((bus->codec_mask & (1 << c)) & fchip_azx->codec_probe_mask) {
			if (probe_codec(fchip_azx, c) < 0) {
				
				// Some BIOSen give you wrong codec addresses
				// that don't exist
				printk(KERN_WARNING "fchip: Codec #%d probe error; disabling it...\n", c);
				bus->codec_mask &= ~(1 << c);
				// no codecs
				if (bus->codec_mask == 0){
					break;
				}
				// More badly, accessing to a non-existing
				// codec often screws up the controller chip,
				// and disturbs the further communications.
				// Thus if an error occurs during probing,
				// better to reset the controller chip to
				// get back to the sanity state.
				fchip_stop_chip(fchip_azx);
				fchip_init_chip(fchip_azx, true);
			}
		}
	}

	// Then create codec instances
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


static void fchip_codec_rewrite_ops(struct snd_pcm_ops *new_ops, struct snd_pcm_ops *defined_ops){
	new_ops->pointer = defined_ops->pointer;
	new_ops->open = defined_ops->open;
	new_ops->close = defined_ops->close;
	new_ops->hw_params = defined_ops->hw_params;
	new_ops->hw_free = defined_ops->hw_free;
	new_ops->prepare = defined_ops->prepare;
	new_ops->trigger = defined_ops->trigger;
	new_ops->pointer = defined_ops->pointer;
	new_ops->get_time_info = defined_ops->get_time_info;
	
	new_ops->open = fchip_pcm_open;
	new_ops->pointer = fchip_pcm_pointer;
	new_ops->close = fchip_pcm_close;
	new_ops->hw_params = fchip_pcm_hw_params;
	new_ops->hw_free = fchip_pcm_hw_free;
	new_ops->prepare = fchip_pcm_prepare;
	new_ops->trigger = fchip_pcm_trigger;
	new_ops->get_time_info = fchip_pcm_get_time_info;
}

int fchip_codec_configure(struct fchip_azx* fchip_azx)
{
	struct hda_codec* codec, * next;
	struct hda_pcm* codec_pcm;
	struct fchip_runtime_pr *runtime_pr;
	struct snd_pcm_str *stream;
	struct snd_pcm_substream *substream;
	int success = 0;

	static struct snd_pcm_ops myops = {};
	static bool ops_redefined = false;

	list_for_each_codec(codec, &fchip_azx->bus) {
		if (!snd_hda_codec_configure(codec)){
			success++;
		
			list_for_each_entry(codec_pcm, &codec->pcm_list_head, list) {
				for (int dir = 0; dir < 2; dir++) {
					if (codec_pcm->stream[dir].substreams){
						// snd_pcm_set_ops(codec_pcm->pcm, s, &azx_pcm_ops);
						
						// a dirty-dirty approach. the goal is to override one operation,
						// while the ops field of the substream is a const field.
						stream = &codec_pcm->pcm->streams[dir];
						for (substream = stream->substream; substream != NULL; substream = substream->next){
							if(!ops_redefined){
								fchip_codec_rewrite_ops(&myops, substream->ops);
								ops_redefined = true;
							}

							substream->ops = &myops;
						}
					}
				}
			}
		}
	}

	if (success) {
		// unregister failed codecs if any codec has been probed
		list_for_each_codec_safe(codec, next, &fchip_azx->bus) {
			if (!codec->configured) {
				printk(KERN_ERR "fchip: Unable to configure codec #%d, disabling\n", codec->addr);
				snd_hdac_device_unregister(&codec->core);
			}
		}
	}

	return success ? 0 : -ENODEV;
}
