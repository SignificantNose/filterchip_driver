#include "fchip.h"
#include "fchip_pcm.h"
#include "fchip_codec.h"
#include "fchip_vga.h"
#include "fchip_posfix.h"
#include "fchip_hda_bus.h"
#include "fchip_int.h"

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char* id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

// hda specifics
static char *patch[SNDRV_CARDS];
static int probe_only[SNDRV_CARDS];
static int jackpoll_ms[SNDRV_CARDS];
static char *model[SNDRV_CARDS];
static int position_fix[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS-1)] = -1};
static int bdl_pos_adj[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS-1)] = -1};
static int probe_mask[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS-1)] = -1};
static int single_cmd = -1;
static int align_buffer_size = -1;
static int enable_msi = -1;
static int hda_snoop = -1;
static int pm_blacklist = -1;



// number of codec slots for each chipset: 0 = default slots (i.e. 4) 
static const unsigned int azx_max_codecs[AZX_NUM_DRIVERS] = {
	[AZX_DRIVER_NVIDIA] = 8,
	[AZX_DRIVER_TERA] = 1,
};
static bool beep_mode[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS-1)] =
					CONFIG_SND_HDA_INPUT_BEEP_MODE};

static bool ctl_dev_id = IS_ENABLED(CONFIG_SND_HDA_CTL_DEV_ID) ? 1 : 0;
static int power_save = CONFIG_SND_HDA_POWER_SAVE_DEFAULT;



static DEFINE_MUTEX(card_list_lock);
static LIST_HEAD(card_list);


void update_pci_byte(struct pci_dev *pci, unsigned int reg, unsigned char mask, unsigned char val)
{
	unsigned char data;

	pci_read_config_byte(pci, reg, &data);
	data &= ~mask;
	data |= (val & mask);
	pci_write_config_byte(pci, reg, data);
}

static void fchip_add_card_list(struct fchip_azx* fchip_azx)
{
	struct fchip_hda_intel *hda = container_of(fchip_azx, struct fchip_hda_intel, chip);
	mutex_lock(&card_list_lock);
	list_add(&hda->list, &card_list);
	mutex_unlock(&card_list_lock);
}

static void fchip_del_card_list(struct fchip_azx* fchip_azx)
{
	struct fchip_hda_intel *hda = container_of(fchip_azx, struct fchip_hda_intel, chip);
	mutex_lock(&card_list_lock);
	list_del_init(&hda->list);
	mutex_unlock(&card_list_lock);
}

void fchip_init_chip(struct fchip_azx* fchip_azx, bool full_reset)
{
	if (snd_hdac_bus_init_chip(azx_to_hda_bus(fchip_azx), full_reset)) {
		/* correct RINTCNT for CXT */
		if (fchip_azx->driver_caps & AZX_DCAPS_CTX_WORKAROUND){
			fchip_writereg_w(fchip_azx, RINTCNT, 0xc0);
		}
	}
}
void fchip_stop_chip(struct fchip_azx* fchip_azx)
{
	snd_hdac_bus_stop_chip(azx_to_hda_bus(fchip_azx));
}


static int fchip_dev_disconnect(struct snd_device *device)
{
	struct fchip_azx* fchip_azx = device->device_data;
	struct hdac_bus *bus = azx_to_hda_bus(fchip_azx);

	fchip_azx->bus.shutdown = 1;
	cancel_work_sync(&bus->unsol_work);

	return 0;
}


static void fchip_free(struct fchip_azx* fchip_azx);
// COMPONENT dtor
static int fchip_dev_free(struct snd_device* device)
{
    // printk(KERN_DEBUG "fchip: Device free called\n");
    // return snd_filterchip_free(device->device_data);
	fchip_free(device->device_data);
	return 0;
}

static void fchip_irq_pending_work(struct work_struct *work)
{}

static void fchip_check_snoop_available(struct fchip_azx *chip)
{
	int snoop = hda_snoop;

	if (snoop >= 0) {
		printk(KERN_INFO "fchip: Force to %s mode by module option\n",
			 snoop ? "snoop" : "non-snoop");
		chip->snoop = snoop;
		chip->uc_buffer = !snoop;
		return;
	}

	snoop = true;
	if (fchip_get_snoop_type(chip) == AZX_SNOOP_TYPE_NONE &&
	    chip->driver_type == AZX_DRIVER_VIA) 
	{
		/* force to non-snoop mode for a new VIA controller
		 * when BIOS is set
		 */
		u8 val;
		pci_read_config_byte(chip->pci, 0x42, &val);
		if (!(val & 0x80) && (chip->pci->revision == 0x30 ||
				      chip->pci->revision == 0x20))
		{
			snoop = false;
		}
	}

	if (chip->driver_caps & AZX_DCAPS_SNOOP_OFF){
		snoop = false;
	}

#ifdef CONFIG_X86
	/* check the presence of DMA ops (i.e. IOMMU), disable snoop conditionally */
	if ((chip->driver_caps & AZX_DCAPS_AMD_ALLOC_FIX) &&
	    !get_dma_ops(chip->card->dev)){
		snoop = false;
	}
#endif

	chip->snoop = snoop;
	if (!snoop) {
		printk(KERN_INFO "fchip: Force to non-snoop mode\n");
		/* C-Media requires non-cached pages only for CORB/RIRB */
		if (chip->driver_type != AZX_DRIVER_CMEDIA){
			chip->uc_buffer = true;
		}
	}
}

static int default_bdl_pos_adj(struct fchip_azx *chip)
{
	/* some exceptions: Atoms seem problematic with value 1 */
	if (chip->pci->vendor == PCI_VENDOR_ID_INTEL) {
		switch (chip->pci->device) {
		case PCI_DEVICE_ID_INTEL_HDA_BYT:
		case PCI_DEVICE_ID_INTEL_HDA_BSW:
			return 32;
		case PCI_DEVICE_ID_INTEL_HDA_APL:
			return 64;
		}
	}

	switch (chip->driver_type) {
	/*
	 * increase the bdl size for Glenfly Gpus for hardware
	 * limitation on hdac interrupt interval
	 */
	case AZX_DRIVER_GFHDMI:
		return 128;
	case AZX_DRIVER_ICH:
	case AZX_DRIVER_PCH:
		return 1;
	default:
		return 32;
	}
}

