#pragma once
#include "fchip.h"

#define AZX_DEFAULT_CODECS	4


struct azx_pcm {
	struct fchip_azx *chip;
	struct snd_pcm *pcm;
	struct hda_codec *codec;
	struct hda_pcm *info;
	struct list_head list;
};

int fchip_probe_codecs(struct fchip_azx *chip, unsigned int max_slots);
int fchip_codec_configure(struct fchip_azx *chip);
