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


#pragma region DEVICE_IDS_AND_CAPABILITIES

enum {
	AZX_SNOOP_TYPE_NONE,
	AZX_SNOOP_TYPE_SCH,
	AZX_SNOOP_TYPE_ATI,
	AZX_SNOOP_TYPE_NVIDIA,
};

enum {
	AZX_DRIVER_ICH,
	AZX_DRIVER_PCH,
	AZX_DRIVER_SCH,
	AZX_DRIVER_SKL,
	AZX_DRIVER_HDMI,
	AZX_DRIVER_ATI,
	AZX_DRIVER_ATIHDMI,
	AZX_DRIVER_ATIHDMI_NS,
	AZX_DRIVER_GFHDMI,
	AZX_DRIVER_VIA,
	AZX_DRIVER_SIS,
	AZX_DRIVER_ULI,
	AZX_DRIVER_NVIDIA,
	AZX_DRIVER_TERA,
	AZX_DRIVER_CTX,
	AZX_DRIVER_CTHDA,
	AZX_DRIVER_CMEDIA,
	AZX_DRIVER_ZHAOXIN,
	AZX_DRIVER_LOONGSON,
	AZX_DRIVER_GENERIC,
	AZX_NUM_DRIVERS, /* keep this as last entry */
};

/* driver quirks (capabilities) */
/* bits 0-7 are used for indicating driver type */
#define AZX_DCAPS_NO_TCSEL	(1 << 8)	/* No Intel TCSEL bit */
#define AZX_DCAPS_NO_MSI	(1 << 9)	/* No MSI support */
#define AZX_DCAPS_SNOOP_MASK	(3 << 10)	/* snoop type mask */
#define AZX_DCAPS_SNOOP_OFF	(1 << 12)	/* snoop default off */
#ifdef CONFIG_SND_HDA_I915
#define AZX_DCAPS_I915_COMPONENT (1 << 13)	/* bind with i915 gfx */
#else
#define AZX_DCAPS_I915_COMPONENT 0		/* NOP */
#endif
#define AZX_DCAPS_AMD_ALLOC_FIX	(1 << 14)	/* AMD allocation workaround */
#define AZX_DCAPS_CTX_WORKAROUND (1 << 15)	/* X-Fi workaround */
#define AZX_DCAPS_POSFIX_LPIB	(1 << 16)	/* Use LPIB as default */
#define AZX_DCAPS_AMD_WORKAROUND (1 << 17)	/* AMD-specific workaround */
#define AZX_DCAPS_NO_64BIT	(1 << 18)	/* No 64bit address */
/* 19 unused */
#define AZX_DCAPS_OLD_SSYNC	(1 << 20)	/* Old SSYNC reg for ICH */
#define AZX_DCAPS_NO_ALIGN_BUFSIZE (1 << 21)	/* no buffer size alignment */
/* 22 unused */
#define AZX_DCAPS_4K_BDLE_BOUNDARY (1 << 23)	/* BDLE in 4k boundary */
/* 24 unused */
#define AZX_DCAPS_COUNT_LPIB_DELAY  (1 << 25)	/* Take LPIB as delay */
#define AZX_DCAPS_PM_RUNTIME	(1 << 26)	/* runtime PM support */
#define AZX_DCAPS_RETRY_PROBE	(1 << 27)	/* retry probe if no codec is configured */
#define AZX_DCAPS_CORBRP_SELF_CLEAR (1 << 28)	/* CORBRP clears itself after reset */
#define AZX_DCAPS_NO_MSI64      (1 << 29)	/* Stick to 32-bit MSIs */
#define AZX_DCAPS_SEPARATE_STREAM_TAG	(1 << 30) /* capture and playback use separate stream tag */
#define AZX_DCAPS_PIO_COMMANDS (1 << 31)	/* Use PIO instead of CORB for commands */


#define AZX_DCAPS_SNOOP_TYPE(type) ((AZX_SNOOP_TYPE_ ## type) << 10)

/* quirks for old Intel chipsets */
#define AZX_DCAPS_INTEL_ICH \
	(AZX_DCAPS_OLD_SSYNC | AZX_DCAPS_NO_ALIGN_BUFSIZE)

/* quirks for Intel PCH */
#define AZX_DCAPS_INTEL_PCH_BASE \
	(AZX_DCAPS_NO_ALIGN_BUFSIZE | AZX_DCAPS_COUNT_LPIB_DELAY |\
	 AZX_DCAPS_SNOOP_TYPE(SCH))