static const struct snd_pci_quirk probe_mask_list[] = {
	/* Thinkpad often breaks the controller communication when accessing
	 * to the non-working (or non-existing) modem codec slot.
	 */
	SND_PCI_QUIRK(0x1014, 0x05b7, "Thinkpad Z60", 0x01),
	SND_PCI_QUIRK(0x17aa, 0x2010, "Thinkpad X/T/R60", 0x01),
	SND_PCI_QUIRK(0x17aa, 0x20ac, "Thinkpad X/T/R61", 0x01),
	/* broken BIOS */
	SND_PCI_QUIRK(0x1028, 0x20ac, "Dell Studio Desktop", 0x01),
	/* including bogus ALC268 in slot#2 that conflicts with ALC888 */
	SND_PCI_QUIRK(0x17c0, 0x4085, "Medion MD96630", 0x01),
	/* forced codec slots */
	SND_PCI_QUIRK(0x1043, 0x1262, "ASUS W5Fm", 0x103),
	SND_PCI_QUIRK(0x1046, 0x1262, "ASUS W5F", 0x103),
	SND_PCI_QUIRK(0x1558, 0x0351, "Schenker Dock 15", 0x105),
	/* WinFast VP200 H (Teradici) user reported broken communication */
	SND_PCI_QUIRK(0x3a21, 0x040d, "WinFast VP200 H", 0x101),
	{}
};

static void fchip_check_probe_mask(struct fchip_azx *fchip_azx, int dev)
{
	const struct snd_pci_quirk *q;

	fchip_azx->codec_probe_mask = probe_mask[dev];
	if (fchip_azx->codec_probe_mask == -1) {
		q = snd_pci_quirk_lookup(fchip_azx->pci, probe_mask_list);
		if (q) {
			printk(KERN_INFO "fchip: probe_mask set to 0x%x for device %04x:%04x\n",
				 q->value, q->subvendor, q->subdevice);
			fchip_azx->codec_probe_mask = q->value;
		}
	}

	/* check forced option */
	if (fchip_azx->codec_probe_mask != -1 &&
	    (fchip_azx->codec_probe_mask & AZX_FORCE_CODEC_MASK)) {
		azx_to_hda_bus(fchip_azx)->codec_mask = fchip_azx->codec_probe_mask & 0xff;
		printk(KERN_INFO "fchip: codec_mask forced to 0x%x\n",
			 (int)azx_to_hda_bus(fchip_azx)->codec_mask);
	}
}

static void fchip_free_streams(struct fchip_azx* fchip_azx)
{
	struct hdac_bus *bus = azx_to_hda_bus(fchip_azx);
	struct hdac_stream *s;

	while (!list_empty(&bus->stream_list)) {
		s = list_first_entry(&bus->stream_list, struct hdac_stream, list);
		list_del(&s->list);
		kfree(hdac_stream_to_azx_dev(s));
	}
}

static void fchip_stop_all_streams(struct fchip_azx* fchip_azx)
{
	struct hdac_bus *bus = azx_to_hda_bus(fchip_azx);

	snd_hdac_stop_streams(bus);
}

static void fchip_free(struct fchip_azx* fchip_azx)
{
	struct pci_dev *pci = fchip_azx->pci;
	struct fchip_hda_intel *hda = container_of(fchip_azx, struct fchip_hda_intel, chip);
	struct hdac_bus *bus = azx_to_hda_bus(fchip_azx);

	if (hda->freed){
		return;
	}

	if (fchip_has_pm_runtime(fchip_azx) && fchip_azx->running) {
		pm_runtime_get_noresume(&pci->dev);
		pm_runtime_forbid(&pci->dev);
		pm_runtime_dont_use_autosuspend(&pci->dev);
	}

	fchip_azx->running = 0;

	fchip_del_card_list(fchip_azx);

	hda->init_failed = 1; /* to be sure */
	complete_all(&hda->probe_wait);

	if (hda->use_vga_switcheroo) {
		if (fchip_azx->disabled && hda->probe_continued){
			snd_hda_unlock_devices(&fchip_azx->bus);
		}
		if (hda->vga_switcheroo_registered){
			vga_switcheroo_unregister_client(fchip_azx->pci);
		}
	}

	if (bus->chip_init) {
		fchip_clear_irq_pending(fchip_azx);
		fchip_stop_all_streams(fchip_azx);
		fchip_stop_chip(fchip_azx);
	}

	if (bus->irq >= 0){
		free_irq(bus->irq, (void*)fchip_azx);
	}

	fchip_free_stream_pages(fchip_azx);
	fchip_free_streams(fchip_azx);
	snd_hdac_bus_exit(bus);

#ifdef CONFIG_SND_HDA_PATCH_LOADER
	release_firmware(fchip_azx->fw);
#endif
	fchip_display_power(fchip_azx, false);

	if (fchip_azx->driver_caps & AZX_DCAPS_I915_COMPONENT){
		snd_hdac_i915_exit(bus);
	}

	hda->freed = 1;
}

/* On some boards setting power_save to a non 0 value leads to clicking /
 * popping sounds when ever we enter/leave powersaving mode. Ideally we would
 * figure out how to avoid these sounds, but that is not always feasible.
 * So we keep a list of devices where we disable powersaving as its known
 * to causes problems on these devices.
 */
