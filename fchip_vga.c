#include "fchip_vga.h"

struct pci_dev* get_bound_vga(struct pci_dev *pci)
{
	struct pci_dev *p;

	/* check only discrete GPU */
	switch (pci->vendor) {
	case PCI_VENDOR_ID_ATI:
	case PCI_VENDOR_ID_AMD:
		if (pci->devfn == 1) {
			p = pci_get_domain_bus_and_slot(pci_domain_nr(pci->bus),
							pci->bus->number, 0);
			if (p) {
				/* ATPX is in the integrated GPU's ACPI namespace
				 * rather than the dGPU's namespace. However,
				 * the dGPU is the one who is involved in
				 * vgaswitcheroo.
				 */
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
	return false;
}

void fchip_init_vga_switcheroo(struct fchip_azx *chip)
{
	struct fchip_hda_intel *hda = container_of(chip, struct fchip_hda_intel, chip);
	struct pci_dev *p = get_bound_vga(chip->pci);
	struct pci_dev *parent;
	
	if (p) {
		printk(KERN_INFO "fchip: Handle vga_switcheroo audio client\n");
		hda->use_vga_switcheroo = 1;

		/* cleared in either gpu_bound op or codec probe, or when its
		 * upstream port has _PR3 (i.e. dGPU).
		 */
		parent = pci_upstream_bridge(p);
		chip->bus.keep_power = parent ? !pci_pr3_present(parent) : 1;
		chip->driver_caps |= AZX_DCAPS_PM_RUNTIME;
		pci_dev_put(p);
	}
}
static void fchip_setup_vga_switcheroo_runtime_pm(struct fchip_azx *chip)
{}

int fchip_register_vga_switcheroo(struct fchip_azx *chip)
{
	return 0;
}


