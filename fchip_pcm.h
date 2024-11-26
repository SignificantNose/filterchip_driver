#pragma once
#include "fchip.h"
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#define FILTERCHIP_PCM_BUFFER_SIZE (64*1024)

// WARNING: a slippery thing. It is not static, therefore
// it might collide with other functions with the same 
// name (which is not very likely, with such a name)
int snd_filterchip_new_pcm(struct fchip* chip); 