static const struct snd_pci_quirk power_save_denylist[] = {
	/* https://bugzilla.redhat.com/show_bug.cgi?id=1525104 */
	SND_PCI_QUIRK(0x1849, 0xc892, "Asrock B85M-ITX", 0),
	/* https://bugzilla.redhat.com/show_bug.cgi?id=1525104 */
	SND_PCI_QUIRK(0x1849, 0x0397, "Asrock N68C-S UCC", 0),
	/* https://bugzilla.redhat.com/show_bug.cgi?id=1525104 */
	SND_PCI_QUIRK(0x1849, 0x7662, "Asrock H81M-HDS", 0),
	/* https://bugzilla.redhat.com/show_bug.cgi?id=1525104 */
	SND_PCI_QUIRK(0x1043, 0x8733, "Asus Prime X370-Pro", 0),
	/* https://bugzilla.redhat.com/show_bug.cgi?id=1525104 */
	SND_PCI_QUIRK(0x1028, 0x0497, "Dell Precision T3600", 0),
	/* https://bugzilla.redhat.com/show_bug.cgi?id=1525104 */
	/* Note the P55A-UD3 and Z87-D3HP share the subsys id for the HDA dev */
	SND_PCI_QUIRK(0x1458, 0xa002, "Gigabyte P55A-UD3 / Z87-D3HP", 0),
	/* https://bugzilla.redhat.com/show_bug.cgi?id=1525104 */
	SND_PCI_QUIRK(0x8086, 0x2040, "Intel DZ77BH-55K", 0),
	/* https://bugzilla.kernel.org/show_bug.cgi?id=199607 */
	SND_PCI_QUIRK(0x8086, 0x2057, "Intel NUC5i7RYB", 0),
	/* https://bugs.launchpad.net/bugs/1821663 */
	SND_PCI_QUIRK(0x8086, 0x2064, "Intel SDP 8086:2064", 0),
	/* https://bugzilla.redhat.com/show_bug.cgi?id=1520902 */
	SND_PCI_QUIRK(0x8086, 0x2068, "Intel NUC7i3BNB", 0),
	/* https://bugzilla.kernel.org/show_bug.cgi?id=198611 */
	SND_PCI_QUIRK(0x17aa, 0x2227, "Lenovo X1 Carbon 3rd Gen", 0),
	SND_PCI_QUIRK(0x17aa, 0x316e, "Lenovo ThinkCentre M70q", 0),
	/* https://bugzilla.redhat.com/show_bug.cgi?id=1689623 */
	SND_PCI_QUIRK(0x17aa, 0x367b, "Lenovo IdeaCentre B550", 0),
	/* https://bugzilla.redhat.com/show_bug.cgi?id=1572975 */
	SND_PCI_QUIRK(0x17aa, 0x36a7, "Lenovo C50 All in one", 0),
	/* https://bugs.launchpad.net/bugs/1821663 */
	SND_PCI_QUIRK(0x1631, 0xe017, "Packard Bell NEC IMEDIA 5204", 0),
	/* KONTRON SinglePC may cause a stall at runtime resume */
	SND_PCI_QUIRK(0x1734, 0x1232, "KONTRON SinglePC", 0),
	{}
};

void fchip_set_default_power_save(struct fchip_azx* fchip_azx)
{
	struct fchip_hda_intel *hda = container_of(fchip_azx, struct fchip_hda_intel, chip);
	int val = power_save;

	if (pm_blacklist < 0) {
		const struct snd_pci_quirk *q;

		q = snd_pci_quirk_lookup(fchip_azx->pci, power_save_denylist);
		if (q && val) {
			printk(KERN_INFO "fchip: Device %04x:%04x is on the power_save denylist, forcing power_save to 0\n",
				 q->subvendor, q->subdevice);
			val = 0;
			hda->runtime_pm_disabled = 1;
		}
	} else if (pm_blacklist > 0) {
		printk(KERN_INFO "fchip: Forcing power_save to 0 via option\n");
		val = 0;
	}
	snd_hda_set_power_save(&fchip_azx->bus, val * 1000);
}


static void fchip_remove(struct pci_dev *pci)
{
	struct snd_card* card = pci_get_drvdata(pci);
	struct fchip_azx* fchip_azx;
	struct fchip_hda_intel* hda;

	if (card){
		/* cancel the pending probing work */
		fchip_azx = ((struct fchip*)(card->private_data))->azx_chip;
		hda = container_of(fchip_azx, struct fchip_hda_intel, chip);
		/* Below is an ugly workaround.
		 * Both device_release_driver() and driver_probe_device()
		 * take *both* the device's and its parent's lock before
		 * calling the remove() and probe() callbacks.  The codec
		 * probe takes the locks of both the codec itself and its
		 * parent, i.e. the PCI controller dev.  Meanwhile, when
		 * the PCI controller is unbound, it takes its lock, too
		 * ==> ouch, a deadlock!
		 * As a workaround, we unlock temporarily here the controller
		 * device during cancel_work_sync() call.
		 */
		device_unlock(&pci->dev);
		cancel_delayed_work_sync(&hda->probe_work);
		device_lock(&pci->dev);

		pci_set_drvdata(pci, NULL);
		snd_card_free(card);
	}
}

static void __fchip_shutdown_chip(struct fchip_azx* fchip_azx, bool skip_link_reset)
{
	fchip_stop_chip(fchip_azx);
	if (!skip_link_reset){
		fchip_enter_link_reset(fchip_azx);
	}
	fchip_clear_irq_pending(fchip_azx);
	fchip_display_power(fchip_azx, false);
}

static void fchip_shutdown(struct pci_dev *pci)
{
	struct snd_card* card = pci_get_drvdata(pci);
	struct fchip_azx* fchip_azx;

	if (!card){
		return;
	}

	fchip_azx = ((struct fchip*)(card->private_data))->azx_chip;
	if (fchip_azx && fchip_azx->running){
		__fchip_shutdown_chip(fchip_azx, true);
	}
}



// determine stream direction based on its idx (see p.34)
static int stream_direction(struct fchip_azx* fchip_azx, unsigned char index)
{
	if (index >= fchip_azx->capture_index_offset &&
	    index < fchip_azx->capture_index_offset + fchip_azx->capture_streams){
		return SNDRV_PCM_STREAM_CAPTURE;
	}

	return SNDRV_PCM_STREAM_PLAYBACK;
}

// assign the starting BDL address to each stream (device) 
// (using stream descriptors) and init
int fchip_init_streams(struct fchip_azx* fchip_azx)
{
	int i;
	int stream_tags[2] = { 0, 0 };

	/* initialize each stream (aka device)
	 * assign the starting bdl address to each stream (device)
	 * and initialize
	 */
	for (i = 0; i < fchip_azx->num_streams; i++) {
		struct azx_dev *azx_dev = kzalloc(sizeof(*azx_dev), GFP_KERNEL);
		int dir, tag;

		if (!azx_dev){
			return -ENOMEM;
		}

		dir = stream_direction(fchip_azx, i);
		/* stream tag must be unique throughout
		 * the stream direction group,
		 * valid values 1...15
		 * use separate stream tag if the flag
		 * AZX_DCAPS_SEPARATE_STREAM_TAG is used
		 */
		if (fchip_azx->driver_caps & AZX_DCAPS_SEPARATE_STREAM_TAG){
			tag = ++stream_tags[dir];
		}
		else{	
			tag = i + 1;
		}

		snd_hdac_stream_init(azx_to_hda_bus(fchip_azx), azx_dev_to_hdac_stream(azx_dev), i, dir, tag);
	}

	return 0;
}

