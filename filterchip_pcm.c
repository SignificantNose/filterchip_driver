#include "filterchip_pcm.h"
#include "filterchip.h"

static struct snd_pcm_hardware snd_filterchip_playback_hw = {
    .info = (SNDRV_PCM_INFO_MMAP |
                SNDRV_PCM_INFO_INTERLEAVED |
                SNDRV_PCM_INFO_BLOCK_TRANSFER |
                SNDRV_PCM_INFO_MMAP_VALID),
                /*
				  SNDRV_PCM_INFO_RESUME | // not fully implemented?
				 SNDRV_PCM_INFO_PAUSE |
				 SNDRV_PCM_INFO_SYNC_START |
				 SNDRV_PCM_INFO_HAS_WALL_CLOCK | // legacy
				 SNDRV_PCM_INFO_HAS_LINK_ATIME |
				 SNDRV_PCM_INFO_NO_PERIOD_WAKEUP),
                 */
    .formats =          SNDRV_PCM_FMTBIT_S16_LE,
    .rates =            SNDRV_PCM_RATE_48000,
    .rate_min =         48000,
    .rate_max =         48000,
    .channels_min =     2,
    .channels_max =     2,
    .buffer_bytes_max = 32768,  // 4*1024*1024
    .period_bytes_min = 4096,   // 128
    .period_bytes_max = 32768,  // 2*1024*1024
    .periods_min =      1,      // 2
    .periods_max =      1024,   // 32 in hda_controller
}; 

static int snd_filterchip_playback_open(struct snd_pcm_substream* substream)
{
    struct snd_pcm_runtime* runtime = substream->runtime;
    runtime->hw = snd_filterchip_playback_hw;
    
    return 0;
}

static int snd_filterchip_playback_close(struct snd_pcm_substream* substream)
{
    return 0;
}

static int snd_filterchip_pcm_hw_params(struct snd_pcm_substream* substream, struct snd_pcm_hw_params* hw_params)
{
    return snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
}

static int snd_filterchip_pcm_hw_free(struct snd_pcm_substream* substream)
{
    return snd_pcm_lib_free_pages(substream);
}

static int snd_filterchip_pcm_prepare(struct snd_pcm_substream* substream){
    return 0;
}

static int snd_filterchip_pcm_trigger(struct snd_pcm_substream* substream, int cmd){
    return 0;
}

static snd_pcm_uframes_t snd_filterchip_pcm_pointer(struct snd_pcm_substream* substream){
    return 0;
}



static struct snd_pcm_ops snd_filterchip_pcm_ops = {
    .open =         snd_filterchip_playback_open,
    .close =        snd_filterchip_playback_close,
    .ioctl =        snd_pcm_lib_ioctl,
    .hw_params =    snd_filterchip_pcm_hw_params,
    .hw_free =      snd_filterchip_pcm_hw_free,
    .prepare =      snd_filterchip_pcm_prepare,
    .trigger =      snd_filterchip_pcm_trigger,
    .pointer =      snd_filterchip_pcm_pointer
};

static int snd_filterchip_new_pcm(struct filterchip* chip)
{
    struct snd_pcm* pcm;
    int err;   
    
    // first 0 for PCM number, as only one PCM instance is expected
    err = snd_pcm_new(chip->card, FCHIP_DRIVER_NAME, 0, 1, 0, &pcm);
    if (err<0){
        printk(KERN_ERR "filterchip: failed to create new PCM instance: %d", err);
        return err;
    }

    pcm->private_data = chip;
    strcpy(pcm->name, FCHIP_DRIVER_NAME);
    chip->pcm = pcm;
    
    snd_pcm_set_ops(
        pcm, SNDRV_PCM_STREAM_PLAYBACK,
        &snd_filterchip_pcm_ops
    );

    snd_pcm_lib_preallocate_pages_for_all(
        pcm, SNDRV_DMA_TYPE_DEV, &chip->pci->dev, 
        FILTERCHIP_PCM_BUFFER_SIZE, FILTERCHIP_PCM_BUFFER_SIZE
    );

    // if there is any private data that must be defined 
    // for a PCM device, allocate it here and free in the
    // dtor, that you also must define yourself.

    return 0;
}