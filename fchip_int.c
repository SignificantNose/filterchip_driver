#include "fchip_int.h"
#include "fchip_hda_bus.h"
#include "fchip_posfix.h"

static void stream_update(struct hdac_bus *bus, struct hdac_stream *s)
{
	struct fchip_azx* fchip_azx = hdac_bus_to_azx(bus);
	struct azx_dev *azx_dev = hdac_stream_to_azx_dev(s);

	// check whether this IRQ is really acceptable
	if (!fchip_azx->ops->position_check ||
	    fchip_azx->ops->position_check(fchip_azx, azx_dev)) 
    {
		spin_unlock(&bus->reg_lock);
		snd_pcm_period_elapsed(azx_dev_to_hdac_stream(azx_dev)->substream);
		spin_lock(&bus->reg_lock);
	}
}

static irqreturn_t fchip_interrupt(int irq, void *dev_id)
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
			// Clearing the interrupt status here ensures that no
			// interrupt gets masked after the RIRB wp is read in
			// snd_hdac_bus_update_rirb. This avoids a possible
			// race condition where codec response in RIRB may
			// remain unserviced by IRQ, eventually falling back
			// to polling mode in fchip_rirb_get_response.
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

void fchip_irq_pending_work(struct work_struct *work)
{
	struct fchip_hda_intel* hda = container_of(work, struct fchip_hda_intel, irq_pending_work);
	struct fchip_azx* fchip_azx = &hda->chip;
	struct hdac_bus *bus = azx_to_hda_bus(fchip_azx);
	struct hdac_stream *s;
	int pending, ok;

	if (!hda->irq_pending_warned) {
		printk(KERN_INFO "fchip: IRQ timing workaround is activated for card #%d. Suggest a bigger bdl_pos_adj.\n",
			 fchip_azx->card->number);
		hda->irq_pending_warned = 1;
	}

	for (;;) {
		pending = 0;
		spin_lock_irq(&bus->reg_lock);

		list_for_each_entry(s, &bus->stream_list, list) {
			struct azx_dev *azx_dev = hdac_stream_to_azx_dev(s);
			if (!azx_dev->irq_pending || !s->substream || !s->running)
			{
				continue;
			}

			ok = fchip_position_ok(fchip_azx, azx_dev);
			if (ok > 0) {
				azx_dev->irq_pending = 0;
				spin_unlock(&bus->reg_lock);
				snd_pcm_period_elapsed(s->substream);
				spin_lock(&bus->reg_lock);
			} 
			else if (ok < 0) {
				pending = 0;	/* too early */
			} 
			else{
				pending++;
			}
		}
		
		spin_unlock_irq(&bus->reg_lock);
		if (!pending){
			return;
		}

		msleep(1);
	}
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

/* clear irq_pending flags and assure no on-going workq */
void fchip_clear_irq_pending(struct fchip_azx* fchip_azx)
{
	struct hdac_bus *bus = azx_to_hda_bus(fchip_azx);
	struct hdac_stream *s;

	spin_lock_irq(&bus->reg_lock);
	list_for_each_entry(s, &bus->stream_list, list) {
		struct azx_dev *azx_dev = hdac_stream_to_azx_dev(s);
		azx_dev->irq_pending = 0;
	}
	spin_unlock_irq(&bus->reg_lock);
}

int fchip_disable_msi_reset_irq(struct fchip_azx* fchip_azx)
{
    struct hdac_bus *bus = azx_to_hda_bus(fchip_azx);
	int err;

	free_irq(bus->irq, fchip_azx);
	bus->irq = -1;
	fchip_azx->card->sync_irq = -1;
	pci_disable_msi(fchip_azx->pci);
	fchip_azx->msi = 0;
	err = fchip_acquire_irq(fchip_azx, 1);
	if (err < 0){
		return err;
    }

	return 0;
}
