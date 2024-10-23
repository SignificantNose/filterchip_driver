#include "filterchip.h"
#include "filterchip_pcm.h"

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
    printk(KERN_DEBUG "fchip: Interrupt handler called\n");
    struct filterchip* chip = dev_id;
    // todo
    return IRQ_HANDLED;
}

// CHIP dtor
static int snd_filterchip_free(struct filterchip* chip)
{
    printk(KERN_DEBUG "fchip: Chip free called\n");

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
    printk(KERN_DEBUG "fchip: Device free called\n");
    return snd_filterchip_free(device->device_data);
}

// CHIP ctor
static int snd_filterchip_create(
    struct snd_card* card,
    struct pci_dev* pci, 
    struct filterchip** rchip
)
{
    printk(KERN_DEBUG "fchip: Chip create called\n");
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
    printk(KERN_DEBUG "fchip: probe called on device %x:%x\n", pci_id->vendor, pci_id->device);
    static int dev;
    struct snd_card* card;
    struct filterchip* chip;
    int err;

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
    printk(KERN_DEBUG "fchip: Chip remove called\n");
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
    printk(KERN_DEBUG "fchip: init called\n");
    return pci_register_driver(&driver);
}

static void __exit alsa_card_filterchip_exit(void){
    printk(KERN_DEBUG "fchip: exit called\n");
    pci_unregister_driver(&driver);
}

module_init(alsa_card_filterchip_init)
module_exit(alsa_card_filterchip_exit)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Significant Nose");
MODULE_DESCRIPTION("ALSA compliant audio driver with a filtering function");