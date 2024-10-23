#pragma once
#include "filterchip.h"
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#define FILTERCHIP_PCM_BUFFER_SIZE (64*1024)

static int snd_filterchip_new_pcm(struct filterchip* chip); 