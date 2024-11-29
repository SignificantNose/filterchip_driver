#pragma once
#include "fchip.h"

#define azx_dev_to_hdac_stream(dev)		(&(dev)->core)
#define hdac_stream_to_azx_dev(s)	container_of(s, struct azx_dev, core)


int fchip_acquire_irq(struct fchip_azx *fchip_azx, int do_disconnect);
void fchip_clear_irq_pending(struct fchip_azx* fchip_azx);
int fchip_disable_msi_reset_irq(struct fchip_azx *chip);
void fchip_irq_pending_work(struct work_struct *work);