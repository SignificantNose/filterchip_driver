#include "filterchip.h"

#pragma region PCM_HEADER
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#define FILTERCHIP_PCM_BUFFER_SIZE (64*1024)

static int snd_filterchip_new_pcm(struct filterchip* chip); 
#pragma endregion

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char* id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

// for now, I only define my soundcard in the ID table
static struct pci_device_id snd_filterchip_idtable[] = {
    { FCHIP_VENDOR_ID, FCHIP_DEVICE_ID, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0, },
    { 0,}
};



// interrupt handler 
static irqreturn_t snd_filterchip_interrupt(int irq, void* dev_id){
    struct filterchip* chip = dev_id;
    // todo
    return IRQ_HANDLED;
}

// CHIP dtor
static int snd_filterchip_free(struct filterchip* chip)
{
    if(chip->irq >= 0){
        free_irq(chip->irq, chip);
    }

    pci_release_regions(chip->pci);
    pci_disable_device(chip->pci);
    kfree(chip);
    return 0;
}

// COMPONENT dtor
static int snd_filterchip_dev_free(struct snd_device* device)
{
    return snd_filterchip_free(device->device_data);
}

// CHIP ctor
static int snd_filterchip_create(
    struct snd_card* card,
    struct pci_dev* pci, 
    struct filterchip** rchip
)
{
    static struct snd_device_ops ops = {
        .dev_free = snd_filterchip_dev_free,
    };
    struct filterchip* chip;
    int err;
    int dma_bits;

    *rchip = NULL;


    // initializing the pci entry
    err = pci_enable_device(pci);
    if(err<0){
        return err;
    }
    // checking pci availability
    dma_bits = FCHIP_DMA_MASK_BITS;
    if(
        dma_set_mask(&pci->dev, DMA_BIT_MASK(dma_bits))<0 ||
        dma_set_coherent_mask(&pci->dev, DMA_BIT_MASK(dma_bits))<0)
    {
        printk(KERN_ERR "FChip: error setting %dbit DMA mask\n", dma_bits);
        pci_disable_device(pci);
        return -ENXIO;
    }

    chip = kzalloc(sizeof(*chip), GFP_KERNEL);
    if(chip==NULL){
        pci_disable_device(pci);
        return -ENOMEM;
    }

    chip->card = card;
    chip->pci = pci;
    chip->irq = -1;

    // resource allocation
    // 1. i/o port
    err = pci_request_regions(pci, FCHIP_DRIVER_NAME);
    if(err<0){
        kfree(chip);
        pci_disable_device(pci);
        return err;
    }
    chip->port = pci_resource_start(pci, 0);
    
    // 2. interrupt source
    if(request_irq(pci->irq, snd_filterchip_interrupt, IRQF_SHARED, KBUILD_MODNAME, chip)){
        printk(KERN_ERR "FCHIP: cannot grab irq %d\n", pci->irq);
        snd_filterchip_free(chip);
        return -EBUSY;
    }
    chip->irq = pci->irq;

    err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, chip, &ops);
    if(err<0){
        snd_filterchip_free(chip);
        return err;
    }

    *rchip = chip;
    return 0;
}


// DRIVER ctor
static int snd_filterchip_probe(
    struct pci_dev* pci,
    const struct pci_device_id* pci_id
)
{
    static int dev;
    struct snd_card* card;
    struct filterchip* chip;
    int err;

    printk(KERN_INFO "filterchip: probe called on device %x:%x\n", pci_id->vendor, pci_id->device);
    // if there's going to be one card, I think it's redundant? 
    // and the whole probe will be called only once?
    // but for now, leave it this way.
    if(dev>=SNDRV_CARDS){
        return -ENODEV;
    }
    if(!enable[dev]){
        dev++;
        return -ENOENT;
    }

    err = snd_card_new(&pci->dev, index[dev], id[dev], THIS_MODULE, 0, &card);
    if(err<0){
        return err;
    }


    err = snd_filterchip_create(card, pci, &chip);
    if(err<0){
        snd_card_free(card);
        return err;
    }

    err = snd_filterchip_new_pcm(chip);
    if(err<0){
        snd_card_free(card);
        return err;
    }

    strcpy(card->driver, FCHIP_DRIVER_NAME);
    strcpy(card->shortname, FCHIP_DRIVER_SHORTNAME);
    sprintf(card->longname, "%s at 0x%lx irq %i", card->shortname, chip->port, chip->irq);

    // todo later (PCM goes here?)

    err = snd_card_register(card);
    if(err<0){
        snd_card_free(card);
        return err;
    }

    // we do it, so that later we can free the chip, 
    // as only pci_dev struct is passed to the dtor.
    // that way, we're able to free the card by acquiring
    // it from the drvdata of the pci_dev struct
    pci_set_drvdata(pci, card);
    dev++;
    return 0;
}

// CHIP dtor
static void snd_filterchip_remove(struct pci_dev* pci){
    snd_card_free(pci_get_drvdata(pci));
    pci_set_drvdata(pci, NULL);
}



// driver module definition
static struct pci_driver driver = {
    .name = KBUILD_MODNAME,
    .id_table = snd_filterchip_idtable,
    .probe = snd_filterchip_probe,
    .remove = snd_filterchip_remove,
};

static int __init alsa_card_filterchip_init(void){
    printk(KERN_INFO "filterchip: im here!\n");
    return pci_register_driver(&driver);
}

static void __exit alsa_card_filterchip_exit(void){
    printk(KERN_INFO "filterchip: im outta here!\n");
    pci_unregister_driver(&driver);
}

module_init(alsa_card_filterchip_init)
module_exit(alsa_card_filterchip_exit)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Significant Nose");
MODULE_DESCRIPTION("ALSA compliant audio driver with a filtering function");


#pragma region PCM_SOURCE
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
#pragma endregion