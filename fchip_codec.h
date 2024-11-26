#pragma once
#include "fchip.h"

int fchip_probe_codecs(struct fchip_azx *chip, unsigned int max_slots);
int fchip_codec_configure(struct fchip_azx *chip);
