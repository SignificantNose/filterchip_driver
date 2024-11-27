#pragma once
#include "fchip.h"

#define hdac_bus_to_azx(_bus)	container_of(_bus, struct fchip_azx, bus.core)


int fchip_bus_init(struct fchip_azx* fchip_azx, const char* model);