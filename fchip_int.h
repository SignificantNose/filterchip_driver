#pragma once
#include "fchip.h"

#define azx_dev_to_hdac_stream(dev)		(&(dev)->core)
#define hdac_stream_to_azx_dev(s)	container_of(s, struct azx_dev, core)


int fchip_acquire_irq(struct fchip_azx *fchip_azx, int do_disconnect);