static void fchip_init_pci(struct fchip_azx* fchip_azx)
{
	int snoop_type = fchip_get_snoop_type(fchip_azx);

	/* Clear bits 0-2 of PCI register TCSEL (at offset 0x44)
	 * TCSEL == Traffic Class Select Register, which sets PCI express QOS
	 * Ensuring these bits are 0 clears playback static on some HD Audio
	 * codecs.
	 * The PCI register TCSEL is defined in the Intel manuals.
	 */
	if (!(fchip_azx->driver_caps & AZX_DCAPS_NO_TCSEL)) {
		printk(KERN_DEBUG "fchip: Clearing TCSEL\n");
		update_pci_byte(fchip_azx->pci, AZX_PCIREG_TCSEL, 0x07, 0);
	}

	/* For ATI SB450/600/700/800/900 and AMD Hudson azalia HD audio,
	 * we need to enable snoop.
	 */
	if (snoop_type == AZX_SNOOP_TYPE_ATI) {
		printk(KERN_DEBUG "fchip: Setting ATI snoop: %d\n", fchip_snoop(fchip_azx));
		update_pci_byte(fchip_azx->pci,	ATI_SB450_HDAUDIO_MISC_CNTR2_ADDR, 0x07, 
			fchip_snoop(fchip_azx) ? ATI_SB450_HDAUDIO_ENABLE_SNOOP : 0);
	}

	/* For NVIDIA HDA, enable snoop */
	if (snoop_type == AZX_SNOOP_TYPE_NVIDIA) {
		printk(KERN_DEBUG "fchip: Setting Nvidia snoop: %d\n", fchip_snoop(fchip_azx));
		update_pci_byte(fchip_azx->pci,
				NVIDIA_HDA_TRANSREG_ADDR,
				0x0f, NVIDIA_HDA_ENABLE_COHBITS);
		update_pci_byte(fchip_azx->pci,
				NVIDIA_HDA_ISTRM_COH,
				0x01, NVIDIA_HDA_ENABLE_COHBIT);
		update_pci_byte(fchip_azx->pci,
				NVIDIA_HDA_OSTRM_COH,
				0x01, NVIDIA_HDA_ENABLE_COHBIT);
	}

	/* Enable SCH/PCH snoop if needed */
	if (snoop_type == AZX_SNOOP_TYPE_SCH) {
		unsigned short snoop;
		pci_read_config_word(fchip_azx->pci, INTEL_SCH_HDA_DEVC, &snoop);
		if ((!fchip_snoop(fchip_azx) && !(snoop & INTEL_SCH_HDA_DEVC_NOSNOOP)) ||
		    (fchip_snoop(fchip_azx) && (snoop & INTEL_SCH_HDA_DEVC_NOSNOOP))) {
			snoop &= ~INTEL_SCH_HDA_DEVC_NOSNOOP;
			if (!fchip_snoop(fchip_azx)){
				snoop |= INTEL_SCH_HDA_DEVC_NOSNOOP;
			}
			pci_write_config_word(fchip_azx->pci, INTEL_SCH_HDA_DEVC, snoop);
			pci_read_config_word(fchip_azx->pci,
				INTEL_SCH_HDA_DEVC, &snoop);
		}
	
		printk(KERN_DEBUG "fchip: SCH snoop: %s\n",
			(snoop & INTEL_SCH_HDA_DEVC_NOSNOOP) ?
			"Disabled" : "Enabled");
    }
}

/*
 * ML_LCAP bits:
 *  bit 0: 6 MHz Supported
 *  bit 1: 12 MHz Supported
 *  bit 2: 24 MHz Supported
 *  bit 3: 48 MHz Supported
 *  bit 4: 96 MHz Supported
 *  bit 5: 192 MHz Supported
 */
static int intel_get_lctl_scf(struct fchip_azx* fchip_azx)
{
	struct hdac_bus* bus = azx_to_hda_bus(fchip_azx);
	static const int preferred_bits[] = { 2, 3, 1, 4, 5 };
	u32 val, t;
	int i;

	val = readl(bus->mlcap + AZX_ML_BASE + AZX_REG_ML_LCAP);

	for (i = 0; i < ARRAY_SIZE(preferred_bits); i++) {
		t = preferred_bits[i];
		if (val & (1 << t)){
			return t;
		}
	}

	printk(KERN_WARNING "fchip: Set audio clock frequency to 6MHz");
	return 0;
}

static int intel_ml_lctl_set_power(struct fchip_azx* fchip_azx, int state)
{
	struct hdac_bus *bus = azx_to_hda_bus(fchip_azx);
	u32 val;
	int timeout;

	/*
	 * Changes to LCTL.SCF are only needed for the first multi-link dealing
	 * with external codecs
	 */
	val = readl(bus->mlcap + AZX_ML_BASE + AZX_REG_ML_LCTL);
	val &= ~AZX_ML_LCTL_SPA;
	val |= state << AZX_ML_LCTL_SPA_SHIFT;
	writel(val, bus->mlcap + AZX_ML_BASE + AZX_REG_ML_LCTL);
	/* wait for CPA */
	timeout = 50;
	while (timeout) {
		if (((readl(bus->mlcap + AZX_ML_BASE + AZX_REG_ML_LCTL)) &
		    AZX_ML_LCTL_CPA) == (state << AZX_ML_LCTL_CPA_SHIFT)){
			return 0;
		}
		timeout--;
		udelay(10);
	}

	return -1;
}

static void intel_init_lctl(struct fchip_azx* fchip_azx)
{
	struct hdac_bus *bus = azx_to_hda_bus(fchip_azx);
	u32 val;
	int ret;

	/* 0. check lctl register value is correct or not */
	val = readl(bus->mlcap + AZX_ML_BASE + AZX_REG_ML_LCTL);
	/* only perform additional configurations if the SCF is initially based on 6MHz */
	if ((val & AZX_ML_LCTL_SCF) != 0){
		return;
	}

	/*
	 * Before operating on SPA, CPA must match SPA.
	 * Any deviation may result in undefined behavior.
	 */
	if (((val & AZX_ML_LCTL_SPA) >> AZX_ML_LCTL_SPA_SHIFT) !=
		((val & AZX_ML_LCTL_CPA) >> AZX_ML_LCTL_CPA_SHIFT)){
		return;
	}

	/* 1. turn link down: set SPA to 0 and wait CPA to 0 */
	ret = intel_ml_lctl_set_power(fchip_azx, 0);
	udelay(100);
	if (ret)
		goto set_spa;

	/* 2. update SCF to select an audio clock different from 6MHz */
	val &= ~AZX_ML_LCTL_SCF;
	val |= intel_get_lctl_scf(fchip_azx);
	writel(val, bus->mlcap + AZX_ML_BASE + AZX_REG_ML_LCTL);

set_spa:
	/* 4. turn link up: set SPA to 1 and wait CPA to 1 */
	intel_ml_lctl_set_power(fchip_azx, 1);
	udelay(100);
}


