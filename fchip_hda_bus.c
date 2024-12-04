#include "fchip_hda_bus.h"
#include "fchip_posfix.h"

static unsigned int fchip_command_addr(u32 cmd)
{
	unsigned int addr = cmd >> 28;

	if (addr >= FCHIP_AZX_MAX_CODECS) {
		snd_BUG();
		addr = 0;
	}

	return addr;
}

// send cmd callbacks
static int fchip_single_wait_for_response(struct fchip_azx *fchip_azx, unsigned int addr)
{
	int timeout = 50;

	while (timeout--) {
		// check IRV (immediate result valid) bit
		if (fchip_readreg_w(fchip_azx, IRS) & AZX_IRS_VALID) {
			// reuse rirb.res as the response return value
			azx_to_hda_bus(fchip_azx)->rirb.res[addr] = fchip_readreg_l(fchip_azx, IR);
			return 0;
		}
		udelay(1);
	}
	if (printk_ratelimit()){
		printk(KERN_DEBUG "fchip: get_response timeout: IRS=0x%x\n",
			fchip_readreg_w(fchip_azx, IRS));
    }
	azx_to_hda_bus(fchip_azx)->rirb.res[addr] = -1;
	return -EIO;
}

static int fchip_single_send_cmd(struct hdac_bus *bus, u32 val)
{
	struct fchip_azx* fchip_azx = hdac_bus_to_azx(bus);
	unsigned int addr = fchip_command_addr(val);
	int timeout = 50;

	bus->last_cmd[fchip_command_addr(val)] = val;
	while (timeout--) {
		// check ICB (immediate command busy) bit, see p.52
		if (!((fchip_readreg_w(fchip_azx, IRS) & AZX_IRS_BUSY))) {
			// Clear IRV valid bit
			fchip_writereg_w(fchip_azx, IRS, fchip_readreg_w(fchip_azx, IRS) |
				   AZX_IRS_VALID);
			fchip_writereg_l(fchip_azx, IC, val);
			fchip_writereg_w(fchip_azx, IRS, fchip_readreg_w(fchip_azx, IRS) |
				   AZX_IRS_BUSY);
			return fchip_single_wait_for_response(fchip_azx, addr);
		}
		udelay(1);
	}
	if (printk_ratelimit()){
		printk(KERN_DEBUG "fchip: send_cmd timeout: IRS=0x%x, val=0x%x\n",
			fchip_readreg_w(fchip_azx, IRS), val);
    }
	return -EIO;
}

int fchip_send_cmd(struct hdac_bus *bus, unsigned int val)
{
	struct fchip_azx *fchip_azx = hdac_bus_to_azx(bus);

	if (fchip_azx->disabled){
		return 0;
    }

	if (fchip_azx->single_cmd || bus->use_pio_for_commands){
        return fchip_single_send_cmd(bus, val);    
    }
	else{
		return snd_hdac_bus_send_cmd(bus, val);
    }
}


// read response callbacks
static int fchip_single_get_response(struct hdac_bus *bus, unsigned int addr,
				   unsigned int *res)
{
	if (res){
		*res = bus->rirb.res[addr];
    }
	return 0;
}

static int fchip_rirb_get_response(struct hdac_bus *bus, unsigned int addr,
				 unsigned int *res)
{
	struct fchip_azx* fchip_azx = hdac_bus_to_azx(bus);
	struct hda_bus* hbus = &fchip_azx->bus;
	int err;

 again:
	err = snd_hdac_bus_get_response(bus, addr, res);
	if (!err){
		return 0;
    }

	if (hbus->no_response_fallback){
		return -EIO;
    }

	if (!bus->polling_mode) {
		printk(KERN_WARNING	"fchip: fchip_get_response timeout, switching to polling mode: last cmd=0x%08x\n",
			 bus->last_cmd[addr]);
		bus->polling_mode = 1;
		goto again;
	}

	if (fchip_azx->msi) {
		printk(KERN_WARNING "fchip: No response from codec, disabling MSI: last cmd=0x%08x\n",
			 bus->last_cmd[addr]);
		if (fchip_azx->ops->disable_msi_reset_irq &&
		    fchip_azx->ops->disable_msi_reset_irq(fchip_azx) < 0)
        {
			return -EIO;
        }
		goto again;
	}

	if (fchip_azx->probing) {
		// If this critical timeout happens during the codec probing
		// phase, this is likely an access to a non-existing codec
		// slot.  Better to return an error and reset the system.
		return -EIO;
	}

	// no fallback mechanism?
	if (!fchip_azx->fallback_to_single_cmd){
		return -EIO;
	}

	// a fatal communication error; need either to reset or to fallback
	// to the single_cmd mode
	if (hbus->allow_bus_reset && !hbus->response_reset && !hbus->in_reset) {
		hbus->response_reset = 1;
		printk(KERN_ERR "fchip: No response from codec, resetting bus: last cmd=0x%08x\n",
			bus->last_cmd[addr]);
		return -EAGAIN; /* give a chance to retry */
	}

	printk(KERN_ERR "fchip: fchip_get_response timeout, switching to single_cmd mode: last cmd=0x%08x\n",
		bus->last_cmd[addr]);
	fchip_azx->single_cmd = 1;
	hbus->response_reset = 0;
	snd_hdac_bus_stop_cmd_io(bus);
	return -EIO;
}


int fchip_get_response(struct hdac_bus *bus, unsigned int addr, unsigned int *res)
{
	struct fchip_azx *fchip_azx = hdac_bus_to_azx(bus);

	if (fchip_azx->disabled){
		return 0;
    }
	if (fchip_azx->single_cmd || bus->use_pio_for_commands){
		return fchip_single_get_response(bus, addr, res);
    }
	else{
		return fchip_rirb_get_response(bus, addr, res);
    }
}

static const struct hdac_bus_ops bus_core_ops = {
	.command = fchip_send_cmd,
	.get_response = fchip_get_response,
};

// hda bus initialization
int fchip_bus_init(struct fchip_azx* fchip_azx, const char* model)
{
	struct hda_bus *bus = &fchip_azx->bus;
	int err;

    // adding the methods for communication with CORB/RIRB
	err = snd_hdac_bus_init(&bus->core, fchip_azx->card->dev, &bus_core_ops);
	if (err < 0){
		return err;
    }

	bus->card = fchip_azx->card;
	mutex_init(&bus->prepare_mutex);
	bus->pci = fchip_azx->pci;
	bus->modelname = model;
	bus->mixer_assigned = -1;
	bus->core.snoop = fchip_snoop(fchip_azx);
	if (fchip_azx->get_position[0] != fchip_get_pos_lpib ||
	    fchip_azx->get_position[1] != fchip_get_pos_lpib)
    {
		bus->core.use_posbuf = true;
    }
	bus->core.bdl_pos_adj = fchip_azx->bdl_pos_adj;
	if (fchip_azx->driver_caps & AZX_DCAPS_CORBRP_SELF_CLEAR){
		bus->core.corbrp_self_clear = true;
    }

	if (fchip_azx->driver_caps & AZX_DCAPS_4K_BDLE_BOUNDARY){
		bus->core.align_bdle_4k = true;
    }

// PIO - programmable I/O. see immediate c/i registers p.50
	if (fchip_azx->driver_caps & AZX_DCAPS_PIO_COMMANDS){
		bus->core.use_pio_for_commands = true;
    }

	// enable sync_write flag for stable communication as default
	bus->core.sync_write = 1;

	return 0;
}