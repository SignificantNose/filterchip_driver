#pragma once

#include <linux/init.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/hda_codec.h>
#include <sound/hda_register.h>
#include <sound/hda_i915.h>
#include <linux/dma-map-ops.h>

#define FCHIP_DRIVER_NAME "FChip"
#define FCHIP_DRIVER_SHORTNAME "Filterchip"
#define FCHIP_DMA_MASK_BITS 32


#define FCHIP_AZX_MAX_CODECS		HDA_MAX_CODECS

/* max number of SDs */
/* ICH, ATI and VIA have 4 playback and 4 capture */
#define ICH6_NUM_CAPTURE	4
#define ICH6_NUM_PLAYBACK	4

/* ULI has 6 playback and 5 capture */
#define ULI_NUM_CAPTURE		5
#define ULI_NUM_PLAYBACK	6

/* ATI HDMI may have up to 8 playbacks and 0 capture */
#define ATIHDMI_NUM_CAPTURE	0
#define ATIHDMI_NUM_PLAYBACK	8


#define AZX_FORCE_CODEC_MASK	0x100


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


// snooping:
/* Defines for ATI HD Audio support in SB450 south bridge */
#define ATI_SB450_HDAUDIO_MISC_CNTR2_ADDR   0x42
#define ATI_SB450_HDAUDIO_ENABLE_SNOOP      0x02

/* Defines for Nvidia HDA support */
#define NVIDIA_HDA_TRANSREG_ADDR      0x4e
#define NVIDIA_HDA_ENABLE_COHBITS     0x0f
#define NVIDIA_HDA_ISTRM_COH          0x4d
#define NVIDIA_HDA_OSTRM_COH          0x4c
#define NVIDIA_HDA_ENABLE_COHBIT      0x01

/* Defines for Intel SCH HDA snoop control */
#define INTEL_HDA_CGCTL	 0x48
#define INTEL_HDA_CGCTL_MISCBDCGE        (0x1 << 6)
#define INTEL_SCH_HDA_DEVC      0x78
#define INTEL_SCH_HDA_DEVC_NOSNOOP       (0x1<<11)



#pragma endregion


struct azx_dev {
	struct hdac_stream core;

	unsigned int irq_pending:1;
	/*
	 * For VIA:
	 *  A flag to ensure DMA position is 0
	 *  when link position is not greater than FIFO size
	 */
	unsigned int insufficient:1;
};

// very necessary line of code, for callbacks to be casted properly
struct fchip_azx;

typedef unsigned int (*azx_get_pos_callback_t)(struct fchip_azx *, struct azx_dev *);
typedef int (*azx_get_delay_callback_t)(struct fchip_azx *, struct azx_dev *, unsigned int pos);

struct fchip_azx {
	struct hda_bus bus;

	struct snd_card *card;
	struct pci_dev *pci;
	int dev_index;

	// chip type specific
	int driver_type;
	unsigned int driver_caps;
	int playback_streams;
	int playback_index_offset;
	int capture_streams;
	int capture_index_offset;
	int num_streams;
	int jackpoll_interval; //jack poll interval in jiffies

	// Register interaction
	const struct hda_controller_ops *ops;

	// position adjustment callbacks
	azx_get_pos_callback_t get_position[2];
	azx_get_delay_callback_t get_delay[2];

	// locks
	struct mutex open_mutex; // Prevents concurrent open/close operations

	// PCM
	struct list_head pcm_list; // azx_pcm list

	// HD codec
	int  codec_probe_mask; // copied from probe_mask option
	unsigned int beep_mode;
	bool ctl_dev_id;

#ifdef CONFIG_SND_HDA_PATCH_LOADER
	const struct firmware *fw;
#endif

	// flags
	int bdl_pos_adj;
	unsigned int running:1;
	unsigned int fallback_to_single_cmd:1;
	unsigned int single_cmd:1;
	unsigned int msi:1;
	unsigned int probing:1; // codec probing phase
	unsigned int snoop:1;
	unsigned int uc_buffer:1; // non-cached pages for stream buffers
	unsigned int align_buffer_size:1;
	unsigned int disabled:1; // disabled by vga_switcheroo
	unsigned int pm_prepared:1;

	// GTS present
	unsigned int gts_present:1;

#ifdef CONFIG_SND_HDA_DSP_LOADER
	struct azx_dev saved_azx_dev;
#endif
};