static void bxt_reduce_dma_latency(struct fchip_azx* fchip_azx)
{
	u32 val;

	val = fchip_readreg_l(fchip_azx, VS_EM4L);
	val &= (0x3 << 20);
	fchip_writereg_l(fchip_azx, VS_EM4L, val);
}

static void fchip_hda_intel_init_chip(struct fchip_azx* fchip_azx, bool full_reset)
{
	struct hdac_bus *bus = azx_to_hda_bus(fchip_azx);
	struct pci_dev *pci = fchip_azx->pci;
	u32 val;

	snd_hdac_set_codec_wakeup(bus, true);
	if (fchip_azx->driver_type == AZX_DRIVER_SKL) {
		pci_read_config_dword(pci, INTEL_HDA_CGCTL, &val);
		val = val & ~INTEL_HDA_CGCTL_MISCBDCGE;
		pci_write_config_dword(pci, INTEL_HDA_CGCTL, val);
	}
	fchip_init_chip(fchip_azx, full_reset);
	if (fchip_azx->driver_type == AZX_DRIVER_SKL) {
		pci_read_config_dword(pci, INTEL_HDA_CGCTL, &val);
		val = val | INTEL_HDA_CGCTL_MISCBDCGE;
		pci_write_config_dword(pci, INTEL_HDA_CGCTL, val);
	}

	snd_hdac_set_codec_wakeup(bus, false);

	/* reduce dma latency to avoid noise */
	if (HDA_CONTROLLER_IS_APL(pci)){
		bxt_reduce_dma_latency(fchip_azx);
	}

	if (bus->mlcap != NULL){
		intel_init_lctl(fchip_azx);
	}
}

static const struct snd_pci_quirk msi_allow_deny_list[] = {
	SND_PCI_QUIRK(0x103c, 0x2191, "HP", 0), /* AMD Hudson */
	SND_PCI_QUIRK(0x103c, 0x2192, "HP", 0), /* AMD Hudson */
	SND_PCI_QUIRK(0x103c, 0x21f7, "HP", 0), /* AMD Hudson */
	SND_PCI_QUIRK(0x103c, 0x21fa, "HP", 0), /* AMD Hudson */
	SND_PCI_QUIRK(0x1043, 0x81f2, "ASUS", 0), /* Athlon64 X2 + nvidia */
	SND_PCI_QUIRK(0x1043, 0x81f6, "ASUS", 0), /* nvidia */
	SND_PCI_QUIRK(0x1043, 0x822d, "ASUS", 0), /* Athlon64 X2 + nvidia MCP55 */
	SND_PCI_QUIRK(0x1179, 0xfb44, "Toshiba Satellite C870", 0), /* AMD Hudson */
	SND_PCI_QUIRK(0x1849, 0x0888, "ASRock", 0), /* Athlon64 X2 + nvidia */
	SND_PCI_QUIRK(0xa0a0, 0x0575, "Aopen MZ915-M", 0), /* ICH6 */
	{}
};

static void fchip_check_msi(struct fchip_azx *chip)
{
	const struct snd_pci_quirk *q;

	if (enable_msi >= 0) {
		chip->msi = !!enable_msi;
		return;
	}

	// enable MSI by default
	chip->msi = 1;	
	
	// if the value is found in allow_deny list, set it to this value
	q = snd_pci_quirk_lookup(chip->pci, msi_allow_deny_list);
	if (q) {
		printk(KERN_INFO "fchip: MSI for device %04x:%04x set to %d\n", q->subvendor, q->subdevice, q->value);
		chip->msi = q->value;
		return;
	}

	/* NVidia chipsets seem to cause troubles with MSI */
	if (chip->driver_caps & AZX_DCAPS_NO_MSI) {
		printk(KERN_INFO, "fchip: Disabling MSI\n");
		chip->msi = 0;
	}
}

