/*
 * MUSB OTG driver - support for Mentor's DMA controller
 *
 * Copyright 2005 Mentor Graphics Corporation
 * Copyright (C) 2005-2007 by Texas Instruments
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *   Porting to suit mediatek
 *   Copyright (C) 2011-2012
 *   Wei.Wen, Mediatek Inc <wei.wen@mediatek.com>
 */
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include "musb_core.h"
#include "musbhsdma.h"
#include "musb_qmu.h"

void mt85xx_mask_ack_irq(unsigned int irq);

static int dma_controller_start(struct dma_controller *c)
{
	/* nothing to do */
	return 0;
}

static void dma_channel_release(struct dma_channel *channel);

static int dma_controller_stop(struct dma_controller *c)
{
	struct musb_dma_controller *controller = container_of(c,
			struct musb_dma_controller, controller);
	struct musb *musb = controller->private_data;
	struct dma_channel *channel;
	u8 bit;

	if (controller->used_channels != 0) {
		dev_err(musb->controller,
			"Stopping DMA controller while channel active\n");

		for (bit = 0; bit < controller->channel_count; bit++) {
			if (controller->used_channels & (1 << bit)) {
				channel = &controller->channel[bit].channel;
				dma_channel_release(channel);

				if (!controller->used_channels)
					break;
			}
		}
	}

	return 0;
}

static struct dma_channel *dma_channel_allocate(struct dma_controller *c,
				struct musb_hw_ep *hw_ep, u8 transmit)
{
	struct musb_dma_controller *controller = container_of(c,
			struct musb_dma_controller, controller);
	struct musb_dma_channel *musb_channel = NULL;
	struct dma_channel *channel = NULL;
	
#ifndef LINUX_EMU_USB_CDC_SUPPORT
	u8 bit;

	for (bit = 0; bit < controller->channel_count; bit++) {
		if (!(controller->used_channels & (1 << bit))) {
			controller->used_channels |= (1 << bit);
			musb_channel = &(controller->channel[bit]);
			musb_channel->controller = controller;
			musb_channel->idx = bit;
			musb_channel->epnum = hw_ep->epnum;
			musb_channel->transmit = transmit;
			channel = &(musb_channel->channel);
			channel->private_data = musb_channel;
			channel->status = MUSB_DMA_STATUS_FREE;
	        channel->max_len = 0xFFFFFFFF;		
			/* Tx => mode 1; Rx => mode 0 */
			channel->desired_mode = transmit;
			channel->actual_len = 0;
			break;
		}
	}
#else	
    if(hw_ep->epnum == 1)
    	{
	    	if(transmit)//tx
    		{
				controller->used_channels |= (1 << 1);
				musb_channel = &(controller->channel[1]);
				musb_channel->controller = controller;
				musb_channel->idx = 1;
				musb_channel->epnum = hw_ep->epnum;
				musb_channel->transmit = transmit;
				channel = &(musb_channel->channel);
				channel->private_data = musb_channel;
				channel->status = MUSB_DMA_STATUS_FREE;
				channel->max_len = 0xFFFFFFFF;		
				channel->desired_mode = transmit;
				channel->actual_len = 0;
    		}
			else
			{
			controller->used_channels |= (1 << 0);
			musb_channel = &(controller->channel[0]);
			musb_channel->controller = controller;
			musb_channel->idx = 0;
			musb_channel->epnum = hw_ep->epnum;
			musb_channel->transmit = transmit;
			channel = &(musb_channel->channel);
			channel->private_data = musb_channel;
			channel->status = MUSB_DMA_STATUS_FREE;
	        channel->max_len = 0xFFFFFFFF;		
			channel->desired_mode = transmit;
			channel->actual_len = 0;
			}
    	}
	else
		{
		return NULL;
		}
#endif 
	return channel;
}

static void dma_channel_release(struct dma_channel *channel)
{
	struct musb_dma_channel *musb_channel = channel->private_data;

	channel->actual_len = 0;
	musb_channel->start_addr = 0;
	musb_channel->len = 0;

	musb_channel->controller->used_channels &=
		~(1 << musb_channel->idx);

	channel->status = MUSB_DMA_STATUS_UNKNOWN;
}