/* PCH up to IVB; no runtime PM; bind with i915 gfx */
#define AZX_DCAPS_INTEL_PCH_NOPM \
	(AZX_DCAPS_INTEL_PCH_BASE | AZX_DCAPS_I915_COMPONENT)

/* PCH for HSW/BDW; with runtime PM */
/* no i915 binding for this as HSW/BDW has another controller for HDMI */
#define AZX_DCAPS_INTEL_PCH \
	(AZX_DCAPS_INTEL_PCH_BASE | AZX_DCAPS_PM_RUNTIME)

/* HSW HDMI */
#define AZX_DCAPS_INTEL_HASWELL \
	(/*AZX_DCAPS_ALIGN_BUFSIZE |*/ AZX_DCAPS_COUNT_LPIB_DELAY |\
	 AZX_DCAPS_PM_RUNTIME | AZX_DCAPS_I915_COMPONENT |\
	 AZX_DCAPS_SNOOP_TYPE(SCH))

/* Broadwell HDMI can't use position buffer reliably, force to use LPIB */
#define AZX_DCAPS_INTEL_BROADWELL \
	(/*AZX_DCAPS_ALIGN_BUFSIZE |*/ AZX_DCAPS_POSFIX_LPIB |\
	 AZX_DCAPS_PM_RUNTIME | AZX_DCAPS_I915_COMPONENT |\
	 AZX_DCAPS_SNOOP_TYPE(SCH))

#define AZX_DCAPS_INTEL_BAYTRAIL \
	(AZX_DCAPS_INTEL_PCH_BASE | AZX_DCAPS_I915_COMPONENT)

#define AZX_DCAPS_INTEL_BRASWELL \
	(AZX_DCAPS_INTEL_PCH_BASE | AZX_DCAPS_PM_RUNTIME |\
	 AZX_DCAPS_I915_COMPONENT)

#define AZX_DCAPS_INTEL_SKYLAKE \
	(AZX_DCAPS_INTEL_PCH_BASE | AZX_DCAPS_PM_RUNTIME |\
	 AZX_DCAPS_SEPARATE_STREAM_TAG | AZX_DCAPS_I915_COMPONENT)

#define AZX_DCAPS_INTEL_BROXTON		AZX_DCAPS_INTEL_SKYLAKE

#define AZX_DCAPS_INTEL_LNL \
	(AZX_DCAPS_INTEL_SKYLAKE | AZX_DCAPS_PIO_COMMANDS)

/* quirks for ATI SB / AMD Hudson */
#define AZX_DCAPS_PRESET_ATI_SB \
	(AZX_DCAPS_NO_TCSEL | AZX_DCAPS_POSFIX_LPIB |\
	 AZX_DCAPS_SNOOP_TYPE(ATI))

/* quirks for ATI/AMD HDMI */
#define AZX_DCAPS_PRESET_ATI_HDMI \
	(AZX_DCAPS_NO_TCSEL | AZX_DCAPS_POSFIX_LPIB|\
	 AZX_DCAPS_NO_MSI64)

/* quirks for ATI HDMI with snoop off */
#define AZX_DCAPS_PRESET_ATI_HDMI_NS \
	(AZX_DCAPS_PRESET_ATI_HDMI | AZX_DCAPS_AMD_ALLOC_FIX)

/* quirks for AMD SB */
#define AZX_DCAPS_PRESET_AMD_SB \
	(AZX_DCAPS_NO_TCSEL | AZX_DCAPS_AMD_WORKAROUND |\
	 AZX_DCAPS_SNOOP_TYPE(ATI) | AZX_DCAPS_PM_RUNTIME |\
	 AZX_DCAPS_RETRY_PROBE)

/* quirks for Nvidia */
#define AZX_DCAPS_PRESET_NVIDIA \
	(AZX_DCAPS_NO_MSI | AZX_DCAPS_CORBRP_SELF_CLEAR |\
	 AZX_DCAPS_SNOOP_TYPE(NVIDIA))

#define AZX_DCAPS_PRESET_CTHDA \
	(AZX_DCAPS_NO_MSI | AZX_DCAPS_POSFIX_LPIB |\
	 AZX_DCAPS_NO_64BIT |\
	 AZX_DCAPS_4K_BDLE_BOUNDARY | AZX_DCAPS_SNOOP_OFF)

#pragma endregion

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
