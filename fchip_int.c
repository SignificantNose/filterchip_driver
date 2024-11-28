#include "fchip_int.h"
#include "fchip_hda_bus.h"

static void stream_update(struct hdac_bus *bus, struct hdac_stream *s)
{
	struct fchip_azx* fchip_azx = hdac_bus_to_azx(bus);
	struct azx_dev *azx_dev = hdac_stream_to_azx_dev(s);

	/* check whether this IRQ is really acceptable */
	if (!fchip_azx->ops->position_check ||
	    fchip_azx->ops->position_check(fchip_azx, azx_dev)) 
    {
		spin_unlock(&bus->reg_lock);
		snd_pcm_period_elapsed(azx_dev_to_hdac_stream(azx_dev)->substream);
		spin_lock(&bus->reg_lock);
	}
}

irqreturn_t fchip_interrupt(int irq, void *dev_id)
{
    struct fchip_azx* fchip_azx = dev_id;
	struct hdac_bus* bus = azx_to_hda_bus(fchip_azx);
	u32 status;
	bool active, handled = false;
	int repeat = 0; /* count for avoiding endless loop */

	if (fchip_has_pm_runtime(fchip_azx)){
		if (!pm_runtime_active(fchip_azx->card->dev)){
			return IRQ_NONE;
        }
    }

	spin_lock(&bus->reg_lock);

	if (fchip_azx->disabled){
		goto unlock;
    }

	do {
		status = fchip_readreg_l(fchip_azx, INTSTS);
		if (status == 0 || status == 0xffffffff){
			break;
        }

		handled = true;
		active = false;
		if (snd_hdac_bus_handle_stream_irq(bus, status, stream_update)){
			active = true;
        }

		status = fchip_readreg_b(fchip_azx, RIRBSTS);
		if (status & RIRB_INT_MASK) {
			/*
			 * Clearing the interrupt status here ensures that no
			 * interrupt gets masked after the RIRB wp is read in
			 * snd_hdac_bus_update_rirb. This avoids a possible
			 * race condition where codec response in RIRB may
			 * remain unserviced by IRQ, eventually falling back
			 * to polling mode in fchip_rirb_get_response.
			 */
			fchip_writereg_b(fchip_azx, RIRBSTS, RIRB_INT_MASK);
			active = true;
			if (status & RIRB_INT_RESPONSE) {
				if (fchip_azx->driver_caps & AZX_DCAPS_CTX_WORKAROUND){
					udelay(80);
                }
				snd_hdac_bus_update_rirb(bus);
			}
		}
	} while (active && ++repeat < 10);

 unlock:
	spin_unlock(&bus->reg_lock);

	return IRQ_RETVAL(handled);
}

int fchip_acquire_irq(struct fchip_azx *fchip_azx, int do_disconnect)
{
	struct hdac_bus *bus = azx_to_hda_bus(fchip_azx);

	if (request_irq(fchip_azx->pci->irq, fchip_interrupt,
			fchip_azx->msi ? 0 : IRQF_SHARED,
			fchip_azx->card->irq_descr, fchip_azx)) 
	{
		printk(KERN_ERR "fchip: Unable to grab IRQ %d, disabling device\n", fchip_azx->pci->irq);

		if (do_disconnect){
			snd_card_disconnect(fchip_azx->card);
		}
		return -1;
	}
	bus->irq = fchip_azx->pci->irq;
	fchip_azx->card->sync_irq = bus->irq;
	pci_intx(fchip_azx->pci, !fchip_azx->msi);
	return 0;
}