static int fchip_first_init(struct fchip_azx *chip)
{
	int dev = chip->dev_index;
	struct pci_dev *pci = chip->pci;
	struct snd_card *card = chip->card;
	struct hdac_bus *bus = azx_to_hda_bus(chip);
	int err;
	unsigned short gcap;
	unsigned int dma_bits = 64;

#if BITS_PER_LONG != 64
	/* Fix up base address on ULI M5461 */
	if (chip->driver_type == AZX_DRIVER_ULI) {
		u16 tmp3;
		pci_read_config_word(pci, 0x40, &tmp3);
		pci_write_config_word(pci, 0x40, tmp3 | 0x10);
		pci_write_config_dword(pci, PCI_BASE_ADDRESS_1, 0);
	}
#endif

	// Fix response write request not synced to memory when handle
	// hdac interrupt on Glenfly Gpus
	
	if (chip->driver_type == AZX_DRIVER_GFHDMI)
	{
		bus->polling_mode = 1;
	}

	if (chip->driver_type == AZX_DRIVER_LOONGSON) {
		bus->polling_mode = 1;
		bus->not_use_interrupts = 1;
		bus->access_sdnctl_in_dword = 1;
	}

	err = pcim_iomap_regions(pci, 1 << 0, "ICH HD audio for filterchip");
	if (err < 0)
	{
		return err;
	}


	bus->addr = pci_resource_start(pci, 0);
	bus->remap_addr = pcim_iomap_table(pci)[0];

	if (chip->driver_type == AZX_DRIVER_SKL)
	{
		snd_hdac_bus_parse_capabilities(bus);
	}

	/*
	 * Some Intel CPUs has always running timer (ART) feature and
	 * controller may have Global time sync reporting capability, so
	 * check both of these before declaring synchronized time reporting
	 * capability SNDRV_PCM_INFO_HAS_LINK_SYNCHRONIZED_ATIME
	 */
	chip->gts_present = false;

#ifdef CONFIG_X86
	if (bus->ppcap && boot_cpu_has(X86_FEATURE_ART))
	{
		chip->gts_present = true;
	}
#endif

	if (chip->msi) {
		if (chip->driver_caps & AZX_DCAPS_NO_MSI64) {
			printk(KERN_DEBUG "fchip: Disabling 64bit MSI\n");
			pci->no_64bit_msi = true;
		}
		if (pci_enable_msi(pci) < 0){
			chip->msi = 0;
		}
	}

	// enabling the device to be the bus master
	pci_set_master(pci);

	// global capabilities (p. 28)
	gcap = fchip_readreg_w(chip, GCAP);
	printk(KERN_DEBUG "fchip: Chipset global capabilities = 0x%x\n", gcap);

	/* AMD devices support 40 or 48bit DMA, take the safe one */
	if (chip->pci->vendor == PCI_VENDOR_ID_AMD){
		dma_bits = 40;
	}

	/* disable SB600 64bit support for safety */
	if (chip->pci->vendor == PCI_VENDOR_ID_ATI) {
		struct pci_dev *p_smbus;
		dma_bits = 40;

		// iterate through known PCI devices in order to find this one:
		p_smbus = pci_get_device(PCI_VENDOR_ID_ATI,
					 PCI_DEVICE_ID_ATI_SBX00_SMBUS,
					 NULL);
		if (p_smbus) {
			if (p_smbus->revision < 0x30){
				gcap &= ~AZX_GCAP_64OK;
			}
			pci_dev_put(p_smbus);
		}
	}

	/* NVidia hardware normally only supports up to 40 bits of DMA */
	if (chip->pci->vendor == PCI_VENDOR_ID_NVIDIA)
		dma_bits = 40;

	/* disable 64bit DMA address on some devices */
	if (chip->driver_caps & AZX_DCAPS_NO_64BIT) {
		printk(KERN_DEBUG "fchip: Disabling 64bit DMA\n");
		gcap &= ~AZX_GCAP_64OK;
	}

	/* disable buffer size rounding to 128-byte multiples if supported */
	if (align_buffer_size >= 0){
		chip->align_buffer_size = !!align_buffer_size;
	}
	else {
		if (chip->driver_caps & AZX_DCAPS_NO_ALIGN_BUFSIZE){
			chip->align_buffer_size = 0;
		}
		else{
			chip->align_buffer_size = 1;
		}
	}

	/* allow 64bit DMA address if supported by H/W */
	if (!(gcap & AZX_GCAP_64OK))
	{
		dma_bits = 32;
	}

	// dma_set_mask_and_coherent is the same as calling 
	// dma_set_mask and dma_set_coherent_mask for the SAME 
	// mask (which is the case here)
	if (dma_set_mask_and_coherent(&pci->dev, DMA_BIT_MASK(dma_bits)))
	{
		dma_set_mask_and_coherent(&pci->dev, DMA_BIT_MASK(32));
	}
	dma_set_max_seg_size(&pci->dev, UINT_MAX);

	// read number of streams from GCAP register 
	// instead of using hardcoded value
	
	chip->capture_streams = (gcap >> 8) & 0x0f;
	chip->playback_streams = (gcap >> 12) & 0x0f;
	if (!chip->playback_streams && !chip->capture_streams) {
		// gcap didn't give any info, switching to 
		// old method: look at chipset type

		switch (chip->driver_type) {
		case AZX_DRIVER_ULI:
			chip->playback_streams = ULI_NUM_PLAYBACK;
			chip->capture_streams = ULI_NUM_CAPTURE;
			break;
		case AZX_DRIVER_ATIHDMI:
		case AZX_DRIVER_ATIHDMI_NS:
			chip->playback_streams = ATIHDMI_NUM_PLAYBACK;
			chip->capture_streams = ATIHDMI_NUM_CAPTURE;
			break;
		case AZX_DRIVER_GFHDMI:
		case AZX_DRIVER_GENERIC:
		default:
			chip->playback_streams = ICH6_NUM_PLAYBACK;
			chip->capture_streams = ICH6_NUM_CAPTURE;
			break;
		}
	}

	// similar to p.34 intel hda spec:
	chip->capture_index_offset = 0;
	chip->playback_index_offset = chip->capture_streams;
	chip->num_streams = chip->playback_streams + chip->capture_streams;

	// sanity check for the SDxCTL.STRM field overflow
	if (chip->num_streams > 15 &&
	    (chip->driver_caps & AZX_DCAPS_SEPARATE_STREAM_TAG) == 0) {
		printk(KERN_WARNING "fchip: number of I/O streams is %d, forcing separate stream tags", chip->num_streams);
		chip->driver_caps |= AZX_DCAPS_SEPARATE_STREAM_TAG;
	}

	// initialize SDs
	err = fchip_init_streams(chip);
	if (err < 0){
		return err;
	}

	err = fchip_alloc_stream_pages(chip);
	if (err < 0){
		return err;
	}
		

	/* initialize chip */
	fchip_init_pci(chip);

	snd_hdac_i915_set_bclk(bus);

	fchip_hda_intel_init_chip(chip, (probe_only[dev] & 2) == 0);

	/* codec detection */
	if (!azx_to_hda_bus(chip)->codec_mask) {
		printk(KERN_ERR "no codecs found!\n");
		/* keep running the rest for the runtime PM */
	}

	if (fchip_acquire_irq(chip, 0) < 0){
		return -EBUSY;
	}



    strcpy(card->driver, FCHIP_DRIVER_NAME);
    strcpy(card->shortname, FCHIP_DRIVER_SHORTNAME);
    sprintf(card->longname, "%s at 0x%lx irq %i", card->shortname, bus->addr, bus->irq);

	return 0;
}

static void fchip_probe_work(struct work_struct *work)
{
	struct fchip_hda_intel *hda = container_of(work, struct fchip_hda_intel, probe_work.work);
	fchip_probe_continue(&hda->chip);
}


static int fchip_position_check(struct fchip_azx *chip, struct azx_dev *azx_dev)
{
	return 0;
}

static const struct hda_controller_ops fchip_pci_hda_ops = {
	.disable_msi_reset_irq = fchip_disable_msi_reset_irq,
	.position_check = fchip_position_check,
};

