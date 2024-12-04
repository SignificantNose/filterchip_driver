#include "fchip_vga.h"


#ifdef SUPPORT_VGA_SWITCHEROO

#ifdef CONFIG_ACPI
// ATPX is in the integrated GPU's namespace
static bool atpx_present(void)
{
	struct pci_dev *pdev = NULL;
	acpi_handle dhandle, atpx_handle;
	acpi_status status;

	while ((pdev = pci_get_base_class(PCI_BASE_CLASS_DISPLAY, pdev))) {
		if ((pdev->class != PCI_CLASS_DISPLAY_VGA << 8) &&
		    (pdev->class != PCI_CLASS_DISPLAY_OTHER << 8))
			continue;

		dhandle = ACPI_HANDLE(&pdev->dev);
		if (dhandle) {
			status = acpi_get_handle(dhandle, "ATPX", &atpx_handle);
			if (ACPI_SUCCESS(status)) {
				pci_dev_put(pdev);
				return true;
			}
		}
	}
	return false;
}
#else
#define atpx_present() false
#endif


struct pci_dev* get_bound_vga(struct pci_dev *pci)
{
	struct pci_dev *p;

	// check only discrete GPU
	switch (pci->vendor) {
	case PCI_VENDOR_ID_ATI:
	case PCI_VENDOR_ID_AMD:
		if (pci->devfn == 1) {
			p = pci_get_domain_bus_and_slot(pci_domain_nr(pci->bus),
							pci->bus->number, 0);
			if (p) {
				// ATPX is in the integrated GPU's ACPI namespace
				// rather than the dGPU's namespace. However,
				// the dGPU is the one who is involved in
				// vgaswitcheroo.
				if (((p->class >> 16) == PCI_BASE_CLASS_DISPLAY) &&
				    (atpx_present() || apple_gmux_detect(NULL, NULL)))
					return p;
				pci_dev_put(p);
			}
		}
		break;
	case PCI_VENDOR_ID_NVIDIA:
		if (pci->devfn == 1) {
			p = pci_get_domain_bus_and_slot(pci_domain_nr(pci->bus),
							pci->bus->number, 0);
			if (p) {
				if ((p->class >> 16) == PCI_BASE_CLASS_DISPLAY)
					return p;
				pci_dev_put(p);
			}
		}
		break;
	}
	return NULL;
}

bool fchip_check_hdmi_disabled(struct pci_dev *pci)
{
	bool vga_inactive = false;
	struct pci_dev *p = get_bound_vga(pci);

	if (p) {
		if (vga_switcheroo_get_client_state(p) == VGA_SWITCHEROO_OFF){
			vga_inactive = true;
		}
		pci_dev_put(p);
	}
	return vga_inactive;
}

void fchip_init_vga_switcheroo(struct fchip_azx *chip)
{
	struct fchip_hda_intel *hda = container_of(chip, struct fchip_hda_intel, chip);
	struct pci_dev *p = get_bound_vga(chip->pci);
	struct pci_dev *parent;
	
	if (p) {
		printk(KERN_INFO "fchip: Handle vga_switcheroo audio client\n");
		hda->use_vga_switcheroo = 1;

		// cleared in either gpu_bound op or codec probe, or when its
		// upstream port has _PR3 (i.e. dGPU).
		parent = pci_upstream_bridge(p);
		chip->bus.keep_power = parent ? !pci_pr3_present(parent) : 1;
		chip->driver_caps |= AZX_DCAPS_PM_RUNTIME;
		pci_dev_put(p);
	}
}
void fchip_setup_vga_switcheroo_runtime_pm(struct fchip_azx *fchip_azx)
{
	struct fchip_hda_intel* hda = container_of(fchip_azx, struct fchip_hda_intel, chip);
	struct hda_codec *codec;

	if (hda->use_vga_switcheroo && !needs_eld_notify_link(fchip_azx)) {
		list_for_each_codec(codec, &fchip_azx->bus){
			codec->auto_runtime_pm = 1;
		}
		// reset the power save setup
		if (fchip_azx->running){
			fchip_set_default_power_save(fchip_azx);
		}
	}
}




