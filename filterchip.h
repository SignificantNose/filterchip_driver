#pragma once
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/hda_codec.h>

#define FCHIP_DRIVER_NAME "FChip"
#define FCHIP_DRIVER_SHORTNAME "Filterchip"
#define FCHIP_DMA_MASK_BITS 32

#define FCHIP_VENDOR_ID 0x8086 
#define FCHIP_DEVICE_ID 0xa348


struct filterchip{
    // PCI part
    struct snd_card* card;
    struct pci_dev* pci;
    
    // todo change to mmio?
    unsigned long port;
    int irq;


    // PCM part
    struct snd_pcm* pcm;
};