// same as struct hda_intel
struct fchip_hda_intel {
	struct fchip_azx chip;

	// for pending irqs
	struct work_struct irq_pending_work;

	// sync probing
	struct completion probe_wait;
	struct delayed_work probe_work;

	// card list (for power_save trigger)
	struct list_head list;

	// extra flags 
	unsigned int irq_pending_warned:1;
	unsigned int probe_continued:1;
	unsigned int runtime_pm_disabled:1;

	// vga_switcheroo setup
	unsigned int use_vga_switcheroo:1;
	unsigned int vga_switcheroo_registered:1;
	unsigned int init_failed:1; // delayed init failed
	unsigned int freed:1; // resources already released

	bool need_i915_power:1; // the hda controller needs i915 power

	int probe_retry;	// being probe-retry
};

struct fchip{
    // PCI part
    struct snd_card* card;
    struct pci_dev* pci;
    
    // PCM part
    struct snd_pcm* pcm;

	struct fchip_azx* azx_chip;
};

// Functions to read/write to hda registers
struct hda_controller_ops {
	// Disable msi if supported, PCI only
	int (*disable_msi_reset_irq)(struct fchip_azx *);
	// Check if current position is acceptable
	int (*position_check)(struct fchip_azx *chip, struct azx_dev *azx_dev);
	// enable/disable the link power
	int (*link_power)(struct fchip_azx *chip, bool enable);
};

#define azx_to_hda_bus(fchip_azx)	(&(fchip_azx)->bus.core)
#define azx_dev_to_hdac_stream(fchip_azx) (&(fchip_azx)->core)
#define hdac_stream_to_azx_dev(s) container_of(s, struct azx_dev, core)

#define fchip_writereg_l(fchip_azx, reg, value) \
	snd_hdac_chip_writel(azx_to_hda_bus(fchip_azx), reg, value)
#define fchip_readreg_l(fchip_azx, reg) \
	snd_hdac_chip_readl(azx_to_hda_bus(fchip_azx), reg)
#define fchip_writereg_w(fchip_azx, reg, value) \
	snd_hdac_chip_writew(azx_to_hda_bus(fchip_azx), reg, value)
#define fchip_readreg_w(fchip_azx, reg) \
	snd_hdac_chip_readw(azx_to_hda_bus(fchip_azx), reg)
#define fchip_writereg_b(fchip_azx, reg, value) \
	snd_hdac_chip_writeb(azx_to_hda_bus(fchip_azx), reg, value)
#define fchip_readreg_b(fchip_azx, reg) \
	snd_hdac_chip_readb(azx_to_hda_bus(fchip_azx), reg)

#define fchip_alloc_stream_pages(fchip_azx) \
	snd_hdac_bus_alloc_stream_pages(azx_to_hda_bus(fchip_azx))
#define fchip_free_stream_pages(fchip_azx) \
	snd_hdac_bus_free_stream_pages(azx_to_hda_bus(fchip_azx))


#define fchip_has_pm_runtime(fchip_azx) \
	((fchip_azx)->driver_caps & AZX_DCAPS_PM_RUNTIME)

#define fchip_enter_link_reset(chip) \
	snd_hdac_bus_enter_link_reset(azx_to_hda_bus(chip))

#define fchip_get_snoop_type(fchip_azx) \
	(((fchip_azx)->driver_caps & AZX_DCAPS_SNOOP_MASK) >> 10)

#define fchip_snoop(fchip_azx) \
	(!IS_ENABLED(CONFIG_X86) || fchip_azx->snoop)

#define fchip_display_power(fchip_azx, enable) \
	snd_hdac_display_power(azx_to_hda_bus(fchip_azx), HDA_CODEC_IDX_CONTROLLER, enable)

#define fchip_has_pm_runtime(fchip_azx) \
	((fchip_azx)->driver_caps & AZX_DCAPS_PM_RUNTIME)


void fchip_init_chip(struct fchip_azx* fchip_azx, bool full_reset);
void fchip_stop_chip(struct fchip_azx* fchip_azx);

void fchip_set_default_power_save(struct fchip_azx* fchip_azx);
// not a fan of making this an interface, but for now:
int fchip_probe_continue(struct fchip_azx *chip);

void update_pci_config_byte(struct pci_dev *pci, unsigned int reg, unsigned char mask, unsigned char val);