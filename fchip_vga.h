#pragma once
#include "fchip.h"
#include <linux/vga_switcheroo.h>
#include <linux/apple-gmux.h>

#if defined(CONFIG_PM) && defined(CONFIG_VGA_SWITCHEROO)
#if IS_ENABLED(CONFIG_SND_HDA_CODEC_HDMI)
#define SUPPORT_VGA_SWITCHEROO
#endif
#endif


#ifdef SUPPORT_VGA_SWITCHEROO
void fchip_init_vga_switcheroo(struct fchip_azx *chip);
void fchip_setup_vga_switcheroo_runtime_pm(struct fchip_azx *chip);
int fchip_register_vga_switcheroo(struct fchip_azx *chip);

bool fchip_check_hdmi_disabled(struct pci_dev *pci);

#define needs_eld_notify_link(chip)	((chip)->bus.keep_power)

#else
#define fchip_init_vga_switcheroo(chip)		/* NOP */
#define fchip_setup_vga_switcheroo_runtime_pm(chip)	/* NOP */
#define fchip_register_vga_switcheroo(chip)		0

#define fchip_check_hdmi_disabled(pci)	false

#endif
