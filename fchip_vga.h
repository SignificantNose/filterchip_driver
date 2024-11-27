#pragma once
#include "fchip.h"

void fchip_init_vga_switcheroo(struct fchip_azx *chip);
void fchip_setup_vga_switcheroo_runtime_pm(struct fchip_azx *chip);
int fchip_register_vga_switcheroo(struct fchip_azx *chip);

bool fchip_check_hdmi_disabled(struct pci_dev *pci);