static void configure_channel(struct dma_channel *channel,
				u16 packet_sz, u8 mode,
				dma_addr_t dma_addr, u32 len)
{
	struct musb_dma_channel *musb_channel = channel->private_data;
	struct musb_dma_controller *controller = musb_channel->controller;
	void __iomem *mbase = controller->base;
	u8 bchannel = musb_channel->idx;
	u16 csr = 0;

	DBG(4, "%p, pkt_sz %d, addr 0x%lx, len %d, mode %d\n",
			channel, packet_sz, (unsigned long)dma_addr, len, mode);

	if (mode) {
		csr |= 1 << MUSB_HSDMA_MODE1_SHIFT;
		BUG_ON(len < packet_sz);

		if (packet_sz >= 64) {
			csr |= MUSB_HSDMA_BURSTMODE_INCR16
					<< MUSB_HSDMA_BURSTMODE_SHIFT;
		} else if (packet_sz >= 32) {
			csr |= MUSB_HSDMA_BURSTMODE_INCR8
					<< MUSB_HSDMA_BURSTMODE_SHIFT;
		} else if (packet_sz >= 16) {
			csr |= MUSB_HSDMA_BURSTMODE_INCR4
					<< MUSB_HSDMA_BURSTMODE_SHIFT;
		}
	}

	csr |= (musb_channel->epnum << MUSB_HSDMA_ENDPOINT_SHIFT)
		| (1 << MUSB_HSDMA_ENABLE_SHIFT)
		| (1 << MUSB_HSDMA_IRQENABLE_SHIFT)
		| (musb_channel->transmit
				? (1 << MUSB_HSDMA_TRANSMIT_SHIFT)
				: 0);
  /* address/count */
    MGC_Write32(mbase,
                MUSB_HSDMA_CHANNEL_OFFSET(bchannel, MUSB_HSDMA_ADDRESS),
                (uint32_t) dma_addr);
    MGC_Write32(mbase,
                MUSB_HSDMA_CHANNEL_OFFSET(bchannel, MUSB_HSDMA_COUNT),
                len);
  MGC_Write32(mbase,
                MUSB_HSDMA_CHANNEL_OFFSET(bchannel, MUSB_HSDMA_CONTROL),
                csr);
}
static int dma_channel_program(struct dma_channel *channel,
				u16 packet_sz, u8 mode,
				dma_addr_t dma_addr, u32 len)
{
	struct musb_dma_channel *musb_channel = channel->private_data;
	DBG(2, "ep%d-%s pkt_sz %d, dma_addr 0x%lx length %d, mode %d\n",
		musb_channel->epnum,
		musb_channel->transmit ? "Tx" : "Rx",
		packet_sz, (unsigned long)dma_addr, len, mode);

	BUG_ON(channel->status == MUSB_DMA_STATUS_UNKNOWN ||
		channel->status == MUSB_DMA_STATUS_BUSY);

	channel->actual_len = 0;
	musb_channel->start_addr = dma_addr;
	musb_channel->len = len;
	musb_channel->max_packet_sz = packet_sz;
	channel->status = MUSB_DMA_STATUS_BUSY;

	configure_channel(channel, packet_sz, mode, dma_addr, len);

	return true;
}

static int dma_channel_abort(struct dma_channel *channel)
{
	struct musb_dma_channel *musb_channel = channel->private_data;
	void __iomem *mbase = musb_channel->controller->base;

	u8 bchannel = musb_channel->idx;
	int offset;
	u16 csr;

	if (channel->status == MUSB_DMA_STATUS_BUSY) {
		if (musb_channel->transmit) {
			offset = MUSB_EP_OFFSET(musb_channel->epnum,
						MUSB_TXCSR);
			
			/*
			 * The programming guide says that we must clear
			 * the DMAENAB bit before the DMAMODE bit...
			 */
			csr = musb_readw(mbase, offset);
			csr &= ~(MUSB_TXCSR_AUTOSET | MUSB_TXCSR_DMAENAB);
			musb_writew(mbase, offset, csr);
			csr &= ~MUSB_TXCSR_DMAMODE;
			musb_writew(mbase, offset, csr);
		} else {
			offset = MUSB_EP_OFFSET(musb_channel->epnum,
						MUSB_RXCSR);

			csr = musb_readw(mbase, offset);
			csr &= ~(MUSB_RXCSR_AUTOCLEAR |
				 MUSB_RXCSR_DMAENAB |
				 MUSB_RXCSR_DMAMODE);
			musb_writew(mbase, offset, csr);
		}

 /* address/count */
    MGC_Write32(mbase,
                MUSB_HSDMA_CHANNEL_OFFSET(bchannel, MUSB_HSDMA_ADDRESS),
                0);
   MGC_Write32(mbase,
                MUSB_HSDMA_CHANNEL_OFFSET(bchannel, MUSB_HSDMA_COUNT),
                0);
 	MGC_Write32(mbase,
                MUSB_HSDMA_CHANNEL_OFFSET(bchannel, MUSB_HSDMA_CONTROL),
                0);
		channel->status = MUSB_DMA_STATUS_FREE;
	}

	return 0;
}