static void fchip_vs_set_state(struct pci_dev *pci,
			     enum vga_switcheroo_state state)
{
	struct snd_card* card = pci_get_drvdata(pci);
	struct fchip_azx* fchip_azx = ((struct fchip*)(card->private_data))->azx_chip;
	struct fchip_hda_intel* hda = container_of(fchip_azx, struct fchip_hda_intel, chip);
	struct hda_codec* codec;
	bool disabled;

	wait_for_completion(&hda->probe_wait);
	if (hda->init_failed){
		return;
	}

	disabled = (state == VGA_SWITCHEROO_OFF);
	if (fchip_azx->disabled == disabled){
		return;
	}

	if (!hda->probe_continued) {
		fchip_azx->disabled = disabled;
		if (!disabled) {
			printk(KERN_INFO "fchip: Start delayed initialization\n");
			if (fchip_probe_continue(fchip_azx) < 0){
				printk(KERN_ERR "fchip: Initialization error\n");
			}
		}
	} 
	else {
		printk(KERN_INFO, "fchip: %s via vga_switcheroo\n", disabled ? "Disabling" : "Enabling");
		if (disabled) {
			list_for_each_codec(codec, &fchip_azx->bus) {
				pm_runtime_suspend(hda_codec_dev(codec));
				pm_runtime_disable(hda_codec_dev(codec));
			}
			pm_runtime_suspend(card->dev);
			pm_runtime_disable(card->dev);
			/* when we get suspended by vga_switcheroo we end up in D3cold,
			 * however we have no ACPI handle, so pci/acpi can't put us there,
			 * put ourselves there */
			pci->current_state = PCI_D3cold;
			fchip_azx->disabled = true;
			if (snd_hda_lock_devices(&fchip_azx->bus)){
				printk(KERN_WARNING "fchip: Cannot lock devices!\n");
			}
		} 
		else {
			snd_hda_unlock_devices(&fchip_azx->bus);
			fchip_azx->disabled = false;
			pm_runtime_enable(card->dev);
			list_for_each_codec(codec, &fchip_azx->bus) {
				pm_runtime_enable(hda_codec_dev(codec));
				pm_runtime_resume(hda_codec_dev(codec));
			}
		}
	}
}

static bool fchip_vs_can_switch(struct pci_dev *pci)
{
	struct snd_card* card = pci_get_drvdata(pci);
	struct fchip_azx* fchip_azx = ((struct fchip*)(card->private_data))->azx_chip;
	struct fchip_hda_intel* hda = container_of(fchip_azx, struct fchip_hda_intel, chip);

	wait_for_completion(&hda->probe_wait);
	if (hda->init_failed){
		return false;
	}
	if (fchip_azx->disabled || !hda->probe_continued){
		return true;
	}
	if (snd_hda_lock_devices(&fchip_azx->bus)){
		return false;
	}
	snd_hda_unlock_devices(&fchip_azx->bus);
	return true;
}

static void fchip_vs_gpu_bound(struct pci_dev *pci,
			     enum vga_switcheroo_client_id client_id)
{
	struct snd_card* card = pci_get_drvdata(pci);
	struct fchip_azx* fchip_azx = ((struct fchip*)(card->private_data))->azx_chip;

	if (client_id == VGA_SWITCHEROO_DIS){
		fchip_azx->bus.keep_power = 0;
	}
	fchip_setup_vga_switcheroo_runtime_pm(fchip_azx);
}

static const struct vga_switcheroo_client_ops fchip_vga_switcheroo_ops = {
	.set_gpu_state = fchip_vs_set_state,
	.can_switch = fchip_vs_can_switch,
	.gpu_bound = fchip_vs_gpu_bound,
};



int fchip_register_vga_switcheroo(struct fchip_azx *chip)
{
	struct fchip_hda_intel *hda = container_of(chip, struct fchip_hda_intel, chip);
	struct pci_dev *p;
	int err;

	if (!hda->use_vga_switcheroo){
		return 0;
	}

	p = get_bound_vga(chip->pci);
	err = vga_switcheroo_register_audio_client(chip->pci, &fchip_vga_switcheroo_ops, p);
	pci_dev_put(p);

	if (err < 0){
		return err;
	}
	hda->vga_switcheroo_registered = 1;

	return 0;
}
#endif