// CHIP ctor
static int fchip_create(
    struct snd_card* card,
    struct pci_dev* pci, 
	int dev,
	int driver_caps,
    struct fchip** rchip
)
{
    printk(KERN_DEBUG "fchip: Chip create called\n");
    static struct snd_device_ops ops = {
		.dev_disconnect = fchip_dev_disconnect,
        .dev_free = fchip_dev_free,
    };
	struct fchip_hda_intel* fchip_hda;
	struct fchip_azx* fchip_azx;
    struct fchip* fchip;
    int err;

    *rchip = NULL;


    // Initializing the pci entry. Using pcim, so that we 
	// do not have to disable the device ourselves
    err = pcim_enable_device(pci);
    if(err<0){
        return err;
    }

	// using a managed version of kzalloc 
	// (to not free the memory ourselves)
	// first allocate memory for fchip instance, 
	// then - for hda_intel instance
    fchip = devm_kzalloc(&pci->dev, sizeof(*fchip), GFP_KERNEL);
	if(fchip == NULL){
        // pci_disable_device(pci);
		return -ENOMEM;
	}

	fchip_hda = devm_kzalloc(&pci->dev, sizeof(*fchip_hda), GFP_KERNEL);
    if(fchip_hda==NULL){
        // pci_disable_device(pci);
        return -ENOMEM;
    }

	fchip_azx = &fchip_hda->chip;
	fchip->azx_chip = fchip_azx;
	mutex_init(&fchip_azx->open_mutex);
    fchip_azx->card = card;
    fchip_azx->pci = pci;
	fchip_azx->ops = &fchip_pci_hda_ops;
	fchip_azx->driver_caps = driver_caps;
	fchip_azx->driver_type = driver_caps & 0xff;
	
	fchip_check_msi(fchip_azx);
	fchip_azx->dev_index = dev;

	if(jackpoll_ms[dev]>=50 && jackpoll_ms[dev]<=60000){
		fchip_azx->jackpoll_interval = msecs_to_jiffies(jackpoll_ms[dev]);
	}



	INIT_LIST_HEAD(&fchip_azx->pcm_list);
	INIT_WORK(&fchip_hda->irq_pending_work, fchip_irq_pending_work);
	INIT_LIST_HEAD(&fchip_hda->list);
	
	fchip_init_vga_switcheroo(fchip_azx);
	init_completion(&fchip_hda->probe_wait);

	assign_position_fix(fchip_azx, check_position_fix(fchip_azx, position_fix[dev]));

	if (single_cmd < 0) 
	{
		// allow on errors
		fchip_azx->fallback_to_single_cmd = 1; 	
	}
	else
	{
		fchip_azx->single_cmd = single_cmd;
	} 

	fchip_check_snoop_available(fchip_azx);

	if (bdl_pos_adj[dev] < 0)
	{
		fchip_azx->bdl_pos_adj = default_bdl_pos_adj(fchip_azx);
	}
	else
	{
		fchip_azx->bdl_pos_adj = bdl_pos_adj[dev];
	}

	err = fchip_bus_init(fchip_azx, model[dev]);
	if (err < 0)
	{
		return err;
	}

	/* use the non-cached pages in non-snoop mode */
	if (!fchip_snoop(fchip_azx))
	{
		azx_to_hda_bus(fchip_azx)->dma_type = SNDRV_DMA_TYPE_DEV_WC;
	}

	if (fchip_azx->driver_type == AZX_DRIVER_NVIDIA) 
	{
		printk(KERN_DEBUG "fchip: Enable delay in RIRB handling\n");
		fchip_azx->bus.core.needs_damn_long_delay = 1;
	}

	fchip_check_probe_mask(fchip_azx, dev);

	err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, fchip_azx, &ops);
	if (err < 0) {
		printk(KERN_ERR "fchip: Error creating device!\n", pci->dev.id);
		
		fchip_free(fchip_azx);
		return err;
	}

	/* continue probing in work context as may trigger request module */
	INIT_DELAYED_WORK(&fchip_hda->probe_work, fchip_probe_work);

	*rchip = fchip;

	return 0;
}

int fchip_probe_continue(struct fchip_azx *chip);

#ifdef CONFIG_SND_HDA_PATCH_LOADER
static void fchip_firmware_cb(const struct firmware *fw, void *context)
{
	struct snd_card *card = context;
	struct fchip_azx *fchip_azx = ((struct fchip*)(card->private_data))->azx_chip;

	if (fw){
		fchip_azx->fw = fw;
	}
	else
		printk(KERN_ERR "fchip: Cannot load firmware, continue without patching\n");
	if (!fchip_azx->disabled) {
		//continue probing
		fchip_probe_continue(fchip_azx);
	}
}
#endif // CONFIG_SND_HDA_PATCH_LOADER


int fchip_probe_continue(struct fchip_azx *fchip_azx)
{
	struct fchip_hda_intel *hda = container_of(fchip_azx, struct fchip_hda_intel, chip);
	struct hdac_bus *bus = azx_to_hda_bus(fchip_azx);
	struct pci_dev *pci = fchip_azx->pci;
	int dev = fchip_azx->dev_index;
	int err;

	if (fchip_azx->disabled || hda->init_failed)
	{
		return -EIO;
	}
	if (hda->probe_retry)
	{
		goto probe_retry;
	}

	to_hda_bus(bus)->bus_probing = 1;
	hda->probe_continued = 1;

	/* Request display power well for the HDA controller or codec. For
	 * Haswell/Broadwell, both the display HDA controller and codec need
	 * this power. For other platforms, like Baytrail/Braswell, only the
	 * display codec needs the power and it can be released after probe.
	 */
	fchip_display_power(fchip_azx, true);

	err = fchip_first_init(fchip_azx);
	if (err < 0){
		goto out_free;
	}

#ifdef CONFIG_SND_HDA_INPUT_BEEP
	fchip_azx->beep_mode = beep_mode[dev];
#endif

	fchip_azx->ctl_dev_id = ctl_dev_id;

	// create codec instances
	if (bus->codec_mask) {
		err = fchip_probe_codecs(fchip_azx, azx_max_codecs[fchip_azx->driver_type]);
		if (err < 0){
			goto out_free;
		}
	}

#ifdef CONFIG_SND_HDA_PATCH_LOADER
	if (fchip_azx->fw) {
		err = snd_hda_load_patch(&fchip_azx->bus, fchip_azx->fw->size,
					 fchip_azx->fw->data);
		if (err < 0){
			goto out_free;
		}
	}
#endif

 probe_retry:
	if (bus->codec_mask && !(probe_only[dev] & 1)) {
		err = fchip_codec_configure(fchip_azx);
		if (err) {
			if ((fchip_azx->driver_caps & AZX_DCAPS_RETRY_PROBE) &&
			    ++hda->probe_retry < 60) {
				schedule_delayed_work(&hda->probe_work,
						      msecs_to_jiffies(1000));
				return 0; /* keep things up */
			}
			printk(KERN_ERR "fchip: Cannot probe codecs, giving up\n");
			goto out_free;
		}
	}

	err = snd_card_register(fchip_azx->card);
	if (err < 0){
		goto out_free;
	}

	fchip_setup_vga_switcheroo_runtime_pm(fchip_azx);

	fchip_azx->running = 1;
	fchip_add_card_list(fchip_azx);

	fchip_set_default_power_save(fchip_azx);

	if (fchip_has_pm_runtime(fchip_azx)) {
		pm_runtime_use_autosuspend(&pci->dev);
		pm_runtime_allow(&pci->dev);
		pm_runtime_put_autosuspend(&pci->dev);
	}

out_free:
	if (err < 0) {
		pci_set_drvdata(pci, NULL);
		snd_card_free(fchip_azx->card);
		return err;
	}

	if (!hda->need_i915_power)
	{
		fchip_display_power(fchip_azx, false);
	}
	complete_all(&hda->probe_wait);
	to_hda_bus(bus)->bus_probing = 0;
	hda->probe_retry = 0;
	return 0;
}