enum dma_channel_status
musb_dma_channel_status(struct dma_channel *c)
{
	struct musb_dma_channel *musb_channel;
	void __iomem *mbase;

	u8 bchannel; 
	u16 csr;
	u16 csr_mask;
	u32 addr;
	if(is_dma_capable() && c)
	{
		musb_channel = c->private_data;
		mbase = musb_channel->controller->base;

		bchannel = musb_channel->idx; 
		csr = musb_readw(mbase,
					MUSB_HSDMA_CHANNEL_OFFSET(bchannel,
							MUSB_HSDMA_CONTROL));
		
		if(!musb_channel->transmit)
		{
	 		/*handle last short packet in multiple packet DMA RX mode 1*/  
			csr_mask = (1 << MUSB_HSDMA_ENABLE_SHIFT) | (1 << MUSB_HSDMA_MODE1_SHIFT) | 
        				(1 << MUSB_HSDMA_IRQENABLE_SHIFT) ;
			if( (csr & csr_mask)==csr_mask)
			{
				/* most DMA controllers would update the count register for simplicity... */
				addr = musb_read_hsdma_addr(mbase,
						bchannel);
				c->actual_len = addr
					- musb_channel->start_addr;
				DBG(2,"DMA actual_length=%zd\n",c->actual_len);
				musb_writew(mbase,
				MUSB_HSDMA_CHANNEL_OFFSET(bchannel, MUSB_HSDMA_CONTROL),
				0);
				musb_write_hsdma_addr(mbase, bchannel, 0);
				musb_write_hsdma_count(mbase, bchannel, 0);
				c->status=MUSB_DMA_STATUS_MODE1_SHORTPKT;
			}
		}
		return c->status;
	}
	else
	{
		return MUSB_DMA_STATUS_UNKNOWN;
	}
}



