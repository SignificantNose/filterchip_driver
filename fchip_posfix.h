#pragma once
#include "fchip.h"
// this module is responsible for assigning callbacks
// that return the current DMA position in the controller
// based on the device

#define AMD_FIFO_SIZE	32

enum {
	POS_FIX_AUTO,
	POS_FIX_LPIB,
	POS_FIX_POSBUF,
	POS_FIX_VIACOMBO,
	POS_FIX_COMBO,
	POS_FIX_SKL,
	POS_FIX_FIFO,
};

unsigned int fchip_get_pos_lpib(struct fchip_azx *chip, struct azx_dev *azx_dev);

void assign_position_fix(struct fchip_azx *chip, int fix);
int check_position_fix(struct fchip_azx *chip, int fix);