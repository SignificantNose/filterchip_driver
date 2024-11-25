#include "fchip.h"
#include "fchip_pcm.h"

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char* id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

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

    // allocate memory for private data here
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



/* PCI IDs */
static const struct pci_device_id fchip_idtable[] = {
	/* CPT */
	{ PCI_DEVICE_DATA(INTEL, HDA_CPT, AZX_DRIVER_PCH | AZX_DCAPS_INTEL_PCH_NOPM) },
	/* PBG */
	{ PCI_DEVICE_DATA(INTEL, HDA_PBG, AZX_DRIVER_PCH | AZX_DCAPS_INTEL_PCH_NOPM) },
	/* Panther Point */
	{ PCI_DEVICE_DATA(INTEL, HDA_PPT, AZX_DRIVER_PCH | AZX_DCAPS_INTEL_PCH_NOPM) },
	/* Lynx Point */
	{ PCI_DEVICE_DATA(INTEL, HDA_LPT, AZX_DRIVER_PCH | AZX_DCAPS_INTEL_PCH) },
	/* 9 Series */
	{ PCI_DEVICE_DATA(INTEL, HDA_9_SERIES, AZX_DRIVER_PCH | AZX_DCAPS_INTEL_PCH) },
	/* Wellsburg */
	{ PCI_DEVICE_DATA(INTEL, HDA_WBG_0, AZX_DRIVER_PCH | AZX_DCAPS_INTEL_PCH) },
	{ PCI_DEVICE_DATA(INTEL, HDA_WBG_1, AZX_DRIVER_PCH | AZX_DCAPS_INTEL_PCH) },
	/* Lewisburg */
	{ PCI_DEVICE_DATA(INTEL, HDA_LBG_0, AZX_DRIVER_PCH | AZX_DCAPS_INTEL_SKYLAKE) },
	{ PCI_DEVICE_DATA(INTEL, HDA_LBG_1, AZX_DRIVER_PCH | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Lynx Point-LP */
	{ PCI_DEVICE_DATA(INTEL, HDA_LPT_LP_0, AZX_DRIVER_PCH | AZX_DCAPS_INTEL_PCH) },
	/* Lynx Point-LP */
	{ PCI_DEVICE_DATA(INTEL, HDA_LPT_LP_1, AZX_DRIVER_PCH | AZX_DCAPS_INTEL_PCH) },
	/* Wildcat Point-LP */
	{ PCI_DEVICE_DATA(INTEL, HDA_WPT_LP, AZX_DRIVER_PCH | AZX_DCAPS_INTEL_PCH) },
	/* Skylake (Sunrise Point) */
	{ PCI_DEVICE_DATA(INTEL, HDA_SKL, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Skylake-LP (Sunrise Point-LP) */
	{ PCI_DEVICE_DATA(INTEL, HDA_SKL_LP, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Kabylake */
	{ PCI_DEVICE_DATA(INTEL, HDA_KBL, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Kabylake-LP */
	{ PCI_DEVICE_DATA(INTEL, HDA_KBL_LP, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Kabylake-H */
	{ PCI_DEVICE_DATA(INTEL, HDA_KBL_H, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Coffelake */
	{ PCI_DEVICE_DATA(INTEL, HDA_CNL_H, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Cannonlake */
	{ PCI_DEVICE_DATA(INTEL, HDA_CNL_LP, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* CometLake-LP */
	{ PCI_DEVICE_DATA(INTEL, HDA_CML_LP, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* CometLake-H */
	{ PCI_DEVICE_DATA(INTEL, HDA_CML_H, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	{ PCI_DEVICE_DATA(INTEL, HDA_RKL_S, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* CometLake-S */
	{ PCI_DEVICE_DATA(INTEL, HDA_CML_S, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* CometLake-R */
	{ PCI_DEVICE_DATA(INTEL, HDA_CML_R, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Icelake */
	{ PCI_DEVICE_DATA(INTEL, HDA_ICL_LP, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Icelake-H */
	{ PCI_DEVICE_DATA(INTEL, HDA_ICL_H, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Jasperlake */
	{ PCI_DEVICE_DATA(INTEL, HDA_ICL_N, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	{ PCI_DEVICE_DATA(INTEL, HDA_JSL_N, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Tigerlake */
	{ PCI_DEVICE_DATA(INTEL, HDA_TGL_LP, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Tigerlake-H */
	{ PCI_DEVICE_DATA(INTEL, HDA_TGL_H, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* DG1 */
	{ PCI_DEVICE_DATA(INTEL, HDA_DG1, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* DG2 */
	{ PCI_DEVICE_DATA(INTEL, HDA_DG2_0, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	{ PCI_DEVICE_DATA(INTEL, HDA_DG2_1, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	{ PCI_DEVICE_DATA(INTEL, HDA_DG2_2, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Alderlake-S */
	{ PCI_DEVICE_DATA(INTEL, HDA_ADL_S, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Alderlake-P */
	{ PCI_DEVICE_DATA(INTEL, HDA_ADL_P, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	{ PCI_DEVICE_DATA(INTEL, HDA_ADL_PS, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	{ PCI_DEVICE_DATA(INTEL, HDA_ADL_PX, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Alderlake-M */
	{ PCI_DEVICE_DATA(INTEL, HDA_ADL_M, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Alderlake-N */
	{ PCI_DEVICE_DATA(INTEL, HDA_ADL_N, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Elkhart Lake */
	{ PCI_DEVICE_DATA(INTEL, HDA_EHL_0, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	{ PCI_DEVICE_DATA(INTEL, HDA_EHL_3, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Raptor Lake */
	{ PCI_DEVICE_DATA(INTEL, HDA_RPL_S, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	{ PCI_DEVICE_DATA(INTEL, HDA_RPL_P_0, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	{ PCI_DEVICE_DATA(INTEL, HDA_RPL_P_1, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	{ PCI_DEVICE_DATA(INTEL, HDA_RPL_M, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	{ PCI_DEVICE_DATA(INTEL, HDA_RPL_PX, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	{ PCI_DEVICE_DATA(INTEL, HDA_MTL, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Battlemage */
	{ PCI_DEVICE_DATA(INTEL, HDA_BMG, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Lunarlake-P */
	{ PCI_DEVICE_DATA(INTEL, HDA_LNL_P, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_LNL) },
	/* Arrow Lake-S */
	{ PCI_DEVICE_DATA(INTEL, HDA_ARL_S, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Arrow Lake */
	{ PCI_DEVICE_DATA(INTEL, HDA_ARL, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_SKYLAKE) },
	/* Panther Lake */
	// { PCI_DEVICE_DATA(INTEL, HDA_PTL, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_LNL) },
	{ PCI_DEVICE_DATA(INTEL, HDA_APL, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_LNL) },
	/* Apollolake (Broxton-P) */
	{ PCI_DEVICE_DATA(INTEL, HDA_APL, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_BROXTON) },
	/* Gemini-Lake */
	{ PCI_DEVICE_DATA(INTEL, HDA_GML, AZX_DRIVER_SKL | AZX_DCAPS_INTEL_BROXTON) },
	/* Haswell */
	{ PCI_DEVICE_DATA(INTEL, HDA_HSW_0, AZX_DRIVER_HDMI | AZX_DCAPS_INTEL_HASWELL) },
	{ PCI_DEVICE_DATA(INTEL, HDA_HSW_2, AZX_DRIVER_HDMI | AZX_DCAPS_INTEL_HASWELL) },
	{ PCI_DEVICE_DATA(INTEL, HDA_HSW_3, AZX_DRIVER_HDMI | AZX_DCAPS_INTEL_HASWELL) },
	/* Broadwell */
	{ PCI_DEVICE_DATA(INTEL, HDA_BDW, AZX_DRIVER_HDMI | AZX_DCAPS_INTEL_BROADWELL) },
	/* 5 Series/3400 */
	{ PCI_DEVICE_DATA(INTEL, HDA_5_3400_SERIES_0, AZX_DRIVER_SCH | AZX_DCAPS_INTEL_PCH_NOPM) },
	{ PCI_DEVICE_DATA(INTEL, HDA_5_3400_SERIES_1, AZX_DRIVER_SCH | AZX_DCAPS_INTEL_PCH_NOPM) },
	/* Poulsbo */
	{ PCI_DEVICE_DATA(INTEL, HDA_POULSBO, AZX_DRIVER_SCH | AZX_DCAPS_INTEL_PCH_BASE |
	  AZX_DCAPS_POSFIX_LPIB) },
	/* Oaktrail */
	{ PCI_DEVICE_DATA(INTEL, HDA_OAKTRAIL, AZX_DRIVER_SCH | AZX_DCAPS_INTEL_PCH_BASE) },
	/* BayTrail */
	{ PCI_DEVICE_DATA(INTEL, HDA_BYT, AZX_DRIVER_PCH | AZX_DCAPS_INTEL_BAYTRAIL) },
	/* Braswell */
	{ PCI_DEVICE_DATA(INTEL, HDA_BSW, AZX_DRIVER_PCH | AZX_DCAPS_INTEL_BRASWELL) },
	/* ICH6 */
	{ PCI_DEVICE_DATA(INTEL, HDA_ICH6, AZX_DRIVER_ICH | AZX_DCAPS_INTEL_ICH) },
	/* ICH7 */
	{ PCI_DEVICE_DATA(INTEL, HDA_ICH7, AZX_DRIVER_ICH | AZX_DCAPS_INTEL_ICH) },
	/* ESB2 */
	{ PCI_DEVICE_DATA(INTEL, HDA_ESB2, AZX_DRIVER_ICH | AZX_DCAPS_INTEL_ICH) },
	/* ICH8 */
	{ PCI_DEVICE_DATA(INTEL, HDA_ICH8, AZX_DRIVER_ICH | AZX_DCAPS_INTEL_ICH) },
	/* ICH9 */
	{ PCI_DEVICE_DATA(INTEL, HDA_ICH9_0, AZX_DRIVER_ICH | AZX_DCAPS_INTEL_ICH) },
	/* ICH9 */
	{ PCI_DEVICE_DATA(INTEL, HDA_ICH9_1, AZX_DRIVER_ICH | AZX_DCAPS_INTEL_ICH) },
	/* ICH10 */
	{ PCI_DEVICE_DATA(INTEL, HDA_ICH10_0, AZX_DRIVER_ICH | AZX_DCAPS_INTEL_ICH) },
	/* ICH10 */
	{ PCI_DEVICE_DATA(INTEL, HDA_ICH10_1, AZX_DRIVER_ICH | AZX_DCAPS_INTEL_ICH) },
	/* Generic Intel */
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_ANY_ID),
	  .class = PCI_CLASS_MULTIMEDIA_HD_AUDIO << 8,
	  .class_mask = 0xffffff,
	  .driver_data = AZX_DRIVER_ICH | AZX_DCAPS_NO_ALIGN_BUFSIZE },
	/* ATI SB 450/600/700/800/900 */
	{ PCI_VDEVICE(ATI, 0x437b),
	  .driver_data = AZX_DRIVER_ATI | AZX_DCAPS_PRESET_ATI_SB },
	{ PCI_VDEVICE(ATI, 0x4383),
	  .driver_data = AZX_DRIVER_ATI | AZX_DCAPS_PRESET_ATI_SB },
	/* AMD Hudson */
	{ PCI_VDEVICE(AMD, 0x780d),
	  .driver_data = AZX_DRIVER_GENERIC | AZX_DCAPS_PRESET_ATI_SB },
	/* AMD, X370 & co */
	{ PCI_VDEVICE(AMD, 0x1457),
	  .driver_data = AZX_DRIVER_GENERIC | AZX_DCAPS_PRESET_AMD_SB },
	/* AMD, X570 & co */
	{ PCI_VDEVICE(AMD, 0x1487),
	  .driver_data = AZX_DRIVER_GENERIC | AZX_DCAPS_PRESET_AMD_SB },
	/* AMD Stoney */
	{ PCI_VDEVICE(AMD, 0x157a),
	  .driver_data = AZX_DRIVER_GENERIC | AZX_DCAPS_PRESET_ATI_SB |
			 AZX_DCAPS_PM_RUNTIME },
	/* AMD Raven */
	{ PCI_VDEVICE(AMD, 0x15e3),
	  .driver_data = AZX_DRIVER_GENERIC | AZX_DCAPS_PRESET_AMD_SB },
	/* ATI HDMI */
	{ PCI_VDEVICE(ATI, 0x0002),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS |
	  AZX_DCAPS_PM_RUNTIME },
	{ PCI_VDEVICE(ATI, 0x1308),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS },
	{ PCI_VDEVICE(ATI, 0x157a),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS },
	{ PCI_VDEVICE(ATI, 0x15b3),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS },
	{ PCI_VDEVICE(ATI, 0x793b),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0x7919),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0x960f),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0x970f),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0x9840),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS },
	{ PCI_VDEVICE(ATI, 0xaa00),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0xaa08),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0xaa10),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0xaa18),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0xaa20),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0xaa28),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0xaa30),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0xaa38),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0xaa40),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0xaa48),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0xaa50),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0xaa58),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0xaa60),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0xaa68),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0xaa80),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0xaa88),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0xaa90),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0xaa98),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_VDEVICE(ATI, 0x9902),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS },
	{ PCI_VDEVICE(ATI, 0xaaa0),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS },
	{ PCI_VDEVICE(ATI, 0xaaa8),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS },
	{ PCI_VDEVICE(ATI, 0xaab0),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS },
	{ PCI_VDEVICE(ATI, 0xaac0),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS |
	  AZX_DCAPS_PM_RUNTIME },
	{ PCI_VDEVICE(ATI, 0xaac8),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS |
	  AZX_DCAPS_PM_RUNTIME },
	{ PCI_VDEVICE(ATI, 0xaad8),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS |
	  AZX_DCAPS_PM_RUNTIME },
	{ PCI_VDEVICE(ATI, 0xaae0),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS |
	  AZX_DCAPS_PM_RUNTIME },
	{ PCI_VDEVICE(ATI, 0xaae8),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS |
	  AZX_DCAPS_PM_RUNTIME },
	{ PCI_VDEVICE(ATI, 0xaaf0),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS |
	  AZX_DCAPS_PM_RUNTIME },
	{ PCI_VDEVICE(ATI, 0xaaf8),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS |
	  AZX_DCAPS_PM_RUNTIME },
	{ PCI_VDEVICE(ATI, 0xab00),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS |
	  AZX_DCAPS_PM_RUNTIME },
	{ PCI_VDEVICE(ATI, 0xab08),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS |
	  AZX_DCAPS_PM_RUNTIME },
	{ PCI_VDEVICE(ATI, 0xab10),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS |
	  AZX_DCAPS_PM_RUNTIME },
	{ PCI_VDEVICE(ATI, 0xab18),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS |
	  AZX_DCAPS_PM_RUNTIME },
	{ PCI_VDEVICE(ATI, 0xab20),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS |
	  AZX_DCAPS_PM_RUNTIME },
	{ PCI_VDEVICE(ATI, 0xab28),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS |
	  AZX_DCAPS_PM_RUNTIME },
	{ PCI_VDEVICE(ATI, 0xab30),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS |
	  AZX_DCAPS_PM_RUNTIME },
	{ PCI_VDEVICE(ATI, 0xab38),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI_NS |
	  AZX_DCAPS_PM_RUNTIME },
	/* GLENFLY */
	{ PCI_DEVICE(0x6766, PCI_ANY_ID),
	  .class = PCI_CLASS_MULTIMEDIA_HD_AUDIO << 8,
	  .class_mask = 0xffffff,
	  .driver_data = AZX_DRIVER_GFHDMI | AZX_DCAPS_POSFIX_LPIB |
	  AZX_DCAPS_NO_MSI | AZX_DCAPS_NO_64BIT },
	/* VIA VT8251/VT8237A */
	{ PCI_VDEVICE(VIA, 0x3288), .driver_data = AZX_DRIVER_VIA },
	/* VIA GFX VT7122/VX900 */
	{ PCI_VDEVICE(VIA, 0x9170), .driver_data = AZX_DRIVER_GENERIC },
	/* VIA GFX VT6122/VX11 */
	{ PCI_VDEVICE(VIA, 0x9140), .driver_data = AZX_DRIVER_GENERIC },
	/* SIS966 */
	{ PCI_VDEVICE(SI, 0x7502), .driver_data = AZX_DRIVER_SIS },
	/* ULI M5461 */
	{ PCI_VDEVICE(AL, 0x5461), .driver_data = AZX_DRIVER_ULI },
	/* NVIDIA MCP */
	{ PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID),
	  .class = PCI_CLASS_MULTIMEDIA_HD_AUDIO << 8,
	  .class_mask = 0xffffff,
	  .driver_data = AZX_DRIVER_NVIDIA | AZX_DCAPS_PRESET_NVIDIA },
	/* Teradici */
	{ PCI_DEVICE(0x6549, 0x1200),
	  .driver_data = AZX_DRIVER_TERA | AZX_DCAPS_NO_64BIT },
	{ PCI_DEVICE(0x6549, 0x2200),
	  .driver_data = AZX_DRIVER_TERA | AZX_DCAPS_NO_64BIT },
	/* Creative X-Fi (CA0110-IBG) */
	/* CTHDA chips */
	{ PCI_VDEVICE(CREATIVE, 0x0010),
	  .driver_data = AZX_DRIVER_CTHDA | AZX_DCAPS_PRESET_CTHDA },
	{ PCI_VDEVICE(CREATIVE, 0x0012),
	  .driver_data = AZX_DRIVER_CTHDA | AZX_DCAPS_PRESET_CTHDA },
#if !IS_ENABLED(CONFIG_SND_CTXFI)
	/* the following entry conflicts with snd-ctxfi driver,
	 * as ctxfi driver mutates from HD-audio to native mode with
	 * a special command sequence.
	 */
	{ PCI_DEVICE(PCI_VENDOR_ID_CREATIVE, PCI_ANY_ID),
	  .class = PCI_CLASS_MULTIMEDIA_HD_AUDIO << 8,
	  .class_mask = 0xffffff,
	  .driver_data = AZX_DRIVER_CTX | AZX_DCAPS_CTX_WORKAROUND |
	  AZX_DCAPS_NO_64BIT | AZX_DCAPS_POSFIX_LPIB },
#else
	/* this entry seems still valid -- i.e. without emu20kx chip */
	{ PCI_VDEVICE(CREATIVE, 0x0009),
	  .driver_data = AZX_DRIVER_CTX | AZX_DCAPS_CTX_WORKAROUND |
	  AZX_DCAPS_NO_64BIT | AZX_DCAPS_POSFIX_LPIB },
#endif
	/* CM8888 */
	{ PCI_VDEVICE(CMEDIA, 0x5011),
	  .driver_data = AZX_DRIVER_CMEDIA |
	  AZX_DCAPS_NO_MSI | AZX_DCAPS_POSFIX_LPIB | AZX_DCAPS_SNOOP_OFF },
	/* Vortex86MX */
	{ PCI_VDEVICE(RDC, 0x3010), .driver_data = AZX_DRIVER_GENERIC },
	/* VMware HDAudio */
	{ PCI_VDEVICE(VMWARE, 0x1977), .driver_data = AZX_DRIVER_GENERIC },
	/* AMD/ATI Generic, PCI class code and Vendor ID for HD Audio */
	{ PCI_DEVICE(PCI_VENDOR_ID_ATI, PCI_ANY_ID),
	  .class = PCI_CLASS_MULTIMEDIA_HD_AUDIO << 8,
	  .class_mask = 0xffffff,
	  .driver_data = AZX_DRIVER_GENERIC | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, PCI_ANY_ID),
	  .class = PCI_CLASS_MULTIMEDIA_HD_AUDIO << 8,
	  .class_mask = 0xffffff,
	  .driver_data = AZX_DRIVER_GENERIC | AZX_DCAPS_PRESET_ATI_HDMI },
	/* Zhaoxin */
	{ PCI_VDEVICE(ZHAOXIN, 0x3288), .driver_data = AZX_DRIVER_ZHAOXIN },
	/* Loongson HDAudio*/
	{ PCI_VDEVICE(LOONGSON, PCI_DEVICE_ID_LOONGSON_HDA),
	  .driver_data = AZX_DRIVER_LOONGSON },
	{ PCI_VDEVICE(LOONGSON, PCI_DEVICE_ID_LOONGSON_HDMI),
	  .driver_data = AZX_DRIVER_LOONGSON },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, fchip_idtable);


// driver module definition
static struct pci_driver driver = {
    .name = KBUILD_MODNAME,
    .id_table = fchip_idtable,
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