#ifndef CONFIG_MUSB_PIO_ONLY
irqreturn_t dma_controller_irq(int irq, void *private_data)
{
	struct musb_dma_controller *controller = private_data;
	struct musb *musb = controller->private_data;
	struct musb_dma_channel *musb_channel;
	struct dma_channel *channel;

	void __iomem *mbase = controller->base;

	irqreturn_t retval = IRQ_NONE;

	unsigned long flags;

	u8 bchannel;
	u8 int_hsdma;

	u32 addr;
	u16 csr;

	spin_lock_irqsave(&musb->lock, flags);

	int_hsdma = musb_readb(mbase, MUSB_HSDMA_INTR);
	
	if (!int_hsdma)
		goto done;
       musb_writeb(mbase,MUSB_HSDMA_INTR,int_hsdma);

   
	DBG(6,"dma intr %x\n",int_hsdma);


	for (bchannel = 0; bchannel < controller->channel_count; bchannel++) {
		if (int_hsdma & (1 << bchannel)) {
			musb_channel = (struct musb_dma_channel *)
					&(controller->channel[bchannel]);
			channel = &musb_channel->channel;

			csr = musb_readw(mbase,
					MUSB_HSDMA_CHANNEL_OFFSET(bchannel,
							MUSB_HSDMA_CONTROL));

			if (csr & (1 << MUSB_HSDMA_BUSERROR_SHIFT)) {
				musb_channel->channel.status =
					MUSB_DMA_STATUS_BUS_ABORT;
			} else {
				u8 devctl;

				addr = musb_read_hsdma_addr(mbase,
						bchannel);
				channel->actual_len = addr
					- musb_channel->start_addr;

				DBG(2, "ch %p, 0x%x -> 0x%x (%zd / %d) %s\n",
					channel, musb_channel->start_addr,
					addr, channel->actual_len,
					musb_channel->len,
					(channel->actual_len
						< musb_channel->len) ?
					"=> reconfig 0" : "=> complete");

				devctl = musb_readb(mbase, MUSB_DEVCTL);

				channel->status = MUSB_DMA_STATUS_FREE;

				/* completed */
				if ((devctl & MUSB_DEVCTL_HM)
					&& (musb_channel->transmit)
					&& ((channel->desired_mode == 0)
					    || (channel->actual_len &
					    (musb_channel->max_packet_sz - 1)))
				    ) {
					u8  epnum  = musb_channel->epnum;
					int offset = MUSB_EP_OFFSET(epnum,
								    MUSB_TXCSR);
					u16 txcsr;

					/*
					 * The programming guide says that we
					 * must clear DMAENAB before DMAMODE.
					 */
					musb_ep_select(mbase, epnum);
					txcsr = musb_readw(mbase, offset);
					txcsr &= ~(MUSB_TXCSR_DMAENAB
							| MUSB_TXCSR_AUTOSET);
					musb_writew(mbase, offset, txcsr);
					/* Send out the packet */
					txcsr &= ~MUSB_TXCSR_DMAMODE;
					txcsr |=  MUSB_TXCSR_TXPKTRDY;
					musb_writew(mbase, offset, txcsr);
				}
				musb_dma_completion(musb, musb_channel->epnum,
						    musb_channel->transmit);
			}
		}
	}

#ifdef CONFIG_BLACKFIN
	/* Clear DMA interrup flags */
	musb_writeb(mbase, MUSB_HSDMA_INTR, int_hsdma);
#endif

	retval = IRQ_HANDLED;
done:
	spin_unlock_irqrestore(&musb->lock, flags);
	return retval;
}
#endif
void dma_controller_destroy(struct dma_controller *c)
{
	struct musb_dma_controller *controller = container_of(c,
			struct musb_dma_controller, controller);

	if (!controller)
		return;

#if 0
	if (controller->irq)
		free_irq(controller->irq, c);
#endif	

	kfree(controller);
}

struct dma_controller *__init
dma_controller_create(struct musb *musb, void __iomem *base)
{
	struct musb_dma_controller *controller;
	struct device *dev = musb->controller;
	struct platform_device *pdev = to_platform_device(dev);
	int irq = platform_get_irq(pdev, 1);

	if (irq == 0) {
		dev_err(dev, "No DMA interrupt line!\n");
		return NULL;
	}

	controller = kzalloc(sizeof(*controller), GFP_KERNEL);
	if (!controller)
		return NULL;

	controller->channel_count = musb->config->dma_channels;//MUSB_HSDMA_CHANNELS;
	controller->private_data = musb;
	controller->base = base;

	controller->controller.start = dma_controller_start;
	controller->controller.stop = dma_controller_stop;
	controller->controller.channel_alloc = dma_channel_allocate;
	controller->controller.channel_release = dma_channel_release;
	controller->controller.channel_program = dma_channel_program;
	controller->controller.channel_abort = dma_channel_abort;
	
	//reserved last channel
	#ifdef CONFIG_USB_QMU_SUPPORT
 		//if(musb->config->id == 0)
		{
		  struct musb_dma_channel *musb_channel = &(controller->channel[controller->channel_count-1]);

		  controller->used_channels |= (1<<(controller->channel_count-1));
		  musb_channel->channel.private_data = musb_channel;
		  musb_channel->controller = controller;
		  mtk_q_dma_select(musb,controller->channel_count-1,3);
		  DBG(0,"reserved dma q channel %d\n",controller->channel_count-1);
		}
	#endif
#if  0
	if (request_irq(irq, dma_controller_irq, IRQF_DISABLED | IRQF_SHARED,
			dev_name(musb->controller), &controller->controller)) {
		dev_err(dev, "request_irq %d failed!\n", irq);
		dma_controller_destroy(&controller->controller);

		return NULL;
	}
#endif

	controller->irq = irq;

	return &controller->controller;
}