// DRIVER ctor
static int fchip_probe
(
    struct pci_dev* pci,
    const struct pci_device_id* pci_id
)
{
    printk(KERN_DEBUG "fchip: Probe called on device %x:%x\n", pci_id->vendor, pci_id->device);
    static int dev;
    struct snd_card* card;
	struct fchip_hda_intel *hda;
    struct fchip* fchip;
	struct fchip_azx* fchip_azx;
	bool schedule_probe;
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
		printk(KERN_ERR "fchip: Error creating card\n");
        return err;
    }



// driver_data will contain the data declared in the PCI ID table. 
// The capabilities are defined there.
    err = fchip_create(card, pci, dev, pci_id->driver_data, &fchip);
    if(err<0){
		pci_set_drvdata(pci, NULL);
        snd_card_free(card);
        return err;
    }
	card->private_data = fchip;
	fchip_azx = fchip->azx_chip;
	hda = container_of(fchip_azx, struct fchip_hda_intel, chip);

	pci_set_drvdata(pci, card);

#ifdef CONFIG_SND_HDA_I915
	/* bind with i915 if needed */
	if (fchip_azx->driver_caps & AZX_DCAPS_I915_COMPONENT) {
		err = snd_hdac_i915_init(azx_to_hda_bus(fchip_azx));
		if (err < 0) {
			if (err == -EPROBE_DEFER){
				pci_set_drvdata(pci, NULL);
        		snd_card_free(card);
        		return err;
			}

			/* if the controller is bound only with HDMI/DP
			 * (for HSW and BDW), we need to abort the probe;
			 * for other chips, still continue probing as other
			 * codecs can be on the same link.
			 */
			if (HDA_CONTROLLER_IN_GPU(pci)) {
				printk(KERN_ERR "fchip: HSW/BDW HD-audio HDMI/DP requires binding with gfx driver\n");

				pci_set_drvdata(pci, NULL);
        		snd_card_free(card);
        		return err;
			} 
			else {
				/* don't bother any longer */
				fchip_azx->driver_caps &= ~AZX_DCAPS_I915_COMPONENT;
			}
		}

		/* HSW/BDW controllers need this power */
		if (HDA_CONTROLLER_IN_GPU(pci)){
			hda->need_i915_power = true;
		}
	}
#else
	if (HDA_CONTROLLER_IN_GPU(pci))
	{
		printk(KERN_ERR "fchip: Haswell/Broadwell HDMI/DP must build in CONFIG_SND_HDA_I915\n");
	}
#endif // CONFIG_SND_HDA_I915

	// not sure if the vga is required here, but:
	err = fchip_register_vga_switcheroo(fchip_azx);
	if (err < 0) {
		printk(KERN_ERR "fchip: Error registering vga_switcheroo client\n");
	
		pci_set_drvdata(pci, NULL);
		snd_card_free(card);
		return err;
	}

	if (fchip_check_hdmi_disabled(pci)) {
		printk(KERN_ERR "fchip: VGA controller is disabled\n");
		printk(KERN_ERR "fchip: Delaying initialization\n");
		fchip_azx->disabled = true;
	}

	schedule_probe = !fchip_azx->disabled;

#ifdef CONFIG_SND_HDA_PATCH_LOADER
	if (patch[dev] && *patch[dev]) {
		printk(KERN_INFO "fchip: Applying patch firmware '%s'\n", patch[dev]);
		err = request_firmware_nowait(
			THIS_MODULE, true, patch[dev], &pci->dev, GFP_KERNEL, card,
			fchip_firmware_cb
		);
		if (err < 0){
			pci_set_drvdata(pci, NULL);
			snd_card_free(card);
			return err;
		}
		schedule_probe = false; /* continued in azx_firmware_cb() */
	}
#endif // CONFIG_SND_HDA_PATCH_LOADER

	if (schedule_probe)
	{
		schedule_delayed_work(&hda->probe_work, 0);
	}

	if (fchip_azx->disabled){
		complete_all(&hda->probe_wait);
	}
	return 0;


	
    // err = snd_filterchip_new_pcm(chip);
    // if(err<0){
    //     snd_card_free(card);
    //     return err;
    // }

    // strcpy(card->driver, FCHIP_DRIVER_NAME);
    // strcpy(card->shortname, FCHIP_DRIVER_SHORTNAME);
    // sprintf(card->longname, "%s at 0x%lx irq %i", card->shortname, chip->port, chip->irq);

    // // todo later (PCM goes here?)

    // err = snd_card_register(card);
    // if(err<0){
    //     snd_card_free(card);
    //     return err;
    // }

    // // we do it, so that later we can free the chip, 
    // // as only pci_dev struct is passed to the dtor.
    // // that way, we're able to free the card by acquiring
    // // it from the drvdata of the pci_dev struct
    // pci_set_drvdata(pci, card);
    // dev++;
    // return 0;
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
    .probe = fchip_probe,
    .remove = fchip_remove,
	.shutdown = fchip_shutdown,
	.driver = { .pm = NULL }
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

// // interrupt handler 
// static irqreturn_t snd_filterchip_interrupt(int irq, void* dev_id){
//     printk(KERN_DEBUG "fchip: Interrupt handler called\n");
//     struct fchip* chip = dev_id;
//     // todo
//     return IRQ_HANDLED;
// }

// // CHIP dtor
// static int snd_filterchip_free(struct fchip* chip)
// {
//     printk(KERN_DEBUG "fchip: Chip free called\n");

//     if(chip->irq >= 0){
//         free_irq(chip->irq, chip);
//     }

//     pci_release_regions(chip->pci);
//     pci_disable_device(chip->pci);
//     kfree(chip);
//     return 0;
// }
// // CHIP dtor
// static void snd_filterchip_remove(struct pci_dev* pci){
//     printk(KERN_DEBUG "fchip: Chip remove called\n");
//     snd_card_free(pci_get_drvdata(pci));
//     pci_set_drvdata(pci, NULL);
// }