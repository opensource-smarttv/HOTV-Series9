/*
 * xHCI host controller driver
 *
 * Copyright (C) 2008 Intel Corp.
 *
 * Author: Sarah Sharp
 * Some code borrowed from the Linux EHCI driver.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * Ring initialization rules:
 * 1. Each segment is initialized to zero, except for link TRBs.
 * 2. Ring cycle state = 0.  This represents Producer Cycle State (PCS) or
 *    Consumer Cycle State (CCS), depending on ring function.
 * 3. Enqueue pointer = dequeue pointer = address of first TRB in the segment.
 *
 * Ring behavior rules:
 * 1. A ring is empty if enqueue == dequeue.  This means there will always be at
 *    least one free TRB in the ring.  This is useful if you want to turn that
 *    into a link TRB and expand the ring.
 * 2. When incrementing an enqueue or dequeue pointer, if the next TRB is a
 *    link TRB, then load the pointer with the address in the link TRB.  If the
 *    link TRB had its toggle bit set, you may need to update the ring cycle
 *    state (see cycle bit rules).  You may have to do this multiple times
 *    until you reach a non-link TRB.
 * 3. A ring is full if enqueue++ (for the definition of increment above)
 *    equals the dequeue pointer.
 *
 * Cycle bit rules:
 * 1. When a consumer increments a dequeue pointer and encounters a toggle bit
 *    in a link TRB, it must toggle the ring cycle state.
 * 2. When a producer increments an enqueue pointer and encounters a toggle bit
 *    in a link TRB, it must toggle the ring cycle state.
 *
 * Producer rules:
 * 1. Check if ring is full before you enqueue.
 * 2. Write the ring cycle state to the cycle bit in the TRB you're enqueuing.
 *    Update enqueue pointer between each write (which may update the ring
 *    cycle state).
 * 3. Notify consumer.  If SW is producer, it rings the doorbell for command
 *    and endpoint rings.  If HC is the producer for the event ring,
 *    and it generates an interrupt according to interrupt modulation rules.
 *
 * Consumer rules:
 * 1. Check if TRB belongs to you.  If the cycle bit == your ring cycle state,
 *    the TRB is owned by the consumer.
 * 2. Update dequeue pointer (which may update the ring cycle state) and
 *    continue processing TRBs until you reach a TRB which is not owned by you.
 * 3. Notify the producer.  SW is the consumer for the event ring, and it
 *   updates event ring dequeue pointer.  HC is the consumer for the command and
 *   endpoint rings; it generates events on the event ring for these.
 */

#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include "xhci.h"
#include "mtk-test-lib.h"


/*
 * Returns zero if the TRB isn't in this segment, otherwise it returns the DMA
 * address of the TRB.
 */
dma_addr_t xhci_trb_virt_to_dma(struct xhci_segment *seg,
		union xhci_trb *trb)
{
	unsigned long segment_offset;

	if (!seg || !trb || trb < seg->trbs)
		return 0;
	/* offset in TRBs */
	segment_offset = trb - seg->trbs;
	if (segment_offset > TRBS_PER_SEGMENT)
		return 0;
	return seg->dma + (segment_offset * sizeof(*trb));
}

/* Does this link TRB point to the first segment in a ring,
 * or was the previous TRB the last TRB on the last segment in the ERST?
 */
static inline bool last_trb_on_last_seg(struct xhci_hcd *xhci, struct xhci_ring *ring,
		struct xhci_segment *seg, union xhci_trb *trb)
{
	if (ring == xhci->event_ring)
		return (trb == &seg->trbs[TRBS_PER_SEGMENT]) &&
			(seg->next == xhci->event_ring->first_seg);
	else
		return trb->link.control & LINK_TOGGLE;
}

/* Is this TRB a link TRB or was the last TRB the last TRB in this event ring
 * segment?  I.e. would the updated event TRB pointer step off the end of the
 * event seg?
 */
static inline int last_trb(struct xhci_hcd *xhci, struct xhci_ring *ring,
		struct xhci_segment *seg, union xhci_trb *trb)
{
	if (ring == xhci->event_ring)
		return trb == &seg->trbs[TRBS_PER_SEGMENT];
	else
		return (trb->link.control & TRB_TYPE_BITMASK) == TRB_TYPE(TRB_LINK);
}

static inline int enqueue_is_link_trb(struct xhci_ring *ring)
{
	struct xhci_link_trb *link = &ring->enqueue->link;
	return ((link->control & TRB_TYPE_BITMASK) == TRB_TYPE(TRB_LINK));
}

/* Updates trb to point to the next TRB in the ring, and updates seg if the next
 * TRB is in a new segment.  This does not skip over link TRBs, and it does not
 * effect the ring dequeue or enqueue pointers.
 */
static void next_trb(struct xhci_hcd *xhci,
		struct xhci_ring *ring,
		struct xhci_segment **seg,
		union xhci_trb **trb)
{
	if (last_trb(xhci, ring, *seg, *trb)) {
		*seg = (*seg)->next;
		*trb = ((*seg)->trbs);
	} else {
		(*trb)++;
	}
}

/*
 * See Cycle bit rules. SW is the consumer for the event ring only.
 * Don't make a ring full of link TRBs.  That would be dumb and this would loop.
 */
void inc_deq(struct xhci_hcd *xhci, struct xhci_ring *ring, bool consumer)
{
	union xhci_trb *next = ++(ring->dequeue);
	unsigned long long addr;

	ring->deq_updates++;
	/* Update the dequeue pointer further if that was a link TRB or we're at
	 * the end of an event ring segment (which doesn't have link TRBS)
	 */
	while (last_trb(xhci, ring, ring->deq_seg, next)) {
		if (consumer && last_trb_on_last_seg(xhci, ring, ring->deq_seg, next)) {
			ring->cycle_state = (ring->cycle_state ? 0 : 1);
			if (!in_interrupt())
				xhci_dbg(xhci, "Toggle cycle state for ring %p = %i\n",
						ring,
						(unsigned int) ring->cycle_state);
		}
		ring->deq_seg = ring->deq_seg->next;
		ring->dequeue = ring->deq_seg->trbs;
		next = ring->dequeue;
	}
	addr = (unsigned long long) xhci_trb_virt_to_dma(ring->deq_seg, ring->dequeue);
	if (ring == xhci->event_ring)
		xhci_dbg(xhci, "Event ring deq = 0x%llx (DMA)\n", addr);
	else if (ring == xhci->cmd_ring)
		xhci_dbg(xhci, "Command ring deq = 0x%llx (DMA)\n", addr);
	else
		xhci_dbg(xhci, "Ring deq = 0x%llx (DMA)\n", addr);
}

/*
 * See Cycle bit rules. SW is the consumer for the event ring only.
 * Don't make a ring full of link TRBs.  That would be dumb and this would loop.
 *
 * If we've just enqueued a TRB that is in the middle of a TD (meaning the
 * chain bit is set), then set the chain bit in all the following link TRBs.
 * If we've enqueued the last TRB in a TD, make sure the following link TRBs
 * have their chain bit cleared (so that each Link TRB is a separate TD).
 *
 * Section 6.4.4.1 of the 0.95 spec says link TRBs cannot have the chain bit
 * set, but other sections talk about dealing with the chain bit set.  This was
 * fixed in the 0.96 specification errata, but we have to assume that all 0.95
 * xHCI hardware can't handle the chain bit being cleared on a link TRB.
 *
 * @more_trbs_coming:	Will you enqueue more TRBs before calling
 *			prepare_transfer()?
 */
static void inc_enq(struct xhci_hcd *xhci, struct xhci_ring *ring,
		bool consumer, bool more_trbs_coming)
{
	u32 chain;
	union xhci_trb *next;
	unsigned long long addr;

	chain = ring->enqueue->generic.field[3] & TRB_CHAIN;
	next = ++(ring->enqueue);

	ring->enq_updates++;
	/* Update the dequeue pointer further if that was a link TRB or we're at
	 * the end of an event ring segment (which doesn't have link TRBS)
	 */
	while (last_trb(xhci, ring, ring->enq_seg, next)) {
		if (!consumer) {
			if (ring != xhci->event_ring) {
				/*
				 * If the caller doesn't plan on enqueueing more
				 * TDs before ringing the doorbell, then we
				 * don't want to give the link TRB to the
				 * hardware just yet.  We'll give the link TRB
				 * back in prepare_ring() just before we enqueue
				 * the TD at the top of the ring.
				 */
				if (!chain && !more_trbs_coming)
					break;

				/* If we're not dealing with 0.95 hardware,
				 * carry over the chain bit of the previous TRB
				 * (which may mean the chain bit is cleared).
				 */
				if (!xhci_link_trb_quirk(xhci)) {
					next->link.control &= ~TRB_CHAIN;
					next->link.control |= chain;
				}
				/* Give this link TRB to the hardware */
				wmb();
				next->link.control ^= TRB_CYCLE;
			}
			/* Toggle the cycle bit after the last ring segment. */
			if (last_trb_on_last_seg(xhci, ring, ring->enq_seg, next)) {
				ring->cycle_state = (ring->cycle_state ? 0 : 1);
				if (!in_interrupt())
					xhci_dbg(xhci, "Toggle cycle state for ring %p = %i\n",
							ring,
							(unsigned int) ring->cycle_state);
			}
		}
		ring->enq_seg = ring->enq_seg->next;
		ring->enqueue = ring->enq_seg->trbs;
		next = ring->enqueue;
	}
	addr = (unsigned long long) xhci_trb_virt_to_dma(ring->enq_seg, ring->enqueue);
	if (ring == xhci->event_ring)
		xhci_dbg(xhci, "Event ring enq = 0x%llx (DMA)\n", addr);
	else if (ring == xhci->cmd_ring)
		xhci_dbg(xhci, "Command ring enq = 0x%llx (DMA)\n", addr);
	else
		xhci_dbg(xhci, "Ring enq = 0x%llx (DMA)\n", addr);
}

/*
 * Check to see if there's room to enqueue num_trbs on the ring.  See rules
 * above.
 * FIXME: this would be simpler and faster if we just kept track of the number
 * of free TRBs in a ring.
 */
static int room_on_ring(struct xhci_hcd *xhci, struct xhci_ring *ring,
		unsigned int num_trbs)
{
	int i;
	union xhci_trb *enq = ring->enqueue;
	struct xhci_segment *enq_seg = ring->enq_seg;
	struct xhci_segment *cur_seg;
	unsigned int left_on_ring;

	/* If we are currently pointing to a link TRB, advance the
	 * enqueue pointer before checking for space */
	while (last_trb(xhci, ring, enq_seg, enq)) {
		enq_seg = enq_seg->next;
		enq = enq_seg->trbs;
	}

	/* Check if ring is empty */
	if (enq == ring->dequeue) {
		/* Can't use link trbs */
		left_on_ring = TRBS_PER_SEGMENT - 1;
		for (cur_seg = enq_seg->next; cur_seg != enq_seg;
				cur_seg = cur_seg->next)
			left_on_ring += TRBS_PER_SEGMENT - 1;

		/* Always need one TRB free in the ring. */
		left_on_ring -= 1;
		if (num_trbs > left_on_ring) {
			xhci_warn(xhci, "Not enough room on ring; "
					"need %u TRBs, %u TRBs left\n",
					num_trbs, left_on_ring);
			return 0;
		}
		return 1;
	}
	/* Make sure there's an extra empty TRB available */
	for (i = 0; i <= num_trbs; ++i) {
		if (enq == ring->dequeue)
			return 0;
		enq++;
		while (last_trb(xhci, ring, enq_seg, enq)) {
			enq_seg = enq_seg->next;
			enq = enq_seg->trbs;
		}
	}
	return 1;
}

void xhci_set_hc_event_deq(struct xhci_hcd *xhci)
{
	u64 temp;
	dma_addr_t deq;

	deq = xhci_trb_virt_to_dma(xhci->event_ring->deq_seg,
			xhci->event_ring->dequeue);
	if (deq == 0 && !in_interrupt())
		xhci_warn(xhci, "WARN something wrong with SW event ring "
				"dequeue ptr.\n");
	/* Update HC event ring dequeue pointer */
	temp = xhci_read_64(xhci, &xhci->ir_set->erst_dequeue);
	temp &= ERST_PTR_MASK;
	/* Don't clear the EHB bit (which is RW1C) because
	 * there might be more events to service.
	 */
	temp &= ~ERST_EHB;
	xhci_dbg(xhci, "// Write event ring dequeue pointer, preserving EHB bit\n");
	xhci_write_64(xhci, ((u64) deq & (u64) ~ERST_PTR_MASK) | temp,
			&xhci->ir_set->erst_dequeue);
}

/* Ring the host controller doorbell after placing a command on the ring */
void xhci_ring_cmd_db(struct xhci_hcd *xhci)
{
	u32 temp;

	xhci_dbg(xhci, "// Ding dong!\n");
	temp = xhci_readl(xhci, &xhci->dba->doorbell[0]) & DB_MASK;
	xhci_writel(xhci, temp | DB_TARGET_HOST, &xhci->dba->doorbell[0]);
#if 0
	/* Flush PCI posted writes */
	xhci_readl(xhci, &xhci->dba->doorbell[0]);
#endif
}

static void ring_ep_doorbell(struct xhci_hcd *xhci,
		unsigned int slot_id,
		unsigned int ep_index,
		unsigned int stream_id)
{
	struct xhci_virt_ep *ep;
	unsigned int ep_state;
	u32 field;
	__u32 __iomem *db_addr = &xhci->dba->doorbell[slot_id];

	ep = &xhci->devs[slot_id]->eps[ep_index];
	ep_state = ep->ep_state;
	/* Don't ring the doorbell for this endpoint if there are pending
	 * cancellations because the we don't want to interrupt processing.
	 * We don't want to restart any stream rings if there's a set dequeue
	 * pointer command pending because the device can choose to start any
	 * stream once the endpoint is on the HW schedule.
	 * FIXME - check all the stream rings for pending cancellations.
	 */
	if (!(ep_state & EP_HALT_PENDING) && !(ep_state & SET_DEQ_PENDING)
			&& !(ep_state & EP_HALTED)) {
		field = xhci_readl(xhci, db_addr) & DB_MASK;
		field |= EPI_TO_DB(ep_index) | STREAM_ID_TO_DB(stream_id);
		xhci_writel(xhci, field, db_addr);
	}
}

#if 0//not used
/* Ring the doorbell for any rings with pending URBs */
static void ring_doorbell_for_active_rings(struct xhci_hcd *xhci,
		unsigned int slot_id,
		unsigned int ep_index)
{
	unsigned int stream_id;
	struct xhci_virt_ep *ep;

	ep = &xhci->devs[slot_id]->eps[ep_index];

	/* A ring has pending URBs if its TD list is not empty */
	if (!(ep->ep_state & EP_HAS_STREAMS)) {
		if (!(list_empty(&ep->ring->td_list)))
			ring_ep_doorbell(xhci, slot_id, ep_index, 0);
		return;
	}

	for (stream_id = 1; stream_id < ep->stream_info->num_streams;
			stream_id++) {
		struct xhci_stream_info *stream_info = ep->stream_info;
		if (!list_empty(&stream_info->stream_rings[stream_id]->td_list))
			ring_ep_doorbell(xhci, slot_id, ep_index, stream_id);
	}
}

#endif
/*
 * Find the segment that trb is in.  Start searching in start_seg.
 * If we must move past a segment that has a link TRB with a toggle cycle state
 * bit set, then we will toggle the value pointed at by cycle_state.
 */
static struct xhci_segment *find_trb_seg(
		struct xhci_segment *start_seg,
		union xhci_trb	*trb, int *cycle_state)
{
	struct xhci_segment *cur_seg = start_seg;
	struct xhci_generic_trb *generic_trb;

	while (cur_seg->trbs > trb ||
			&cur_seg->trbs[TRBS_PER_SEGMENT - 1] < trb) {
		generic_trb = &cur_seg->trbs[TRBS_PER_SEGMENT - 1].generic;
		if ((generic_trb->field[3] & TRB_TYPE_BITMASK) ==
				TRB_TYPE(TRB_LINK) &&
				(generic_trb->field[3] & LINK_TOGGLE))
			*cycle_state = ~(*cycle_state) & 0x1;
		cur_seg = cur_seg->next;
		if (cur_seg == start_seg)
			/* Looped over the entire list.  Oops! */
			return NULL;
	}
	return cur_seg;
}

/*
 * Move the xHC's endpoint ring dequeue pointer past cur_td.
 * Record the new state of the xHC's endpoint ring dequeue segment,
 * dequeue pointer, and new consumer cycle state in state.
 * Update our internal representation of the ring's dequeue pointer.
 *
 * We do this in three jumps:
 *  - First we update our new ring state to be the same as when the xHC stopped.
 *  - Then we traverse the ring to find the segment that contains
 *    the last TRB in the TD.  We toggle the xHC's new cycle state when we pass
 *    any link TRBs with the toggle cycle bit set.
 *  - Finally we move the dequeue state one TRB further, toggling the cycle bit
 *    if we've moved it past a link TRB with the toggle cycle bit set.
 */
void xhci_find_new_dequeue_state(struct xhci_hcd *xhci,
		unsigned int slot_id, unsigned int ep_index,
		unsigned int stream_id, struct xhci_td *cur_td,
		struct xhci_dequeue_state *state)
{
	struct xhci_virt_device *dev = xhci->devs[slot_id];
	struct xhci_ring *ep_ring;
	struct xhci_generic_trb *trb;
	struct xhci_ep_ctx *ep_ctx;
	dma_addr_t addr;

	ep_ring = xhci_triad_to_transfer_ring(xhci, slot_id,
			ep_index, stream_id);
	if (!ep_ring) {
		xhci_warn(xhci, "WARN can't find new dequeue state "
				"for invalid stream ID %u.\n",
				stream_id);
		return;
	}
	state->new_cycle_state = 0;
	xhci_dbg(xhci, "Finding segment containing stopped TRB.\n");
	state->new_deq_seg = find_trb_seg(cur_td->start_seg,
			dev->eps[ep_index].stopped_trb,
			&state->new_cycle_state);
	if (!state->new_deq_seg)
		BUG();
	/* Dig out the cycle state saved by the xHC during the stop ep cmd */
	xhci_dbg(xhci, "Finding endpoint context\n");
	ep_ctx = xhci_get_ep_ctx(xhci, dev->out_ctx, ep_index);
	state->new_cycle_state = 0x1 & ep_ctx->deq;

	state->new_deq_ptr = cur_td->last_trb;
	xhci_dbg(xhci, "Finding segment containing last TRB in TD.\n");
	state->new_deq_seg = find_trb_seg(state->new_deq_seg,
			state->new_deq_ptr,
			&state->new_cycle_state);
	if (!state->new_deq_seg)
		BUG();

	trb = &state->new_deq_ptr->generic;
	if ((trb->field[3] & TRB_TYPE_BITMASK) == TRB_TYPE(TRB_LINK) &&
				(trb->field[3] & LINK_TOGGLE))
		state->new_cycle_state = ~(state->new_cycle_state) & 0x1;
	next_trb(xhci, ep_ring, &state->new_deq_seg, &state->new_deq_ptr);

	/* Don't update the ring cycle state for the producer (us). */
	xhci_dbg(xhci, "New dequeue segment = %p (virtual)\n",
			state->new_deq_seg);
	addr = xhci_trb_virt_to_dma(state->new_deq_seg, state->new_deq_ptr);
	xhci_dbg(xhci, "New dequeue pointer = 0x%llx (DMA)\n",
			(unsigned long long) addr);
	xhci_dbg(xhci, "Setting dequeue pointer in internal ring state.\n");
	ep_ring->dequeue = state->new_deq_ptr;
	ep_ring->deq_seg = state->new_deq_seg;
}

static void td_to_noop(struct xhci_hcd *xhci, struct xhci_ring *ep_ring,
		struct xhci_td *cur_td)
{
	struct xhci_segment *cur_seg;
	union xhci_trb *cur_trb;

	for (cur_seg = cur_td->start_seg, cur_trb = cur_td->first_trb;
			true;
			next_trb(xhci, ep_ring, &cur_seg, &cur_trb)) {
		if ((cur_trb->generic.field[3] & TRB_TYPE_BITMASK) ==
				TRB_TYPE(TRB_LINK)) {
			/* Unchain any chained Link TRBs, but
			 * leave the pointers intact.
			 */
			cur_trb->generic.field[3] &= ~TRB_CHAIN;
			xhci_dbg(xhci, "Cancel (unchain) link TRB\n");
			xhci_dbg(xhci, "Address = %p (0x%llx dma); "
					"in seg %p (0x%llx dma)\n",
					cur_trb,
					(unsigned long long)xhci_trb_virt_to_dma(cur_seg, cur_trb),
					cur_seg,
					(unsigned long long)cur_seg->dma);
		} else {
			cur_trb->generic.field[0] = 0;
			cur_trb->generic.field[1] = 0;
			cur_trb->generic.field[2] = 0;
			/* Preserve only the cycle bit of this TRB */
			cur_trb->generic.field[3] &= TRB_CYCLE;
			cur_trb->generic.field[3] |= TRB_TYPE(TRB_TR_NOOP);
			xhci_dbg(xhci, "Cancel TRB %p (0x%llx dma) "
					"in seg %p (0x%llx dma)\n",
					cur_trb,
					(unsigned long long)xhci_trb_virt_to_dma(cur_seg, cur_trb),
					cur_seg,
					(unsigned long long)cur_seg->dma);
		}
		if (cur_trb == cur_td->last_trb)
			break;
	}
}

static int queue_set_tr_deq(struct xhci_hcd *xhci, int slot_id,
		unsigned int ep_index, unsigned int stream_id,
		struct xhci_segment *deq_seg,
		union xhci_trb *deq_ptr, u32 cycle_state);

void xhci_queue_new_dequeue_state(struct xhci_hcd *xhci,
		unsigned int slot_id, unsigned int ep_index,
		unsigned int stream_id,
		struct xhci_dequeue_state *deq_state)
{
	struct xhci_virt_ep *ep = &xhci->devs[slot_id]->eps[ep_index];

	xhci_dbg(xhci, "Set TR Deq Ptr cmd, new deq seg = %p (0x%llx dma), "
			"new deq ptr = %p (0x%llx dma), new cycle = %u\n",
			deq_state->new_deq_seg,
			(unsigned long long)deq_state->new_deq_seg->dma,
			deq_state->new_deq_ptr,
			(unsigned long long)xhci_trb_virt_to_dma(deq_state->new_deq_seg, deq_state->new_deq_ptr),
			deq_state->new_cycle_state);
	queue_set_tr_deq(xhci, slot_id, ep_index, stream_id,
			deq_state->new_deq_seg,
			deq_state->new_deq_ptr,
			(u32) deq_state->new_cycle_state);
	/* Stop the TD queueing code from ringing the doorbell until
	 * this command completes.  The HC won't set the dequeue pointer
	 * if the ring is running, and ringing the doorbell starts the
	 * ring running.
	 */
	ep->ep_state |= SET_DEQ_PENDING;
}

static inline void xhci_stop_watchdog_timer_in_irq(struct xhci_hcd *xhci,
		struct xhci_virt_ep *ep)
{
	ep->ep_state &= ~EP_HALT_PENDING;
	/* Can't del_timer_sync in interrupt, so we attempt to cancel.  If the
	 * timer is running on another CPU, we don't decrement stop_cmds_pending
	 * (since we didn't successfully stop the watchdog timer).
	 */
	if (del_timer(&ep->stop_cmd_timer))
		ep->stop_cmds_pending--;
}

/* Must be called with xhci->lock held in interrupt context */
static void xhci_giveback_urb_in_irq(struct xhci_hcd *xhci,
		struct xhci_td *cur_td, int status, char *adjective)
{
	struct usb_hcd *hcd = xhci_to_hcd(xhci);

	cur_td->urb->hcpriv = NULL;
	usb_hcd_unlink_urb_from_ep(hcd, cur_td->urb);
	xhci_dbg(xhci, "Giveback %s URB %p\n", adjective, cur_td->urb);

	spin_unlock(&xhci->lock);
	usb_hcd_giveback_urb(hcd, cur_td->urb, status);
	kfree(cur_td);
	spin_lock(&xhci->lock);
	xhci_dbg(xhci, "%s URB given back\n", adjective);
}

#if 0//not used
/*
 * When we get a command completion for a Stop Endpoint Command, we need to
 * unlink any cancelled TDs from the ring.  There are two ways to do that:
 *
 *  1. If the HW was in the middle of processing the TD that needs to be
 *     cancelled, then we must move the ring's dequeue pointer past the last TRB
 *     in the TD with a Set Dequeue Pointer Command.
 *  2. Otherwise, we turn all the TRBs in the TD into No-op TRBs (with the chain
 *     bit cleared) so that the HW will skip over them.
 */
static void handle_stopped_endpoint(struct xhci_hcd *xhci,
		union xhci_trb *trb)
{
	unsigned int slot_id;
	unsigned int ep_index;
	struct xhci_ring *ep_ring;
	struct xhci_virt_ep *ep;
	struct list_head *entry;
	struct xhci_td *cur_td = NULL;
	struct xhci_td *last_unlinked_td;

	struct xhci_dequeue_state deq_state;

	memset(&deq_state, 0, sizeof(deq_state));
	slot_id = TRB_TO_SLOT_ID(trb->generic.field[3]);
	ep_index = TRB_TO_EP_INDEX(trb->generic.field[3]);
	ep = &xhci->devs[slot_id]->eps[ep_index];

	if (list_empty(&ep->cancelled_td_list)) {
		xhci_stop_watchdog_timer_in_irq(xhci, ep);
		ring_doorbell_for_active_rings(xhci, slot_id, ep_index);
		return;
	}

	/* Fix up the ep ring first, so HW stops executing cancelled TDs.
	 * We have the xHCI lock, so nothing can modify this list until we drop
	 * it.  We're also in the event handler, so we can't get re-interrupted
	 * if another Stop Endpoint command completes
	 */
	list_for_each(entry, &ep->cancelled_td_list) {
		cur_td = list_entry(entry, struct xhci_td, cancelled_td_list);
		xhci_dbg(xhci, "Cancelling TD starting at %p, 0x%llx (dma).\n",
				cur_td->first_trb,
				(unsigned long long)xhci_trb_virt_to_dma(cur_td->start_seg, cur_td->first_trb));
		ep_ring = xhci_urb_to_transfer_ring(xhci, cur_td->urb);
		if (!ep_ring) {
			/* This shouldn't happen unless a driver is mucking
			 * with the stream ID after submission.  This will
			 * leave the TD on the hardware ring, and the hardware
			 * will try to execute it, and may access a buffer
			 * that has already been freed.  In the best case, the
			 * hardware will execute it, and the event handler will
			 * ignore the completion event for that TD, since it was
			 * removed from the td_list for that endpoint.  In
			 * short, don't muck with the stream ID after
			 * submission.
			 */
			xhci_warn(xhci, "WARN Cancelled URB %p "
					"has invalid stream ID %u.\n",
					cur_td->urb,
					cur_td->urb->stream_id);
			goto remove_finished_td;
		}
		/*
		 * If we stopped on the TD we need to cancel, then we have to
		 * move the xHC endpoint ring dequeue pointer past this TD.
		 */
		if (cur_td == ep->stopped_td)
			xhci_find_new_dequeue_state(xhci, slot_id, ep_index,
					cur_td->urb->stream_id,
					cur_td, &deq_state);
		else
			td_to_noop(xhci, ep_ring, cur_td);
remove_finished_td:
		/*
		 * The event handler won't see a completion for this TD anymore,
		 * so remove it from the endpoint ring's TD list.  Keep it in
		 * the cancelled TD list for URB completion later.
		 */
		list_del(&cur_td->td_list);
	}
	last_unlinked_td = cur_td;
	xhci_stop_watchdog_timer_in_irq(xhci, ep);

	/* If necessary, queue a Set Transfer Ring Dequeue Pointer command */
	if (deq_state.new_deq_ptr && deq_state.new_deq_seg) {
		xhci_queue_new_dequeue_state(xhci,
				slot_id, ep_index,
				ep->stopped_td->urb->stream_id,
				&deq_state);
		xhci_ring_cmd_db(xhci);
	} else {
		/* Otherwise ring the doorbell(s) to restart queued transfers */
		ring_doorbell_for_active_rings(xhci, slot_id, ep_index);
	}
	ep->stopped_td = NULL;
	ep->stopped_trb = NULL;

	/*
	 * Drop the lock and complete the URBs in the cancelled TD list.
	 * New TDs to be cancelled might be added to the end of the list before
	 * we can complete all the URBs for the TDs we already unlinked.
	 * So stop when we've completed the URB for the last TD we unlinked.
	 */
	do {
		cur_td = list_entry(ep->cancelled_td_list.next,
				struct xhci_td, cancelled_td_list);
		list_del(&cur_td->cancelled_td_list);

		/* Clean up the cancelled URB */
		/* Doesn't matter what we pass for status, since the core will
		 * just overwrite it (because the URB has been unlinked).
		 */
		xhci_giveback_urb_in_irq(xhci, cur_td, 0, "cancelled");

		/* Stop processing the cancelled list if the watchdog timer is
		 * running.
		 */
		if (xhci->xhc_state & XHCI_STATE_DYING)
			return;
	} while (cur_td != last_unlinked_td);

	/* Return to the event handler with xhci->lock re-acquired */
}
#endif

/* Watchdog timer function for when a stop endpoint command fails to complete.
 * In this case, we assume the host controller is broken or dying or dead.  The
 * host may still be completing some other events, so we have to be careful to
 * let the event ring handler and the URB dequeueing/enqueueing functions know
 * through xhci->state.
 *
 * The timer may also fire if the host takes a very long time to respond to the
 * command, and the stop endpoint command completion handler cannot delete the
 * timer before the timer function is called.  Another endpoint cancellation may
 * sneak in before the timer function can grab the lock, and that may queue
 * another stop endpoint command and add the timer back.  So we cannot use a
 * simple flag to say whether there is a pending stop endpoint command for a
 * particular endpoint.
 *
 * Instead we use a combination of that flag and a counter for the number of
 * pending stop endpoint commands.  If the timer is the tail end of the last
 * stop endpoint command, and the endpoint's command is still pending, we assume
 * the host is dying.
 */
void xhci_stop_endpoint_command_watchdog(unsigned long arg)
{
	struct xhci_hcd *xhci;
	struct xhci_virt_ep *ep;
	struct xhci_virt_ep *temp_ep;
	struct xhci_ring *ring;
	struct xhci_td *cur_td;
	int ret, i, j;

	ep = (struct xhci_virt_ep *) arg;
	xhci = ep->xhci;

	spin_lock(&xhci->lock);

	ep->stop_cmds_pending--;
	if (xhci->xhc_state & XHCI_STATE_DYING) {
		xhci_dbg(xhci, "Stop EP timer ran, but another timer marked "
				"xHCI as DYING, exiting.\n");
		spin_unlock(&xhci->lock);
		return;
	}
	if (!(ep->stop_cmds_pending == 0 && (ep->ep_state & EP_HALT_PENDING))) {
		xhci_dbg(xhci, "Stop EP timer ran, but no command pending, "
				"exiting.\n");
		spin_unlock(&xhci->lock);
		return;
	}

	xhci_warn(xhci, "xHCI host not responding to stop endpoint command.\n");
	xhci_warn(xhci, "Assuming host is dying, halting host.\n");
	/* Oops, HC is dead or dying or at least not responding to the stop
	 * endpoint command.
	 */
	xhci->xhc_state |= XHCI_STATE_DYING;
	/* Disable interrupts from the host controller and start halting it */
	xhci_quiesce(xhci);
	spin_unlock(&xhci->lock);

	ret = xhci_halt(xhci);

	spin_lock(&xhci->lock);
	if (ret < 0) {
		/* This is bad; the host is not responding to commands and it's
		 * not allowing itself to be halted.  At least interrupts are
		 * disabled, so we can set HC_STATE_HALT and notify the
		 * USB core.  But if we call usb_hc_died(), it will attempt to
		 * disconnect all device drivers under this host.  Those
		 * disconnect() methods will wait for all URBs to be unlinked,
		 * so we must complete them.
		 */
		xhci_warn(xhci, "Non-responsive xHCI host is not halting.\n");
		xhci_warn(xhci, "Completing active URBs anyway.\n");
		/* We could turn all TDs on the rings to no-ops.  This won't
		 * help if the host has cached part of the ring, and is slow if
		 * we want to preserve the cycle bit.  Skip it and hope the host
		 * doesn't touch the memory.
		 */
	}
	for (i = 0; i < MAX_HC_SLOTS; i++) {
		if (!xhci->devs[i])
			continue;
		for (j = 0; j < 31; j++) {
			temp_ep = &xhci->devs[i]->eps[j];
			ring = temp_ep->ring;
			if (!ring)
				continue;
			xhci_dbg(xhci, "Killing URBs for slot ID %u, "
					"ep index %u\n", i, j);
			while (!list_empty(&ring->td_list)) {
				cur_td = list_first_entry(&ring->td_list,
						struct xhci_td,
						td_list);
				list_del(&cur_td->td_list);
				if (!list_empty(&cur_td->cancelled_td_list))
					list_del(&cur_td->cancelled_td_list);
				xhci_giveback_urb_in_irq(xhci, cur_td,
						-ESHUTDOWN, "killed");
			}
			while (!list_empty(&temp_ep->cancelled_td_list)) {
				cur_td = list_first_entry(
						&temp_ep->cancelled_td_list,
						struct xhci_td,
						cancelled_td_list);
				list_del(&cur_td->cancelled_td_list);
				xhci_giveback_urb_in_irq(xhci, cur_td,
						-ESHUTDOWN, "killed");
			}
		}
	}
	spin_unlock(&xhci->lock);
	xhci_to_hcd(xhci)->state = HC_STATE_HALT;
	xhci_dbg(xhci, "Calling usb_hc_died()\n");
	usb_hc_died(xhci_to_hcd(xhci));
	xhci_dbg(xhci, "xHCI host controller is dead.\n");
}

#if 0//not used
/*
 * When we get a completion for a Set Transfer Ring Dequeue Pointer command,
 * we need to clear the set deq pending flag in the endpoint ring state, so that
 * the TD queueing code can ring the doorbell again.  We also need to ring the
 * endpoint doorbell to restart the ring, but only if there aren't more
 * cancellations pending.
 */
static void handle_set_deq_completion(struct xhci_hcd *xhci,
		struct xhci_event_cmd *event,
		union xhci_trb *trb)
{
	unsigned int slot_id;
	unsigned int ep_index;
	unsigned int stream_id;
	struct xhci_ring *ep_ring;
	struct xhci_virt_device *dev;
	struct xhci_ep_ctx *ep_ctx;
	struct xhci_slot_ctx *slot_ctx;

	slot_id = TRB_TO_SLOT_ID(trb->generic.field[3]);
	ep_index = TRB_TO_EP_INDEX(trb->generic.field[3]);
	stream_id = TRB_TO_STREAM_ID(trb->generic.field[2]);
	dev = xhci->devs[slot_id];

	ep_ring = xhci_stream_id_to_ring(dev, ep_index, stream_id);
	if (!ep_ring) {
		xhci_warn(xhci, "WARN Set TR deq ptr command for "
				"freed stream ID %u\n",
				stream_id);
		/* XXX: Harmless??? */
		dev->eps[ep_index].ep_state &= ~SET_DEQ_PENDING;
		return;
	}

	ep_ctx = xhci_get_ep_ctx(xhci, dev->out_ctx, ep_index);
	slot_ctx = xhci_get_slot_ctx(xhci, dev->out_ctx);

	if (GET_COMP_CODE(event->status) != COMP_SUCCESS) {
		unsigned int ep_state;
		unsigned int slot_state;

		switch (GET_COMP_CODE(event->status)) {
		case COMP_TRB_ERR:
			xhci_warn(xhci, "WARN Set TR Deq Ptr cmd invalid because "
					"of stream ID configuration\n");
			break;
		case COMP_CTX_STATE:
			xhci_warn(xhci, "WARN Set TR Deq Ptr cmd failed due "
					"to incorrect slot or ep state.\n");
			ep_state = ep_ctx->ep_info;
			ep_state &= EP_STATE_MASK;
			slot_state = slot_ctx->dev_state;
			slot_state = GET_SLOT_STATE(slot_state);
			xhci_dbg(xhci, "Slot state = %u, EP state = %u\n",
					slot_state, ep_state);
			break;
		case COMP_EBADSLT:
			xhci_warn(xhci, "WARN Set TR Deq Ptr cmd failed because "
					"slot %u was not enabled.\n", slot_id);
			break;
		default:
			xhci_warn(xhci, "WARN Set TR Deq Ptr cmd with unknown "
					"completion code of %u.\n",
					GET_COMP_CODE(event->status));
			break;
		}
		/* OK what do we do now?  The endpoint state is hosed, and we
		 * should never get to this point if the synchronization between
		 * queueing, and endpoint state are correct.  This might happen
		 * if the device gets disconnected after we've finished
		 * cancelling URBs, which might not be an error...
		 */
	} else {
		xhci_dbg(xhci, "Successful Set TR Deq Ptr cmd, deq = @%08llx\n",
				ep_ctx->deq);
		if (xhci_trb_virt_to_dma(dev->eps[ep_index].queued_deq_seg,
					dev->eps[ep_index].queued_deq_ptr) ==
				(ep_ctx->deq & ~(EP_CTX_CYCLE_MASK))) {
			/* Update the ring's dequeue segment and dequeue pointer
			 * to reflect the new position.
			 */
			ep_ring->deq_seg = dev->eps[ep_index].queued_deq_seg;
			ep_ring->dequeue = dev->eps[ep_index].queued_deq_ptr;
		} else {
			xhci_warn(xhci, "Mismatch between completed Set TR Deq "
					"Ptr command & xHCI internal state.\n");
			xhci_warn(xhci, "ep deq seg = %p, deq ptr = %p\n",
					dev->eps[ep_index].queued_deq_seg,
					dev->eps[ep_index].queued_deq_ptr);
		}
	}

	dev->eps[ep_index].ep_state &= ~SET_DEQ_PENDING;
	dev->eps[ep_index].queued_deq_seg = NULL;
	dev->eps[ep_index].queued_deq_ptr = NULL;
	/* Restart any rings with pending URBs */
	ring_doorbell_for_active_rings(xhci, slot_id, ep_index);
}

static void handle_reset_ep_completion(struct xhci_hcd *xhci,
		struct xhci_event_cmd *event,
		union xhci_trb *trb)
{
	int slot_id;
	unsigned int ep_index;

	slot_id = TRB_TO_SLOT_ID(trb->generic.field[3]);
	ep_index = TRB_TO_EP_INDEX(trb->generic.field[3]);
	/* This command will only fail if the endpoint wasn't halted,
	 * but we don't care.
	 */
	xhci_dbg(xhci, "Ignoring reset ep completion code of %u\n",
			(unsigned int) GET_COMP_CODE(event->status));

	/* HW with the reset endpoint quirk needs to have a configure endpoint
	 * command complete before the endpoint can be used.  Queue that here
	 * because the HW can't handle two commands being queued in a row.
	 */
	if (xhci->quirks & XHCI_RESET_EP_QUIRK) {
		xhci_dbg(xhci, "Queueing configure endpoint command\n");
		xhci_queue_configure_endpoint(xhci,
				xhci->devs[slot_id]->in_ctx->dma, slot_id,
				false);
		xhci_ring_cmd_db(xhci);
	} else {
		/* Clear our internal halted state and restart the ring(s) */
		xhci->devs[slot_id]->eps[ep_index].ep_state &= ~EP_HALTED;
		ring_doorbell_for_active_rings(xhci, slot_id, ep_index);
	}
}

/* Check to see if a command in the device's command queue matches this one.
 * Signal the completion or free the command, and return 1.  Return 0 if the
 * completed command isn't at the head of the command list.
 */
static int handle_cmd_in_cmd_wait_list(struct xhci_hcd *xhci,
		struct xhci_virt_device *virt_dev,
		struct xhci_event_cmd *event)
{
	struct xhci_command *command;

	if (list_empty(&virt_dev->cmd_list))
		return 0;

	command = list_entry(virt_dev->cmd_list.next,
			struct xhci_command, cmd_list);
	if (xhci->cmd_ring->dequeue != command->command_trb)
		return 0;

	command->status =
		GET_COMP_CODE(event->status);
	list_del(&command->cmd_list);
	if (command->completion)
		complete(command->completion);
	else
		xhci_free_command(xhci, command);
	return 1;
}

#endif

/*
 * This TD is defined by the TRBs starting at start_trb in start_seg and ending
 * at end_trb, which may be in another segment.  If the suspect DMA address is a
 * TRB in this TD, this function returns that TRB's segment.  Otherwise it
 * returns 0.
 */
struct xhci_segment *trb_in_td(struct xhci_segment *start_seg,
		union xhci_trb	*start_trb,
		union xhci_trb	*end_trb,
		dma_addr_t	suspect_dma)
{
	dma_addr_t start_dma;
	dma_addr_t end_seg_dma;
	dma_addr_t end_trb_dma;
	struct xhci_segment *cur_seg;

	start_dma = xhci_trb_virt_to_dma(start_seg, start_trb);
	cur_seg = start_seg;
//	printk(KERN_DEBUG "start_dma 0x%x\n", start_dma);
//	printk(KERN_DEBUG "start_seg 0x%x\n", (unsigned int)start_seg);
//	printk(KERN_DEBUG "start_trb 0x%x\n", (unsigned int)start_trb);
//	printk(KERN_DEBUG "end_trb 0x%x\n", (unsigned int)end_trb);
//	printk(KERN_DEBUG "suspect_dma 0x%x\n", suspect_dma);	

	do {
		if (start_dma == 0){
			printk(KERN_DEBUG "return NULL 1\n");
			return NULL;
		}
		/* We may get an event for a Link TRB in the middle of a TD */
		end_seg_dma = xhci_trb_virt_to_dma(cur_seg,
				&cur_seg->trbs[TRBS_PER_SEGMENT - 1]);
		/* If the end TRB isn't in this segment, this is set to 0 */
		end_trb_dma = xhci_trb_virt_to_dma(cur_seg, end_trb);
//		printk(KERN_DEBUG "end_trb_dma 0x%x\n", end_trb_dma);	
		if (end_trb_dma > 0) {
			/* The end TRB is in this segment, so suspect should be here */
			if (start_dma <= end_trb_dma) {
				if (suspect_dma >= start_dma && suspect_dma <= end_trb_dma)
					return cur_seg;
			} else {
				/* Case for one segment with
				 * a TD wrapped around to the top
				 */
				if ((suspect_dma >= start_dma &&
							suspect_dma <= end_seg_dma) ||
						(suspect_dma >= cur_seg->dma &&
						 suspect_dma <= end_trb_dma))
					return cur_seg;
			}
			printk(KERN_DEBUG "return NULL 2\n");
			return NULL;
		} else {
			/* Might still be somewhere in this segment */
			if (suspect_dma >= start_dma && suspect_dma <= end_seg_dma)
				return cur_seg;
		}
		cur_seg = cur_seg->next;
		start_dma = xhci_trb_virt_to_dma(cur_seg, &cur_seg->trbs[0]);
	} while (cur_seg != start_seg);
	printk(KERN_DEBUG "return NULL 3\n");
	return NULL;
}

static void xhci_cleanup_halted_endpoint(struct xhci_hcd *xhci,
		unsigned int slot_id, unsigned int ep_index,
		unsigned int stream_id,
		struct xhci_td *td, union xhci_trb *event_trb)
{
	struct xhci_virt_ep *ep = &xhci->devs[slot_id]->eps[ep_index];
	ep->ep_state |= EP_HALTED;
	ep->stopped_td = td;
	ep->stopped_trb = event_trb;
	ep->stopped_stream = stream_id;

	xhci_queue_reset_ep(xhci, slot_id, ep_index);
	xhci_cleanup_stalled_ring(xhci, td->urb->dev, ep_index);

	ep->stopped_td = NULL;
	ep->stopped_trb = NULL;
	ep->stopped_stream = 0;

	xhci_ring_cmd_db(xhci);
}

/* Check if an error has halted the endpoint ring.  The class driver will
 * cleanup the halt for a non-default control endpoint if we indicate a stall.
 * However, a babble and other errors also halt the endpoint ring, and the class
 * driver won't clear the halt in that case, so we need to issue a Set Transfer
 * Ring Dequeue Pointer command manually.
 */
static int xhci_requires_manual_halt_cleanup(struct xhci_hcd *xhci,
		struct xhci_ep_ctx *ep_ctx,
		unsigned int trb_comp_code)
{
	xhci_dbg(xhci, "check required to cleanup halt ep\n");
	xhci_dbg(xhci, "ep_info 0x%x\n", ep_ctx->ep_info);
	/* TRB completion codes that may require a manual halt cleanup */
	if (trb_comp_code == COMP_TX_ERR ||
			trb_comp_code == COMP_BABBLE ||
			trb_comp_code == COMP_SPLIT_ERR)
		/* The 0.96 spec says a babbling control endpoint
		 * is not halted. The 0.96 spec says it is.  Some HW
		 * claims to be 0.95 compliant, but it halts the control
		 * endpoint anyway.  Check if a babble halted the
		 * endpoint.
		 */
		if ((ep_ctx->ep_info & EP_STATE_MASK) == EP_STATE_HALTED)
			return 1;

	return 0;
}

int xhci_is_vendor_info_code(struct xhci_hcd *xhci, unsigned int trb_comp_code)
{
	if (trb_comp_code >= 224 && trb_comp_code <= 255) {
		/* Vendor defined "informational" completion code,
		 * treat as not-an-error.
		 */
		xhci_dbg(xhci, "Vendor defined info completion code %u\n",
				trb_comp_code);
		xhci_dbg(xhci, "Treating code as success.\n");
		return 1;
	}
	return 0;
}

/*
 * Finish the td processing, remove the td from td list;
 * Return 1 if the urb can be given back.
 */
static int finish_td(struct xhci_hcd *xhci, struct xhci_td *td,
	union xhci_trb *event_trb, struct xhci_transfer_event *event,
	struct xhci_virt_ep *ep, int *status, bool skip)
{
	struct xhci_virt_device *xdev;
	struct xhci_ring *ep_ring;
	unsigned int slot_id;
	int ep_index;
	struct urb *urb = NULL;
	struct xhci_ep_ctx *ep_ctx;
	int ret = 0;
	struct urb_priv	*urb_priv;
	u32 trb_comp_code;

	slot_id = TRB_TO_SLOT_ID(event->flags);
	xdev = xhci->devs[slot_id];
	ep_index = TRB_TO_EP_ID(event->flags) - 1;
	ep_ring = xhci_dma_to_transfer_ring(ep, event->buffer);
	ep_ctx = xhci_get_ep_ctx(xhci, xdev->out_ctx, ep_index);
	trb_comp_code = GET_COMP_CODE(event->transfer_len);

	if (skip)
		goto td_cleanup;

	if (trb_comp_code == COMP_STOP_INVAL ||
			trb_comp_code == COMP_STOP) {
		/* The Endpoint Stop Command completion will take care of any
		 * stopped TDs.  A stopped TD may be restarted, so don't update
		 * the ring dequeue pointer or take this TD off any lists yet.
		 */
		ep->stopped_td = td;
		ep->stopped_trb = event_trb;
		return 0;
	} else {
		if (trb_comp_code == COMP_STALL) {
			/* The transfer is completed from the driver's
			 * perspective, but we need to issue a set dequeue
			 * command for this stalled endpoint to move the dequeue
			 * pointer past the TD.  We can't do that here because
			 * the halt condition must be cleared first.  Let the
			 * USB class driver clear the stall later.
			 */
			ep->stopped_td = td;
			ep->stopped_trb = event_trb;
			ep->stopped_stream = ep_ring->stream_id;
		} else if (xhci_requires_manual_halt_cleanup(xhci,
					ep_ctx, trb_comp_code)) {
			/* Other types of errors halt the endpoint, but the
			 * class driver doesn't call usb_reset_endpoint() unless
			 * the error is -EPIPE.  Clear the halted status in the
			 * xHCI hardware manually.
			 */
			xhci_dbg(xhci, "Need to cleanup halt ep, do it\n");
			xhci_cleanup_halted_endpoint(xhci,
					slot_id, ep_index, ep_ring->stream_id,
					td, event_trb);
		} else {

			/* Update ring dequeue pointer */
			while (ep_ring->dequeue != td->last_trb)
				inc_deq(xhci, ep_ring, false);
			inc_deq(xhci, ep_ring, false);	
		}
td_cleanup:
	/* Clean up the endpoint's TD list */
	urb = td->urb;
	urb_priv = urb->hcpriv;

	/* Do one last check of the actual transfer length.
	 * If the host controller said we transferred more data than
	 * the buffer length, urb->actual_length will be a very big
	 * number (since it's unsigned).  Play it safe and say we didn't
	 * transfer anything.
	 */
	if (urb->actual_length > urb->transfer_buffer_length) {
		xhci_warn(xhci, "URB transfer length is wrong, "
				"xHC issue? req. len = %u, "
				"act. len = %u\n",
				urb->transfer_buffer_length,
				urb->actual_length);
		urb->actual_length = 0;
		if (td->urb->transfer_flags & URB_SHORT_NOT_OK)
			*status = -EREMOTEIO;
		else
			*status = 0;
	}
#if 0
	spin_lock(&ep_ring->lock);
	list_del(&td->td_list);
	spin_unlock(&ep_ring->lock);
#endif
	list_del(&td->td_list);
	/* Was this TD slated to be cancelled but completed anyway? */
	if (!list_empty(&td->cancelled_td_list))
		list_del(&td->cancelled_td_list);

	urb_priv->td_cnt++;
	/* Giveback the urb when all the tds are completed */
	if (urb_priv->td_cnt == urb_priv->length)
		ret = 1;
    }
	return ret;
}


/*
 * Process control tds, update urb status and actual_length.
 */
static int process_ctrl_td(struct xhci_hcd *xhci, struct xhci_td *td,
	union xhci_trb *event_trb, struct xhci_transfer_event *event,
	struct xhci_virt_ep *ep, int *status)
{
	struct xhci_virt_device *xdev;
	struct xhci_ring *ep_ring;
	unsigned int slot_id;
	int ep_index;
	struct xhci_ep_ctx *ep_ctx;
	u32 trb_comp_code;

	slot_id = TRB_TO_SLOT_ID(event->flags);
	xdev = xhci->devs[slot_id];
	ep_index = TRB_TO_EP_ID(event->flags) - 1;
	ep_ring = xhci_dma_to_transfer_ring(ep, event->buffer);
	ep_ctx = xhci_get_ep_ctx(xhci, xdev->out_ctx, ep_index);
	trb_comp_code = GET_COMP_CODE(event->transfer_len);

	xhci_debug_trb(xhci, xhci->event_ring->dequeue);
	switch (trb_comp_code) {
	case COMP_SUCCESS:
		if (event_trb == ep_ring->dequeue) {
			xhci_warn(xhci, "WARN: Success on ctrl setup TRB "
					"without IOC set??\n");
			*status = -ESHUTDOWN;
		} else if (event_trb != td->last_trb) {
#if 1
			xhci_warn(xhci, "WARN: Success on ctrl data TRB "
					"without IOC set??\n");
#endif
			*status = -ESHUTDOWN;
		} else {
			xhci_dbg(xhci, "Successful control transfer!\n");
			*status = 0;
		}
		break;
	case COMP_SHORT_TX:
		xhci_warn(xhci, "WARN: short transfer on control ep\n");
		*status = 0;
		break;
	case COMP_STOP_INVAL:
	case COMP_STOP:
		return finish_td(xhci, td, event_trb, event, ep, status, false);
	default:
		xhci_dbg(xhci, "TRB error code %u, "
				"halted endpoint index = %u\n",
				trb_comp_code, ep_index);
		if (!xhci_requires_manual_halt_cleanup(xhci,
					ep_ctx, trb_comp_code))
		break;		
	/* else fall through */
	case COMP_STALL:
		/* Did we transfer part of the data (middle) phase? */
		if (event_trb != ep_ring->dequeue &&
				event_trb != td->last_trb)
			td->urb->actual_length =
				td->urb->transfer_buffer_length
				- TRB_LEN(event->transfer_len);
		else
			td->urb->actual_length = 0;

		xhci_cleanup_halted_endpoint(xhci,
			slot_id, ep_index, 0, td, event_trb);
		return finish_td(xhci, td, event_trb, event, ep, status, true);
		return 0;
	}
	/*
	 * Did we transfer any data, despite the errors that might have
	 * happened?  I.e. did we get past the setup stage?
	 */
	if (event_trb != ep_ring->dequeue) {
		/* The event was for the status stage */
		if (event_trb == td->last_trb) {
			if (td->urb->actual_length != 0) {
				/* Don't overwrite a previously set error code
				 */
				if ((*status == -EINPROGRESS || *status == 0) &&
						(td->urb->transfer_flags
						 & URB_SHORT_NOT_OK))
					/* Did we already see a short data
					 * stage? */
					*status = -EREMOTEIO;
			} else {
				td->urb->actual_length =
					td->urb->transfer_buffer_length;
			}
		} else {
		/* Maybe the event was for the data stage? */
			if (trb_comp_code != COMP_STOP_INVAL) {
				/* We didn't stop on a link TRB in the middle */
				td->urb->actual_length =
					td->urb->transfer_buffer_length -
					TRB_LEN(event->transfer_len);
				xhci_dbg(xhci, "Waiting for status "
						"stage event\n");
				return 0;
			}
		}
	}
	
	return finish_td(xhci, td, event_trb, event, ep, status, false);
}

/*
 * Process bulk and interrupt tds, update urb status and actual_length.
 */
static int process_bulk_intr_td(struct xhci_hcd *xhci, struct xhci_td *td,
	union xhci_trb *event_trb, struct xhci_transfer_event *event,
	struct xhci_virt_ep *ep, int *status)
{
	struct xhci_ring *ep_ring;
	union xhci_trb *cur_trb;
	struct xhci_segment *cur_seg;
	u32 trb_comp_code;

	ep_ring = xhci_dma_to_transfer_ring(ep, event->buffer);
	trb_comp_code = GET_COMP_CODE(event->transfer_len);

	switch (trb_comp_code) {
	case COMP_SUCCESS:
		/* Double check that the HW transferred everything. */
		if (event_trb != td->last_trb) {
			xhci_warn(xhci, "WARN Successful completion "
					"on short TX\n");
			if (td->urb->transfer_flags & URB_SHORT_NOT_OK)
				*status = -EREMOTEIO;
			else
				*status = 0;
		} else {
			if (usb_endpoint_xfer_bulk(&td->urb->ep->desc))
				xhci_dbg(xhci, "Successful bulk "
						"transfer!\n");
			else
				xhci_dbg(xhci, "Successful interrupt "
						"transfer!\n");
			*status = 0;
		}
		break;
	case COMP_SHORT_TX:
		if (td->urb->transfer_flags & URB_SHORT_NOT_OK)
			*status = -EREMOTEIO;
		else
			*status = 0;
		break;
	default:
		/* Others already handled above */
		break;
	}
	xhci_dbg(xhci,
			"ep %#x - asked for %d bytes, "
			"%d bytes untransferred\n",
			td->urb->ep->desc.bEndpointAddress,
			td->urb->transfer_buffer_length,
			TRB_LEN(event->transfer_len));
	/* Fast path - was this the last TRB in the TD for this URB? */
	if (event_trb == td->last_trb) {
		if (TRB_LEN(event->transfer_len) != 0) {
			td->urb->actual_length =
				td->urb->transfer_buffer_length -
				TRB_LEN(event->transfer_len);
			if (td->urb->transfer_buffer_length <
					td->urb->actual_length) {
				xhci_warn(xhci, "HC gave bad length "
						"of %d bytes left\n",
						TRB_LEN(event->transfer_len));
				td->urb->actual_length = 0;
				if (td->urb->transfer_flags & URB_SHORT_NOT_OK)
					*status = -EREMOTEIO;
				else
					*status = 0;
			}
			/* Don't overwrite a previously set error code */
			if (*status == -EINPROGRESS) {
				if (td->urb->transfer_flags & URB_SHORT_NOT_OK)
					*status = -EREMOTEIO;
				else
					*status = 0;
			}
		} else {
			td->urb->actual_length =
				td->urb->transfer_buffer_length;
			/* Ignore a short packet completion if the
			 * untransferred length was zero.
			 */
			if (*status == -EREMOTEIO)
				*status = 0;
		}
	} else {
		/* Slow path - walk the list, starting from the dequeue
		 * pointer, to get the actual length transferred.
		 */
		td->urb->actual_length = 0;
		for (cur_trb = ep_ring->dequeue, cur_seg = ep_ring->deq_seg;
				cur_trb != event_trb;
				next_trb(xhci, ep_ring, &cur_seg, &cur_trb)) {
			if ((cur_trb->generic.field[3] &
			 TRB_TYPE_BITMASK) != TRB_TYPE(TRB_TR_NOOP) &&
			    (cur_trb->generic.field[3] &
			 TRB_TYPE_BITMASK) != TRB_TYPE(TRB_LINK))
				td->urb->actual_length +=
					TRB_LEN(cur_trb->generic.field[2]);
		}
		/* If the ring didn't stop on a Link or No-op TRB, add
		 * in the actual bytes transferred from the Normal TRB
		 */
		if (trb_comp_code != COMP_STOP_INVAL)
			td->urb->actual_length +=
				TRB_LEN(cur_trb->generic.field[2]) -
				TRB_LEN(event->transfer_len);
	}
	return finish_td(xhci, td, event_trb, event, ep, status, false);

}


/*
 * Process isochronous tds, update urb packet status and actual_length.
 */
static int process_isoc_td(struct xhci_hcd *xhci, struct xhci_td *td,
	union xhci_trb *event_trb, struct xhci_transfer_event *event,
	struct xhci_virt_ep *ep, int *status)
{
	struct xhci_ring *ep_ring;
	struct urb_priv *urb_priv;
	int idx;
	int len = 0;
	int skip_td = 0;
	union xhci_trb *cur_trb;
	struct xhci_segment *cur_seg;
	u32 trb_comp_code;

	ep_ring = xhci_dma_to_transfer_ring(ep, event->buffer);
	trb_comp_code = GET_COMP_CODE(event->transfer_len);
	urb_priv = td->urb->hcpriv;
	idx = urb_priv->td_cnt;

	/* handle completion code */
	switch (trb_comp_code) {
	case COMP_SUCCESS:
		td->urb->iso_frame_desc[idx].status = 0;
		xhci_dbg(xhci, "Successful isoc transfer!\n");
		break;
	case COMP_SHORT_TX:
		if (td->urb->transfer_flags & URB_SHORT_NOT_OK)
			td->urb->iso_frame_desc[idx].status =
				 -EREMOTEIO;
		else
			td->urb->iso_frame_desc[idx].status = 0;
		break;
	case COMP_BW_OVER:
		td->urb->iso_frame_desc[idx].status = -ECOMM;
		skip_td = 1;
		break;
	case COMP_BUFF_OVER:
	case COMP_BABBLE:
		td->urb->iso_frame_desc[idx].status = -EOVERFLOW;
		skip_td = 1;
		break;
	case COMP_STALL:
		td->urb->iso_frame_desc[idx].status = -EPROTO;
		skip_td = 1;
		break;
	case COMP_STOP:
	case COMP_STOP_INVAL:
		break;
	default:
		td->urb->iso_frame_desc[idx].status = -1;
		break;
	}

	if (trb_comp_code == COMP_SUCCESS || skip_td == 1) {
		td->urb->iso_frame_desc[idx].actual_length =
			td->urb->iso_frame_desc[idx].length;
		td->urb->actual_length +=
			td->urb->iso_frame_desc[idx].length;
	} else {
		for (cur_trb = ep_ring->dequeue,
		     cur_seg = ep_ring->deq_seg; cur_trb != event_trb;
		     next_trb(xhci, ep_ring, &cur_seg, &cur_trb)) {
			if ((cur_trb->generic.field[3] &
			 TRB_TYPE_BITMASK) != TRB_TYPE(TRB_TR_NOOP) &&
			    (cur_trb->generic.field[3] &
			 TRB_TYPE_BITMASK) != TRB_TYPE(TRB_LINK))
				len +=
				    TRB_LEN(cur_trb->generic.field[2]);
		}
		len += TRB_LEN(cur_trb->generic.field[2]) -
			TRB_LEN(event->transfer_len);

		if (trb_comp_code != COMP_STOP_INVAL) {
			td->urb->iso_frame_desc[idx].actual_length = len;
			td->urb->actual_length += len;
		}
	}

	if ((idx == urb_priv->length - 1) && *status == -EINPROGRESS){
		*status = 0;
		td->urb->status = 0;
	}
	return finish_td(xhci, td, event_trb, event, ep, status, false);

}

static char *trb_name[] = {"Rsv", "Normal", "Setup", "Data", "Statu", "Isoc", "Link",
							"Event Data", "No-op", "Enable Slot", "Disable Slot", "Addr Dev",
							"CFG EP", "Evaluate CTX", "Reset EP", "Stop EP", "Set TR deq",
							"Reset Dev", "Force Event", "Negotiate BW", "Set LT", "Get Port BW",
							"Force Header", "No Op"};
static void handle_cmd_completion(struct xhci_hcd *xhci,
		struct xhci_event_cmd *event)
{
	int slot_id;
	u64 cmd_dma;
	dma_addr_t cmd_dequeue_dma;
	union xhci_trb *trb;
	struct xhci_virt_device *virt_dev;
	unsigned int ep_index, cmd;

	slot_id = TRB_TO_SLOT_ID(event->flags);
	cmd_dma = event->cmd_trb;
	cmd_dequeue_dma = xhci_trb_virt_to_dma(xhci->cmd_ring->deq_seg,
			xhci->cmd_ring->dequeue);
	trb = xhci->cmd_ring->dequeue;
	/* Is the command ring deq ptr out of sync with the deq seg ptr? */
	if (cmd_dequeue_dma == 0) {
		xhci->error_bitmask |= 1 << 4;
		return;
	}
	/* Does the DMA address match our internal dequeue pointer address? */
	if (cmd_dma != (u64) cmd_dequeue_dma) {
		xhci->error_bitmask |= 1 << 5;
		return;
	}

	cmd = (xhci->cmd_ring->dequeue->generic.field[3] & TRB_TYPE_BITMASK)>>10;
	if (g_intr_handled != -1) {
		if (cmd < ARRAY_SIZE(trb_name)) 
			xhci_err(xhci, "cmd : %d %s\n", cmd, trb_name[cmd]);
		else
			xhci_err(xhci, "cmd : %d\n", cmd);
		xhci_err(xhci, "comp code: %d\n", GET_COMP_CODE(event->status));
	}
	switch (xhci->cmd_ring->dequeue->generic.field[3] & TRB_TYPE_BITMASK) {
	case TRB_TYPE(TRB_CMD_NOOP):
		break;
	case TRB_TYPE(TRB_ENABLE_SLOT):
		if (GET_COMP_CODE(event->status) == COMP_SUCCESS){
			xhci_dbg(xhci, "command enable slot success event\n");
			g_slot_id = slot_id;
			g_cmd_status = CMD_DONE;
		}
		else{
			g_slot_id = 0;
			g_cmd_status = CMD_FAIL;
		}
		break;
	case TRB_TYPE(TRB_DISABLE_SLOT):
		if (GET_COMP_CODE(event->status) == COMP_SUCCESS){
			xhci_dbg(xhci, "command disable slot success event, slot_id: %d\n", slot_id);
			g_slot_id = slot_id;
			g_cmd_status = CMD_DONE;
		}
		break;
	case TRB_TYPE(TRB_ADDR_DEV):
		xhci_dbg(xhci, "comp_code: %d\n", GET_COMP_CODE(event->status));
		if(GET_COMP_CODE(event->status) == COMP_SUCCESS){
			xhci_dbg(xhci, "address device success\n");
			g_cmd_status = CMD_DONE;
		}
		else if(GET_COMP_CODE(event->status) == COMP_CMD_ABORT){
			xhci_dbg(xhci, "address device command aborted\n");
			g_cmd_status = CMD_DONE;
		}
		else{
			g_cmd_status = CMD_FAIL;
		}
		break;
	case TRB_TYPE(TRB_CONFIG_EP):
		if(GET_COMP_CODE(event->status) == COMP_SUCCESS){
			xhci_dbg(xhci, "config endpoint success\n");
			g_cmd_status = CMD_DONE;
		}
		else{
			g_cmd_status = CMD_FAIL;
		}
		break;
	case TRB_TYPE(TRB_RESET_DEV):
		if(GET_COMP_CODE(event->status) == COMP_SUCCESS){
			xhci_dbg(xhci, "reset dev success\n");
			g_cmd_status = CMD_DONE;
		}
		else{
			xhci_dbg(xhci, "reset dev failed, code: %d\n", GET_COMP_CODE(event->status));
			g_cmd_status = CMD_FAIL;
		}
		break;
	case TRB_TYPE(TRB_STOP_RING):
		xhci_dbg(xhci, "TRB_STOP_RING\n");
		//xhci_err(xhci, "[DBG] stop ep event refer to 0x%x\n", event->cmd_trb);
		if((((int)event->cmd_trb) & 0xff0) != g_cmd_ring_pointer1 && (((int)event->cmd_trb) & 0xff0) != g_cmd_ring_pointer2){
			xhci_err(xhci, "[DBG] handle stop ep command pointer not equal to enqueued pointer, enqueue 0x%x , 0x%x, event refer 0x%x\n"
				, g_cmd_ring_pointer1, g_cmd_ring_pointer2, (((int)event->cmd_trb) & 0xff0));
		//	while(1);
		}
		if(GET_COMP_CODE(event->status) == COMP_SUCCESS){
					xhci_dbg(xhci, "stop ring success\n");
					g_cmd_status = CMD_DONE;
				}
				else{
					xhci_dbg(xhci, "stop ring failed, code: %d\n", GET_COMP_CODE(event->status));
					g_cmd_status = CMD_FAIL;
		}

		break;
	case TRB_TYPE(TRB_SET_DEQ):
		xhci_dbg(xhci, "TRB_SET_DEQ\n");
		if(GET_COMP_CODE(event->status) == COMP_SUCCESS){
				ep_index = TRB_TO_EP_INDEX(trb->generic.field[3]);
				virt_dev = xhci->devs[slot_id];
				virt_dev->eps[ep_index].ep_state &= ~SET_DEQ_PENDING;
				g_cmd_status = CMD_DONE;
			}
			else{
				xhci_dbg(xhci, "stop ring failed, code: %d\n", GET_COMP_CODE(event->status));
				g_cmd_status = CMD_FAIL;
		}
		break;
	case TRB_TYPE(TRB_EVAL_CONTEXT):
		xhci_dbg(xhci, "TRB_EVAL_CONTEXT\n");
		if(GET_COMP_CODE(event->status) == COMP_SUCCESS){
				g_cmd_status = CMD_DONE;
			}
			else{
				xhci_dbg(xhci, "eval context, code: %d\n", GET_COMP_CODE(event->status));
				g_cmd_status = CMD_FAIL;
		}
		break;
	case TRB_TYPE(TRB_RESET_EP):
		ep_index = TRB_TO_EP_INDEX(trb->generic.field[3]);
		xhci->devs[slot_id]->eps[ep_index].ep_state &= ~EP_HALTED;
		g_cmd_status = CMD_DONE;
		break;
	default:
		if(GET_COMP_CODE(event->status) == COMP_CMD_STOP){
			xhci_dbg(xhci, "command ring stopped\n");
			g_cmd_status = CMD_DONE;
			return;
		}
#if 0
		case TRB_TYPE(TRB_COMPLETION):
		if(GET_COMP_CODE(event->status) == COMP_CMD_STOP){
			xhci_dbg(xhci, "stop command ring\n");
			g_cmd_status = CMD_DONE;
		}
		else{
			xhci_dbg(xhci, "stop command failed, code: %d\n", GET_COMP_CODE(event->status));
			g_cmd_status = CMD_FAIL;
		}
		break;
#endif
		/* Skip over unknown commands on the event ring */
		xhci->error_bitmask |= 1 << 6;
		g_cmd_status = CMD_FAIL;
		break;
	}
	inc_deq(xhci, xhci->cmd_ring, false);
}

void rh_port_clear_change(struct xhci_hcd *xhci, int port_id, int port_temp){
	u32 temp;
	u32 __iomem *addr;
	port_id--;
	
	addr = &xhci->op_regs->port_status_base + NUM_PORT_REGS*(port_id & 0xff);
	//temp = temp = xhci_readl(xhci, addr);
	temp = port_temp;
	xhci_dbg(xhci, "to clear port change, actual port %d status  = 0x%x\n", port_id, temp);
	temp = xhci_port_state_to_clear_change(temp);
	xhci_writel(xhci, temp, addr);
	temp = xhci_readl(xhci, addr);
	xhci_dbg(xhci, "clear port change, actual port %d status  = 0x%x\n", port_id, temp);
}


int rh_get_port_status(struct xhci_hcd *xhci, int port_id){
	u32 temp,status;
	u32 __iomem *addr;
	
	port_id--;
	status = 0;
	
	addr = &xhci->op_regs->port_status_base + NUM_PORT_REGS*(port_id & 0xff);
	temp = xhci_readl(xhci, addr);
	xhci_dbg(xhci, "get port status, actual port %d status  = 0x%x\n", port_id, temp);

	/* wPortChange bits */
	if (temp & PORT_CSC)
		status |= USB_PORT_STAT_C_CONNECTION << 16;
	if (temp & PORT_PEC)
		status |= USB_PORT_STAT_C_ENABLE << 16;
	if ((temp & PORT_OCC))
		status |= USB_PORT_STAT_C_OVERCURRENT << 16;
	if ((temp & PORT_RC))
		status |= USB_PORT_STAT_C_RESET << 16;
	if ((temp & PORT_PLC))
		status |= USB_PORT_STAT_C_SUSPEND << 16;
	/*
	 * FIXME ignoring suspend, reset, and USB 2.1/3.0 specific
	 * changes
	 */
	if (temp & PORT_CONNECT) {
		status |= USB_PORT_STAT_CONNECTION;
		status |= xhci_port_speed(temp);
	}
	if (temp & PORT_PE)
		status |= USB_PORT_STAT_ENABLE;
	if (temp & PORT_OC)
		status |= USB_PORT_STAT_OVERCURRENT;
	if (temp & PORT_RESET)
		status |= USB_PORT_STAT_RESET;
	if (temp & PORT_POWER)
		status |= USB_PORT_STAT_POWER;
	xhci_dbg(xhci, "Get port status returned 0x%x\n", status);
	temp = xhci_port_state_to_neutral(temp);
//	xhci_writel(xhci, temp, addr);
//	temp = xhci_readl(xhci, addr);
	xhci_dbg(xhci, "Actual port %d status  = 0x%x\n", port_id, temp);
#if 0
	put_unaligned(cpu_to_le32(status), (__le32 *) buf);
#endif
	return status;
}

static void handle_port_status(struct xhci_hcd *xhci,
		union xhci_trb *event)
{
	u32 port_id, temp;
	int port_status;
	u32 __iomem *addr;
	int port_index;
	struct xhci_port *port, *combined_port;
	u32 u4CurrPortStatus =0;
#if TEST_OTG
	//u32 temp2;
#endif	
	/* Port status change events always have a successful completion code */
	if (GET_COMP_CODE(event->generic.field[2]) != COMP_SUCCESS) {
		xhci_warn(xhci, "WARN: xHC returned failed port status event\n");
		xhci->error_bitmask |= 1 << 8;
	}
	/* FIXME: core doesn't care about all port link state changes yet */
	port_id = GET_PORT_ID(event->generic.field[0]);
	port_index = get_port_index(port_id);
	if(port_index >= RH_PORT_NUM){
		xhci_err(xhci, "[ERROR] RH_PORT_NUM not enough\n");
		return;
	}
	port = rh_port[port_index];
	port->port_id = port_id;
	addr = &xhci->op_regs->port_status_base + NUM_PORT_REGS*((port_id-1) & 0xff);
	temp = xhci_readl(xhci, addr);
	u4CurrPortStatus = temp;
#if TEST_OTG
	printk(KERN_ERR "[OTG_H] port_status change event port_status 0x%x\n", temp);
#endif
	port_status = rh_get_port_status(xhci, port_id);
//	rh_port_clear_change(xhci, port_id);
	
	if(port_status & (USB_PORT_STAT_C_CONNECTION << 16)){
#if TEST_OTG		
		g_otg_csc = true;
#endif
		if(port_status & USB_PORT_STAT_CONNECTION){
			xhci_err(xhci, "connect port status event, connected\n");
			if (port_status & USB_PORT_STAT_SUPER_SPEED)
				xhci_err(xhci, "SS\n");
			g_port_id = port_id;
			g_port_connect = true;
			port->port_status = CONNECTED;
#if TEST_OTG
			g_otg_wait_con = false;
#endif
			if(!(port_status & USB_PORT_STAT_SUPER_SPEED)){
				if(g_hs_block_reset){
#if TEST_OTG
					port->port_status = ENABLED;
#endif
				}
				else{
#if TEST_OTG

					if(!g_otg_dev_B)
						mdelay(100);
#endif
					//reset status
					addr = &xhci->op_regs->port_status_base + NUM_PORT_REGS*((port_id-1) & 0xff);
					temp = xhci_readl(xhci, addr);
					u4CurrPortStatus = temp;
					temp = xhci_port_state_to_neutral(temp);
					temp = (temp | PORT_RESET);
					xhci_writel(xhci, temp, addr);
					port->port_status = RESET;
				}
			}
			else{
				if(port->port_reenabled == 1){
					port->port_reenabled = 2;
				}
				if(g_device_reconnect == 1)
					g_device_reconnect = 2;
				g_speed = USB_SPEED_SUPER;
				addr = &xhci->op_regs->port_status_base + NUM_PORT_REGS*((port_id-1) & 0xff);
				temp = xhci_readl(xhci, addr);
				u4CurrPortStatus = temp;
				if(((temp & PORT_RESET) == 0) && (temp & PORT_PE) && (PORT_PLS(temp) == 0)){
					port->port_status = ENABLED;
					port->port_speed = USB_SPEED_SUPER;
					xhci_dbg(xhci, "port set: port_id %d, port_status %d, port_speed %d\n"
						, port->port_id, port->port_status, port->port_speed);
					g_port_reset = true;
				}
				else{
					xhci_dbg(xhci, "Super speed port enabled failed!!\n");
					xhci_dbg(xhci, "temp & PORT_RESET 0x%x\n", (temp & PORT_RESET));
					xhci_dbg(xhci, "temp & PORT_PE 0x%x\n", (temp & PORT_PE));
					xhci_dbg(xhci, "temp & PORT_PLS 0x%x\n", (PORT_PLS(temp)));
					g_port_reset = false;
				}
			}
		}
		else{	//port disconnect
			xhci_err(xhci, "connect port status event, disconnected\n");
			switch (g_speed) {
				case USB_SPEED_LOW:
					xhci_err(xhci, "LS\n");
					break;
				case USB_SPEED_HIGH:
					xhci_err(xhci, "HS\n");
					break;
				case USB_SPEED_FULL:
					xhci_err(xhci, "FS\n");
					break;
				case USB_SPEED_SUPER:
					xhci_err(xhci, "SS\n");
					break;
				default:
					xhci_err(xhci, "undef speed\n");
			}
			port->port_speed = 0;
			port->port_status = DISCONNECTED;
			if(port->port_reenabled == 0){
				port->port_reenabled = 1;
			}
			g_port_connect = false;
			g_port_reset = false;
			if(g_device_reconnect == 0)
				g_device_reconnect = 1;
#if TEST_OTG
		//	temp2 = readl(SSUSB_OTG_STS);
			//if change role doesn't turn off power, else turn off power
			//TODO:
#endif
			//workaround for mt6290 
			//mt6290 will generate u2 connect, u2 disconnect, the u3 connect event
			combined_port = rh_port[RH_PORT_NUM - 1 - port_index];
			if (combined_port->port_status == ENABLED) {
				port->port_id = combined_port->port_id;
				port->port_speed = combined_port->port_speed;
				port->port_status = combined_port->port_status;
				port->port_reenabled = combined_port->port_reenabled;
				memset(combined_port, 0, sizeof(*combined_port));
				g_port_connect = true;
				g_port_reset = true;
			}
		}
	}
	if((port_status & (USB_PORT_STAT_C_RESET << 16)) && (port_status & (USB_PORT_STAT_CONNECTION)) && !(port_status & USB_PORT_STAT_SUPER_SPEED)){
		if(!(port_status & USB_PORT_STAT_RESET)){
			if(port_status & USB_PORT_STAT_LOW_SPEED){
				port->port_speed = USB_SPEED_LOW;
				xhci_err(xhci, "LS\n");
				g_speed = USB_SPEED_LOW;
			}
			else if(port_status & USB_PORT_STAT_HIGH_SPEED){
				port->port_speed = USB_SPEED_HIGH;
				g_speed = USB_SPEED_HIGH;
				xhci_err(xhci, "HS\n");
			}
			else{
				port->port_speed = USB_SPEED_FULL;
				g_speed = USB_SPEED_FULL;
				xhci_err(xhci, "FS\n");
			}
			port->port_status = ENABLED;
			if(port->port_reenabled == 1){
				port->port_reenabled = 2;
			}
			if(g_device_reconnect == 1)
				g_device_reconnect = 2;
			g_port_reset = true;
		}
		else{
			g_port_reset = false;
		}
	}
	else if((port_status & (USB_PORT_STAT_C_RESET << 16)) && (port_status & (USB_PORT_STAT_CONNECTION)) 
		&& (port_status & USB_PORT_STAT_SUPER_SPEED)){
		port->port_status = ENABLED;
	}
#if TEST_OTG
	else if((port_status & (USB_PORT_STAT_C_RESET << 16)) && (!(port_status & (USB_PORT_STAT_CONNECTION)))){
		//OTG with PET, change back to device after just reset
		g_port_connect = false;
	}
#endif
	if(port_status & (USB_PORT_STAT_C_SUSPEND << 16)){
		xhci_dbg(xhci, "port link status changed, wake up \n");
		//udelay(1000);
	}
	if(port_status & (USB_PORT_STAT_C_OVERCURRENT << 16)){
		xhci_err(xhci, "port over current changed\n");
		g_port_occ = true;
	}
	
	if ((u4CurrPortStatus & PORT_PLC) && ((u4CurrPortStatus & PORT_PLS_MASK) == XDEV_RESUME)){
		g_port_resume = 1;
		xhci_err(xhci, "device remote wakeup received\n");
	}
	if ((u4CurrPortStatus & PORT_PLC)){
		g_port_plc = 1;
	}
	rh_port_clear_change(xhci, port_id, u4CurrPortStatus);
	/* Update event ring dequeue pointer before dropping the lock */
	inc_deq(xhci, xhci->event_ring, true);
	xhci_set_hc_event_deq(xhci);
}


u64 ts_irq;
/*
 * If this function returns an error condition, it means it got a Transfer
 * event with a corrupted Slot ID, Endpoint ID, or TRB DMA address.
 * At this point, the host controller is probably hosed and should be reset.
 */
static int handle_tx_event(struct xhci_hcd *xhci,
		struct xhci_transfer_event *event)
{
	struct xhci_virt_device *xdev;
	struct xhci_virt_ep *ep;
	struct xhci_ring *ep_ring;
	unsigned int slot_id;
	int ep_index;
	struct xhci_td *td = NULL;
	dma_addr_t event_dma;
	struct xhci_segment *event_seg;
	union xhci_trb *event_trb;
	struct urb *urb = NULL;
	int status = -EINPROGRESS;
	struct xhci_ep_ctx *ep_ctx;
	u32 trb_comp_code;
	int ret;
	ts_irq = local_clock();
	xhci_dbg(xhci, "Got tx complete event\n");
	trb_comp_code = GET_COMP_CODE(event->transfer_len);
	xhci_dbg(xhci, "trb_comp_code: %d *********************\n", trb_comp_code);
#if 1
	if(trb_comp_code == COMP_UNDERRUN || trb_comp_code == COMP_OVERRUN){
		if(trb_comp_code == COMP_UNDERRUN){
			//xhci_err(xhci, "underrun event on endpoint\n");
		}
		else if(trb_comp_code == COMP_OVERRUN){
			//xhci_err(xhci, "overrun event on endpoint\n");
		}
		goto cleanup;
	}
#endif
#if 0
	if (trb_comp_code == COMP_STOP_INVAL ||
			trb_comp_code == COMP_STOP) {
		/* The Endpoint Stop Command completion will take care of any
		 * stopped TDs.  A stopped TD may be restarted, so don't update
		 * the ring dequeue pointer or take this TD off any lists yet.
		 */
		g_cmd_status = CMD_DONE;
		goto cleanup;
	}
#endif
#if 1
	xhci_dbg(xhci, "In %s\n", __func__);
	slot_id = TRB_TO_SLOT_ID(event->flags);
	xdev = xhci->devs[slot_id];
	if (!xdev) {
		xhci_err(xhci, "[ERROR] Transfer event pointed to bad slot\n");
		return -ENODEV;
	}

	/* Endpoint ID is 1 based, our index is zero based */
	ep_index = TRB_TO_EP_ID(event->flags) - 1;
	xhci_dbg(xhci, "%s - ep index = %d\n", __func__, ep_index);
	ep = &xdev->eps[ep_index];
	ep_ring = xhci_dma_to_transfer_ring(ep, event->buffer);
	ep_ctx = xhci_get_ep_ctx(xhci, xdev->out_ctx, ep_index);
	if (!ep_ring || (ep_ctx->ep_info & EP_STATE_MASK) == EP_STATE_DISABLED) {
		xhci_err(xhci, "[ERROR] Transfer event for disabled endpoint "
				"or incorrect stream ring\n");
		return -ENODEV;
	}

	event_dma = event->buffer;
	/* This TRB should be in the TD at the head of this ring's TD list */
	xhci_dbg(xhci, "%s - checking for list empty\n", __func__);
	if (list_empty(&ep_ring->td_list)) {
		if(!g_test_random_stop_ep){
			xhci_warn(xhci, "WARN Event TRB for slot %d ep %d with no TDs queued?\n",
					TRB_TO_SLOT_ID(event->flags), ep_index);
			xhci_warn(xhci, "Event TRB(0x%x): 0x%x 0x%x 0x%x\n"
				, (unsigned int)event, (unsigned int)event->buffer, event->transfer_len, event->flags);
		
			xhci_dbg(xhci, "Event TRB with TRB type ID %u\n",
					(unsigned int) (event->flags & TRB_TYPE_BITMASK)>>10);
			xhci_print_trb_offsets(xhci, (union xhci_trb *) event);
		}
		urb = NULL;
		goto cleanup;
	}
	xhci_dbg(xhci, "%s - getting list entry\n", __func__);
	td = list_entry(ep_ring->td_list.next, struct xhci_td, td_list);

	/* Is this a TRB in the currently executing TD? */
	xhci_dbg(xhci, "%s - looking for TD\n", __func__);
	event_seg = trb_in_td(ep_ring->deq_seg, ep_ring->dequeue,
			td->last_trb, event_dma);
	xhci_dbg(xhci, "%s - found event_seg = %p\n", __func__, event_seg);
	if (!event_seg) {
		/* HC is busted, give up! */
		xhci_err(xhci, "[ERROR] Transfer event TRB DMA ptr not part of current TD\n");
		return -ESHUTDOWN;
	}
	event_trb = &event_seg->trbs[(event_dma - event_seg->dma) / sizeof(*event_trb)];
	xhci_dbg(xhci, "Event TRB with TRB type ID %u\n",
			(unsigned int) (event->flags & TRB_TYPE_BITMASK)>>10);
	xhci_dbg(xhci, "Offset 0x00 (buffer lo) = 0x%x\n",
			lower_32_bits(event->buffer));
	xhci_dbg(xhci, "Offset 0x04 (buffer hi) = 0x%x\n",
			upper_32_bits(event->buffer));
	xhci_dbg(xhci, "Offset 0x08 (transfer length) = 0x%x\n",
			(unsigned int) event->transfer_len);
	xhci_dbg(xhci, "Offset 0x0C (flags) = 0x%x\n",
			(unsigned int) event->flags);

	/* Look for common error cases */
	trb_comp_code = GET_COMP_CODE(event->transfer_len);
//	xhci_dbg(xhci, "td->urb 0x%x\n", td->urb);
	if (trb_comp_code != COMP_SUCCESS) {
		if (!g_test_random_stop_ep || trb_comp_code != COMP_STOP)
			xhci_warn(xhci, "completion code = %d\n", trb_comp_code);
	}
	switch (trb_comp_code) {
	/* Skip codes that require special handling depending on
	 * transfer type
	 */
	case COMP_SUCCESS:
#if 0
		if(usb_endpoint_xfer_control(&td->urb->ep->desc) && g_con_is_enter){
			udelay(g_con_delay_us);
			f_port_set_pls(g_port_id, g_con_enter_ux);
			udelay(100);
			f_port_set_pls((int)g_port_id, 0);
		}
#endif
		if(!usb_endpoint_xfer_isoc(&td->urb->ep->desc)){
			td->urb->actual_length = td->urb->transfer_buffer_length - GET_TRANSFER_LENGTH(event->transfer_len);
			td->urb->status = 0;
			xhci_dbg(xhci, "urb transfer buffer length: %d\n", td->urb->transfer_buffer_length);
			xhci_dbg(xhci, "event trb transfer length: %d\n", GET_TRANSFER_LENGTH(event->transfer_len));
		}
		break;
	case COMP_SHORT_TX:
		if(!usb_endpoint_xfer_isoc(&td->urb->ep->desc)){
			td->urb->actual_length = td->urb->transfer_buffer_length - GET_TRANSFER_LENGTH(event->transfer_len);
			td->urb->status = 0;
		}
		break;
	case COMP_STOP:
		xhci_dbg(xhci, "Stopped on Transfer TRB\n");
		break;
	case COMP_STOP_INVAL:
		xhci_dbg(xhci, "Stopped on No-op or Link TRB\n");
		break;
	case COMP_STALL:
		xhci_warn(xhci, "EP[%d] WARN: Stalled endpoint.\n", ep_index);
		ep->ep_state |= EP_HALTED;
		td->urb->status = -EPIPE;
		break;
	case COMP_TRB_ERR:
		xhci_warn(xhci, "WARN: TRB error on endpoint\n");
		td->urb->status = -EILSEQ;
		break;
	case COMP_SPLIT_ERR:
	case COMP_TX_ERR:
		xhci_warn(xhci, "EP[%d] WARN: transfer error on endpoint\n", ep_index);
		td->urb->status = -EPROTO;
		break;
	case COMP_BABBLE:
		xhci_warn(xhci, "WARN: babble error on endpoint, ep_idx %d\n", ep_index);
		td->urb->status = -EOVERFLOW;
		break;
	case COMP_DB_ERR:
		xhci_warn(xhci, "WARN: HC couldn't access mem fast enough\n");
		td->urb->status = -ENOSR;
		break;
	case COMP_BW_OVER:
		xhci_warn(xhci, "WARN: bandwidth overrun event on endpoint\n");
		break;
	case COMP_BUFF_OVER:
		xhci_warn(xhci, "WARN: buffer overrun event on endpoint\n");
		break;
	case COMP_UNDERRUN:
		/*
		 * When the Isoch ring is empty, the xHC will generate
		 * a Ring Overrun Event for IN Isoch endpoint or Ring
		 * Underrun Event for OUT Isoch endpoint.
		 */
		xhci_dbg(xhci, "underrun event on endpoint\n");
		if (!list_empty(&ep_ring->td_list))
			xhci_dbg(xhci, "Underrun Event for slot %d ep %d "
					"still with TDs queued?\n",
				TRB_TO_SLOT_ID(event->flags), ep_index);
		break;
	case COMP_OVERRUN:
		xhci_dbg(xhci, "overrun event on endpoint\n");
		if (!list_empty(&ep_ring->td_list))
			xhci_dbg(xhci, "Overrun Event for slot %d ep %d "
					"still with TDs queued?\n",
				TRB_TO_SLOT_ID(event->flags), ep_index);
		break;
	case COMP_MISSED_INT:
		/*
		 * When encounter missed service error, one or more isoc tds
		 * may be missed by xHC.
		 * Set skip flag of the ep_ring; Complete the missed tds as
		 * short transfer when process the ep_ring next time.
		 */
		xhci_dbg(xhci, "Miss service interval error, set skip flag\n");
		break;
	default:
		if (xhci_is_vendor_info_code(xhci, trb_comp_code)) {
			urb->status = 0;
			break;
		}
		xhci_warn(xhci, "ERROR Unknown event condition, HC probably busted, comp_code %d\n", trb_comp_code);
		urb = NULL;
		return -ENODEV;
	}
	/* Now update the urb's actual_length and give back to
	 * the core
	 */
	if (usb_endpoint_xfer_control(&td->urb->ep->desc))
		ret = process_ctrl_td(xhci, td, event_trb, event, ep,
					 &status);
	else if (usb_endpoint_xfer_isoc(&td->urb->ep->desc))
		ret = process_isoc_td(xhci, td, event_trb, event, ep,
					 &status);
	else
		ret = process_bulk_intr_td(xhci, td, event_trb, event,
					 ep, &status);
#if 0
	/* Update ring dequeue pointer */
	while (ep_ring->dequeue != td->last_trb)
		inc_deq(xhci, ep_ring, false);
	inc_deq(xhci, ep_ring, false);
#endif

#if 0
	urb = td->urb;
	list_del(&td->td_list);
	if (usb_endpoint_xfer_control(&urb->ep->desc) ||
		(trb_comp_code != COMP_STALL &&
			trb_comp_code != COMP_BABBLE)) {
		kfree(td);
	}
#endif
	#if 0	
	//td cleanup
	list_del(&td->td_list);
	/* Was this TD slated to be cancelled but completed anyway? */
	if (!list_empty(&td->cancelled_td_list))
		list_del(&td->cancelled_td_list);
	/* Leave the TD around for the reset endpoint function to use
	 * (but only if it's not a control endpoint, since we already
	 * queued the Set TR dequeue pointer command for stalled
	 * control endpoints).
	 */
	if (usb_endpoint_xfer_control(&urb->ep->desc) ||
		(trb_comp_code != COMP_STALL &&
			trb_comp_code != COMP_BABBLE)) {
		kfree(td);
	}
	#endif
#if 0	
	if(dev_list[0] && dev_list[0]->slot_id==slot_id){
		xhci_dbg(xhci, "dev 0 slot_id %d trans done\n", slot_id);
		g_trans_status1 = TRANS_DONE;
	}
	else if(dev_list[1] && dev_list[1]->slot_id==slot_id){
		xhci_dbg(xhci, "dev 1 slot_id %d trans done\n", slot_id);
		g_trans_status2 = TRANS_DONE;
	}
	else if(dev_list[2] && dev_list[2]->slot_id==slot_id){
		xhci_dbg(xhci, "dev 2 slot_id %d trans done\n", slot_id);
		g_trans_status3 = TRANS_DONE;
	}
	else if(dev_list[3] && dev_list[3]->slot_id==slot_id){
		xhci_dbg(xhci, "dev 3 slot_id %d trans done\n", slot_id);
		g_trans_status4 = TRANS_DONE;
	}
#endif
#if 0
	if(ep_index == 1 || ep_index == 2 || (dev_list[0] && dev_list[0]->slot_id==slot_id)){
		g_trans_status1 = TRANS_DONE;
	}
	else if(ep_index == 3 || ep_index == 4 || (dev_list[1] && dev_list[1]->slot_id==slot_id)){
		g_trans_status2 = TRANS_DONE;
	}
	else if(ep_index == 5 || ep_index == 6 || (dev_list[2] && dev_list[2]->slot_id==slot_id)){
		g_trans_status3 = TRANS_DONE;
	}
	else if(ep_index == 7 || ep_index == 8 || (dev_list[3] && dev_list[3]->slot_id==slot_id)){
		g_trans_status4 = TRANS_DONE;
	}
#endif

#endif
cleanup:
	inc_deq(xhci, xhci->event_ring, true);
	xhci_set_hc_event_deq(xhci);

	/* FIXME for multi-TD URBs (who have buffers bigger than 64MB) */
	return 0;
}

int xhci_handle_event(struct xhci_hcd *xhci){
	union xhci_trb *event;
	struct xhci_generic_trb *generic_event;
	int update_ptrs = 1;
	int ret;

	if (!xhci->event_ring || !xhci->event_ring->dequeue) {
		xhci->error_bitmask |= 1 << 1;
		return 0;
	}

	event = xhci->event_ring->dequeue;
	/* Does the HC or OS own the TRB? */
	if ((le32_to_cpu(event->event_cmd.flags) & TRB_CYCLE) !=
	    xhci->event_ring->cycle_state) {
		xhci->error_bitmask |= 1 << 2;
		return 0;
	}
	
#if 1
	if(g_event_full){
		struct xhci_generic_trb *event_trb = &event->generic;
		if(GET_COMP_CODE(event_trb->field[2]) == COMP_ER_FULL){
			xhci_dbg(xhci, "Got event ring full\n");
			g_got_event_full = true;
		}
		else{
			xhci_dbg(xhci, "increase SW dequeue pointer\n");
			inc_deq(xhci, xhci->event_ring, true);
			return 0;
		}
	}
#endif
	if((event->event_cmd.flags & TRB_TYPE_BITMASK) == TRB_TYPE(TRB_MFINDEX_WRAP)){
		g_mfindex_event++;
	}
	
	/*
	 * Barrier between reading the TRB_CYCLE (valid) flag above and any
	 * speculative reads of the event's flags/data below.
	 */
	rmb();

	/* FIXME: Handle more event types. */
	switch ((event->event_cmd.flags & TRB_TYPE_BITMASK)) {
	case TRB_TYPE(TRB_COMPLETION):
#if 1
		xhci_dbg(xhci, "%s - calling handle_cmd_completion\n", __func__);
		handle_cmd_completion(xhci, &event->event_cmd);
		xhci_dbg(xhci, "%s - returned from handle_cmd_completion\n", __func__);
#endif
		break;

	case TRB_TYPE(TRB_PORT_STATUS):
#if 1
		xhci_dbg(xhci, "%s - calling handle_port_status\n", __func__);
		handle_port_status(xhci, event);
		xhci_dbg(xhci, "%s - returned from handle_port_status\n", __func__);
		update_ptrs = 0;
#endif
		break;
	case TRB_TYPE(TRB_TRANSFER):
#if 1
		xhci_dbg(xhci, "%s - calling handle_tx_event\n", __func__);
		ret = handle_tx_event(xhci, &event->trans_event);
		xhci_dbg(xhci, "%s - returned from handle_tx_event\n", __func__);
		if (ret < 0)
			xhci->error_bitmask |= 1 << 9;
		else
			update_ptrs = 0;
#endif
		break;
	case TRB_TYPE(TRB_DEV_NOTE):
		xhci_dbg(xhci, "Got device notification packet\n");
		generic_event  = &event->generic;
		xhci_dbg(xhci, "fields 0x%x 0x%x 0x%x 0x%x\n"
			, generic_event->field[0], generic_event->field[1], generic_event->field[2], generic_event->field[3]);
		g_dev_notification = TRB_DEV_NOTE_TYEP(generic_event->field[0]);
		xhci_dbg(xhci, "notification type %d\n", g_dev_notification);
		g_dev_not_value = TRB_DEV_NOTE_VALUE_LO(generic_event->field[0]);
			//| (generic_event->field[1] << 32);
		xhci_dbg(xhci, "notification value %ld\n", g_dev_not_value);
		break;
	default:
		break;
#if 0
		if ((event->event_cmd.flags & TRB_TYPE_BITMASK) >= TRB_TYPE(48))
			handle_vendor_event(xhci, event);
		else
			xhci->error_bitmask |= 1 << 3;
#endif
	}
	/* Any of the above functions may drop and re-acquire the lock, so check
	 * to make sure a watchdog timer didn't mark the host as non-responsive.
	 */
	if (xhci->xhc_state & XHCI_STATE_DYING) {
		xhci_dbg(xhci, "xHCI host dying, returning from "
				"event handler.\n");
		return 0;
	}

	if (update_ptrs)
		/* Update SW event ring dequeue pointer */
		inc_deq(xhci, xhci->event_ring, true);

	/* Are there more items on the event ring?  Caller will call us again to
	 * check.
	 */
	return 1;
}


/****		Endpoint Ring Operations	****/

/*
 * Generic function for queueing a TRB on a ring.
 * The caller must have checked to make sure there's room on the ring.
 *
 * @more_trbs_coming:	Will you enqueue more TRBs before calling
 *			prepare_transfer()?
 */
static void queue_trb(struct xhci_hcd *xhci, struct xhci_ring *ring,
		bool consumer, bool more_trbs_coming,
		u32 field1, u32 field2, u32 field3, u32 field4)
{
	struct xhci_generic_trb *trb;

	trb = &ring->enqueue->generic;
	trb->field[0] = field1;
	trb->field[1] = field2;
	trb->field[2] = field3;
	trb->field[3] = field4;
	xhci_dbg(xhci, "Dump TRB: 0x%x 0x%x 0x%x 0x%x\n", trb->field[0], trb->field[1], trb->field[2], trb->field[3]);
	inc_enq(xhci, ring, consumer, more_trbs_coming);
}

/*
 * Does various checks on the endpoint ring, and makes it ready to queue num_trbs.
 * FIXME allocate segments if the ring is full.
 */
static int prepare_ring(struct xhci_hcd *xhci, struct xhci_ring *ep_ring,
		u32 ep_state, unsigned int num_trbs, gfp_t mem_flags)
{
	/* Make sure the endpoint has been added to xHC schedule */
	xhci_dbg(xhci, "Endpoint state = 0x%x\n", ep_state);
	switch (ep_state) {
	case EP_STATE_DISABLED:
		/*
		 * USB core changed config/interfaces without notifying us,
		 * or hardware is reporting the wrong state.
		 */
		xhci_warn(xhci, "WARN urb submitted to disabled ep\n");
		return -ENOENT;
	case EP_STATE_ERROR:
		xhci_warn(xhci, "WARN waiting for error on ep to be cleared\n");
		/* FIXME event handling code for error needs to clear it */
		/* XXX not sure if this should be -ENOENT or not */
		return -EINVAL;
	case EP_STATE_HALTED:
		xhci_dbg(xhci, "WARN halted endpoint, queueing URB anyway.\n");
	case EP_STATE_STOPPED:
	case EP_STATE_RUNNING:
		break;
	default:
		xhci_err(xhci, "[ERROR] unknown endpoint state for ep\n");
		/*
		 * FIXME issue Configure Endpoint command to try to get the HC
		 * back into a known state.
		 */
		return -EINVAL;
	}
	if (!room_on_ring(xhci, ep_ring, num_trbs)) {
		/* FIXME allocate more room */
		xhci_err(xhci, "[ERROR] no room on ep ring, num_trbs %d\n", num_trbs);
		return -ENOMEM;
	}

	if (enqueue_is_link_trb(ep_ring)) {
		struct xhci_ring *ring = ep_ring;
		union xhci_trb *next;

		xhci_dbg(xhci, "prepare_ring: pointing to link trb\n");
		next = ring->enqueue;

		while (last_trb(xhci, ring, ring->enq_seg, next)) {

			/* If we're not dealing with 0.95 hardware,
			 * clear the chain bit.
			 */
			if (!xhci_link_trb_quirk(xhci))
				next->link.control &= ~TRB_CHAIN;
			else
				next->link.control |= TRB_CHAIN;

			wmb();
			next->link.control ^= (u32) TRB_CYCLE;

			/* Toggle the cycle bit after the last ring segment. */
			if (last_trb_on_last_seg(xhci, ring, ring->enq_seg, next)) {
				ring->cycle_state = (ring->cycle_state ? 0 : 1);
				if (!in_interrupt()) {
					xhci_dbg(xhci, "queue_trb: Toggle cycle "
						"state for ring %p = %i\n",
						ring, (unsigned int)ring->cycle_state);
				}
			}
			ring->enq_seg = ring->enq_seg->next;
			ring->enqueue = ring->enq_seg->trbs;
			next = ring->enqueue;
		}
	}

	return 0;
}

static int prepare_transfer(struct xhci_hcd *xhci,
		struct xhci_virt_device *xdev,
		unsigned int ep_index,
		unsigned int stream_id,
		unsigned int num_trbs,
		struct urb *urb,
		unsigned int td_index,
		gfp_t mem_flags)
{
	int ret;
	struct urb_priv *urb_priv;
	struct xhci_td	*td;
	struct xhci_ring *ep_ring;
	struct xhci_ep_ctx *ep_ctx = xhci_get_ep_ctx(xhci, xdev->out_ctx, ep_index);

	ep_ring = xhci_stream_id_to_ring(xdev, ep_index, stream_id);
	if (!ep_ring) {
		xhci_dbg(xhci, "Can't prepare ring for bad stream ID %u\n",
				stream_id);
		return -EINVAL;
	}
	xhci_dbg(xhci, "prepare transfer EP[%d]\n", ep_index);
	ret = prepare_ring(xhci, ep_ring,
			ep_ctx->ep_info & EP_STATE_MASK,
			num_trbs, mem_flags);
	if (ret)
		return ret;

	urb_priv = urb->hcpriv;
	td = urb_priv->td[td_index];

	INIT_LIST_HEAD(&td->td_list);
	INIT_LIST_HEAD(&td->cancelled_td_list);

	td->urb = urb;
	list_add_tail(&td->td_list, &ep_ring->td_list);
	td->start_seg = ep_ring->enq_seg;
	td->first_trb = ep_ring->enqueue;

	urb_priv->td[td_index] = td;

	return 0;
}

static unsigned int count_sg_trbs_needed(struct xhci_hcd *xhci, struct urb *urb)
{
	int num_sgs, num_trbs, running_total, temp, i;
	struct scatterlist *sg;

	sg = NULL;
	num_sgs = urb->num_sgs;
	temp = urb->transfer_buffer_length;

	xhci_dbg(xhci, "count sg list trbs: \n");
	num_trbs = 0;
	for_each_sg(urb->sg, sg, num_sgs, i) {
		unsigned int previous_total_trbs = num_trbs;
		unsigned int len = sg_dma_len(sg);

		/* Scatter gather list entries may cross 64KB boundaries */
		running_total = TRB_MAX_BUFF_SIZE -
			(sg_dma_address(sg) & ((1 << TRB_MAX_BUFF_SHIFT) - 1));
		if (running_total != 0)
			num_trbs++;

		/* How many more 64KB chunks to transfer, how many more TRBs? */
		while (running_total < sg_dma_len(sg)) {
			num_trbs++;
			running_total += TRB_MAX_BUFF_SIZE;
		}
		xhci_dbg(xhci, " sg #%d: dma = %#llx, len = %#x (%d), num_trbs = %d\n",
				i, (unsigned long long)sg_dma_address(sg),
				len, len, num_trbs - previous_total_trbs);

		len = min_t(int, len, temp);
		temp -= len;
		if (temp == 0)
			break;
	}
	xhci_dbg(xhci, "\n");
	if (!in_interrupt())
		dev_dbg(&urb->dev->dev, "ep %#x - urb len = %d, sglist used, num_trbs = %d\n",
				urb->ep->desc.bEndpointAddress,
				urb->transfer_buffer_length,
				num_trbs);
	return num_trbs;
}

static void check_trb_math(struct urb *urb, int num_trbs, int running_total)
{
	if (num_trbs != 0)
		dev_dbg(&urb->dev->dev, "%s - ep %#x - Miscalculated number of "
				"TRBs, %d left\n", __func__,
				urb->ep->desc.bEndpointAddress, num_trbs);
	if (running_total != urb->transfer_buffer_length)
		dev_dbg(&urb->dev->dev, "%s - ep %#x - Miscalculated tx length, "
				"queued %#x (%d), asked for %#x (%d)\n",
				__func__,
				urb->ep->desc.bEndpointAddress,
				running_total, running_total,
				urb->transfer_buffer_length,
				urb->transfer_buffer_length);
}

static void giveback_first_trb(struct xhci_hcd *xhci, int slot_id,
		unsigned int ep_index, unsigned int stream_id, int start_cycle,
		struct xhci_generic_trb *start_trb, struct xhci_td *td)
{
	/*
	 * Pass all the TRBs to the hardware at once and make sure this write
	 * isn't reordered.
	 */
	wmb();
	if (start_cycle)
		start_trb->field[3] |= start_cycle;
	else
		start_trb->field[3] &= ~0x1;
	ring_ep_doorbell(xhci, slot_id, ep_index, stream_id);
}

/*
 * xHCI uses normal TRBs for both bulk and interrupt.  When the interrupt
 * endpoint is to be serviced, the xHC will consume (at most) one TD.  A TD
 * (comprised of sg list entries) can take several service intervals to
 * transmit.
 */
int xhci_queue_intr_tx(struct xhci_hcd *xhci, gfp_t mem_flags,
		struct urb *urb, int slot_id, unsigned int ep_index)
{
	struct xhci_ep_ctx *ep_ctx = xhci_get_ep_ctx(xhci,
			xhci->devs[slot_id]->out_ctx, ep_index);
	int xhci_interval;
	int ep_interval;

	xhci_interval = EP_INTERVAL_TO_UFRAMES(ep_ctx->ep_info);
	ep_interval = urb->interval;
	/* Convert to microframes */
	if (urb->dev->speed == USB_SPEED_LOW ||
			urb->dev->speed == USB_SPEED_FULL)
		ep_interval *= 8;
	/* FIXME change this to a warning and a suggestion to use the new API
	 * to set the polling interval (once the API is added).
	 */
	if (xhci_interval != ep_interval) {
		if (!printk_ratelimit())
			dev_dbg(&urb->dev->dev, "Driver uses different interval"
					" (%d microframe%s) than xHCI "
					"(%d microframe%s)\n",
					ep_interval,
					ep_interval == 1 ? "" : "s",
					xhci_interval,
					xhci_interval == 1 ? "" : "s");
		urb->interval = xhci_interval;
		/* Convert back to frames for LS/FS devices */
		if (urb->dev->speed == USB_SPEED_LOW ||
				urb->dev->speed == USB_SPEED_FULL)
			urb->interval /= 8;
	}
	return xhci_queue_bulk_tx(xhci, GFP_ATOMIC, urb, slot_id, ep_index);
}

/*
 * The TD size is the number of bytes remaining in the TD (including this TRB),
 * right shifted by 10.
 * It must fit in bits 21:17, so it can't be bigger than 31.
 */
static u32 xhci_td_remainder(unsigned int td_transfer_size, unsigned int td_running_total
	, unsigned int maxp, unsigned trb_buffer_length)
{
	u32 max = 31;
	int remainder, td_packet_count, packet_transferred;

	//0 for the last TRB
	//FIXME: need to workaround if there is ZLP in this TD
	if(td_running_total + trb_buffer_length == td_transfer_size)
		return 0;

	//FIXME: need to take care of high-bandwidth (MAX_ESIT)
	packet_transferred = (td_running_total /*+ trb_buffer_length*/) / maxp;
	td_packet_count = DIV_ROUND_UP(td_transfer_size, maxp);
	remainder = td_packet_count - packet_transferred;
	
	if(remainder > max)
		return max << 17;
	else
		return remainder << 17;

}

static int queue_bulk_sg_tx(struct xhci_hcd *xhci, gfp_t mem_flags,
		struct urb *urb, int slot_id, unsigned int ep_index)
{
	struct xhci_ring *ep_ring;
	unsigned int num_trbs;
	struct urb_priv *urb_priv;
	struct xhci_td *td;
	struct scatterlist *sg;
	int num_sgs;
	int trb_buff_len, this_sg_len, running_total;
	bool first_trb;
	u64 addr;
	int max_packet;
	bool more_trbs_coming;
	bool zlp;
//	int td_packet_count, trb_tx_len_sum, packet_transferred, trb_residue, td_size;

	struct xhci_generic_trb *start_trb;
	int start_cycle;

	ep_ring = xhci_urb_to_transfer_ring(xhci, urb);
	if (!ep_ring)
		return -EINVAL;

	num_trbs = count_sg_trbs_needed(xhci, urb);
	num_sgs = urb->num_sgs;

	trb_buff_len = prepare_transfer(xhci, xhci->devs[slot_id],
		ep_index, urb->stream_id, num_trbs, urb, 0, mem_flags);
	if (trb_buff_len < 0)
		return trb_buff_len;
	
	urb_priv = urb->hcpriv;
	td = urb_priv->td[0];

	zlp = false;
	/* FIXME: this doesn't deal with URB_ZERO_PACKET - need one more */
	switch(urb->dev->speed){
		case USB_SPEED_SUPER:
			max_packet = urb->ep->desc.wMaxPacketSize;
			break;
		case USB_SPEED_HIGH:
		case USB_SPEED_FULL:
		case USB_SPEED_LOW:
			max_packet = urb->ep->desc.wMaxPacketSize & 0x7ff;
			break;
		default:
			break;
	}
	if((urb->transfer_flags & URB_ZERO_PACKET) 
		&& ((urb->transfer_buffer_length % max_packet) == 0)){
		zlp = true;
	}
#if 0
	td_packet_count = urb->transfer_buffer_length/max_packet + (urb->transfer_buffer_length%max_packet > 0 ? 1 : 0);
	trb_tx_len_sum = 0;
	packet_transferred = 0;
#endif
	/*****************************************************************/
	
	/*
	 * Don't give the first TRB to the hardware (by toggling the cycle bit)
	 * until we've finished creating all the other TRBs.  The ring's cycle
	 * state may change as we enqueue the other TRBs, so save it too.
	 */
	start_trb = &ep_ring->enqueue->generic;
	start_cycle = ep_ring->cycle_state;

	running_total = 0;
	/*
	 * How much data is in the first TRB?
	 *
	 * There are three forces at work for TRB buffer pointers and lengths:
	 * 1. We don't want to walk off the end of this sg-list entry buffer.
	 * 2. The transfer length that the driver requested may be smaller than
	 *    the amount of memory allocated for this scatter-gather list.
	 * 3. TRBs buffers can't cross 64KB boundaries.
	 */
	sg = urb->sg;
	addr = (u64) sg_dma_address(sg);
	this_sg_len = sg_dma_len(sg);
	trb_buff_len = TRB_MAX_BUFF_SIZE -
		(addr & ((1 << TRB_MAX_BUFF_SHIFT) - 1));
	trb_buff_len = min_t(int, trb_buff_len, this_sg_len);
	if (trb_buff_len > urb->transfer_buffer_length)
		trb_buff_len = urb->transfer_buffer_length;
	xhci_dbg(xhci, "First length to xfer from 1st sglist entry = %u\n",
			trb_buff_len);

	first_trb = true;
	/* Queue the first TRB, even if it's zero-length */
	do {
		u32 field = 0;
		u32 length_field = 0;
		u32 remainder = 0;

		/* Don't change the cycle bit of the first TRB until later */
		if (first_trb){
			first_trb = false;
			if (start_cycle == 0)
				field |= 0x1;
		}
		else
			field |= ep_ring->cycle_state;

		/* Chain all the TRBs together; clear the chain bit in the last
		 * TRB to indicate it's the last TRB in the chain.
		 */
		if (num_trbs > 1 || zlp) {
			field |= TRB_CHAIN;
		} else {
			/* FIXME - add check for ZERO_PACKET flag before this */
			td->last_trb = ep_ring->enqueue;
			field |= TRB_IOC;
		}
		xhci_dbg(xhci, " sg entry: dma = %#x, len = %#x (%d), "
				"64KB boundary at %#x, end dma = %#x\n",
				(unsigned int) addr, trb_buff_len, trb_buff_len,
				(unsigned int) (addr + TRB_MAX_BUFF_SIZE) & ~(TRB_MAX_BUFF_SIZE - 1),
				(unsigned int) addr + trb_buff_len);
		if (TRB_MAX_BUFF_SIZE -
				(addr & ((1 << TRB_MAX_BUFF_SHIFT) - 1)) < trb_buff_len) {
			xhci_warn(xhci, "WARN: sg dma xfer crosses 64KB boundaries!\n");
			xhci_dbg(xhci, "Next boundary at %#x, end dma = %#x\n",
					(unsigned int) (addr + TRB_MAX_BUFF_SIZE) & ~(TRB_MAX_BUFF_SIZE - 1),
					(unsigned int) addr + trb_buff_len);
		}
		remainder = xhci_td_remainder(urb->transfer_buffer_length, running_total, max_packet, trb_buff_len);
		length_field = TRB_LEN(trb_buff_len) |
			remainder |
			TRB_INTR_TARGET(0);
		if (num_trbs > 1 || zlp)
			more_trbs_coming = true;
		else
			more_trbs_coming = false;
		xhci_dbg(xhci, "queue trb, len[%d], addr[0x%x]\n", trb_buff_len, (unsigned int)addr);
		queue_trb(xhci, ep_ring, false, more_trbs_coming,
				lower_32_bits(addr),
				upper_32_bits(addr),
				length_field,
				/* We always want to know if the TRB was short,
				 * or we won't get an event when it completes.
				 * (Unless we use event data TRBs, which are a
				 * waste of space and HC resources.)
				 */
				field | TRB_ISP | TRB_TYPE(TRB_NORMAL));
		--num_trbs;
		running_total += trb_buff_len;

		/* Calculate length for next transfer --
		 * Are we done queueing all the TRBs for this sg entry?
		 */
		this_sg_len -= trb_buff_len;
		if (this_sg_len == 0) {
			--num_sgs;
			if (num_sgs == 0)
				break;
			sg = sg_next(sg);
			addr = (u64) sg_dma_address(sg);
			this_sg_len = sg_dma_len(sg);
		} else {
			addr += trb_buff_len;
		}

		trb_buff_len = TRB_MAX_BUFF_SIZE -
			(addr & ((1 << TRB_MAX_BUFF_SHIFT) - 1));
		trb_buff_len = min_t(int, trb_buff_len, this_sg_len);
		if (running_total + trb_buff_len > urb->transfer_buffer_length)
			trb_buff_len =
				urb->transfer_buffer_length - running_total;
	} while (num_trbs > 0/*running_total < urb->transfer_buffer_length*/);
	if(zlp){
		u32 field = 0;
		u32 length_field = 0;
		length_field = TRB_LEN(0) |	TRB_INTR_TARGET(0);
		field |= ep_ring->cycle_state;
		field |= TRB_IOC;
		td->last_trb = ep_ring->enqueue;
		xhci_dbg(xhci, "queue trb, len[0x%x], addr[0x%x]\n", length_field, (unsigned int)addr);
		queue_trb(xhci, ep_ring, false, false,
				lower_32_bits(addr),
				upper_32_bits(addr),
				length_field,
				/* We always want to know if the TRB was short,
				 * or we won't get an event when it completes.
				 * (Unless we use event data TRBs, which are a
				 * waste of space and HC resources.)
				 */
				field | TRB_ISP | TRB_TYPE(TRB_NORMAL));
	}
	check_trb_math(urb, num_trbs, running_total);
	giveback_first_trb(xhci, slot_id, ep_index, urb->stream_id,
			start_cycle, start_trb, td);
	return 0;
}

/* This is very similar to what ehci-q.c qtd_fill() does */
int xhci_queue_bulk_tx(struct xhci_hcd *xhci, gfp_t mem_flags,
		struct urb *urb, int slot_id, unsigned int ep_index)
{
	struct xhci_ring *ep_ring;
	struct urb_priv *urb_priv;
	struct xhci_td *td;
	int num_trbs;
	struct xhci_generic_trb *start_trb;
	bool first_trb;
	bool more_trbs_coming;
	int start_cycle;
	u32 field, length_field;
	int max_packet;
	int running_total, trb_buff_len, ret;
	u64 addr;

	max_packet = 0;

	if (urb->num_sgs)
		return queue_bulk_sg_tx(xhci, mem_flags, urb, slot_id, ep_index);

	ep_ring = xhci_urb_to_transfer_ring(xhci, urb);
	if (!ep_ring){
		xhci_err(xhci, "xhci_queue_bulk_tx, Get transfer ring failed\n");
		return -EINVAL;
	}
	num_trbs = 0;
	/* How much data is (potentially) left before the 64KB boundary? */
	running_total = TRB_MAX_BUFF_SIZE -
		(urb->transfer_dma & ((1 << TRB_MAX_BUFF_SHIFT) - 1));

	/* If there's some data on this 64KB chunk, or we have to send a
	 * zero-length transfer, we need at least one TRB
	 */
	if (running_total != 0 || urb->transfer_buffer_length == 0)
		num_trbs++;
	/* How many more 64KB chunks to transfer, how many more TRBs? */
	while (running_total < urb->transfer_buffer_length) {
		num_trbs++;
		running_total += TRB_MAX_BUFF_SIZE;
	}
#if 1
	/* FIXME: this doesn't deal with URB_ZERO_PACKET - need one more */
	switch(urb->dev->speed){
		case USB_SPEED_SUPER:
			max_packet = urb->ep->desc.wMaxPacketSize;
			break;
		case USB_SPEED_HIGH:
		case USB_SPEED_FULL:
		case USB_SPEED_LOW:
			max_packet = urb->ep->desc.wMaxPacketSize & 0x7ff;
			break;
		default:
			break;
	}
	if((urb->transfer_flags & URB_ZERO_PACKET) 
		&& ((urb->transfer_buffer_length % max_packet) == 0)){
		num_trbs++;
	}
	/*****************************************************************/
#endif
	

	if (!in_interrupt())
		dev_dbg(&urb->dev->dev, "ep %#x - urb len = %#x (%d), addr = %#llx, num_trbs = %d\n",
				urb->ep->desc.bEndpointAddress,
				urb->transfer_buffer_length,
				urb->transfer_buffer_length,
				(unsigned long long)urb->transfer_dma,
				num_trbs);

	ret = prepare_transfer(xhci, xhci->devs[slot_id],
		ep_index, urb->stream_id,
		num_trbs, urb, 0, mem_flags);
	if (ret < 0)
		return ret;

	urb_priv = urb->hcpriv;
	td = urb_priv->td[0];

	/*
	 * Don't give the first TRB to the hardware (by toggling the cycle bit)
	 * until we've finished creating all the other TRBs.  The ring's cycle
	 * state may change as we enqueue the other TRBs, so save it too.
	 */
	start_trb = &ep_ring->enqueue->generic;
	start_cycle = ep_ring->cycle_state;

	running_total = 0;
	/* How much data is in the first TRB? */
	addr = (u64) urb->transfer_dma;
	trb_buff_len = TRB_MAX_BUFF_SIZE -
		(urb->transfer_dma & ((1 << TRB_MAX_BUFF_SHIFT) - 1));
	if (urb->transfer_buffer_length < trb_buff_len)
		trb_buff_len = urb->transfer_buffer_length;

	first_trb = true;

	/* Queue the first TRB, even if it's zero-length */
	do {
		u32 remainder = 0;
		field = 0;

		/* Don't change the cycle bit of the first TRB until later */
		if (first_trb){
			first_trb = false;
			if (start_cycle == 0)
					field |= 0x1;
			}
		else
			field |= ep_ring->cycle_state;

		/* Chain all the TRBs together; clear the chain bit in the last
		 * TRB to indicate it's the last TRB in the chain.
		 */
		if (num_trbs > 1) {
			field |= TRB_CHAIN;
		} else {
			/* FIXME - add check for ZERO_PACKET flag before this */
			td->last_trb = ep_ring->enqueue;
			field |= TRB_IOC;
			if(g_is_bei){
				field |= TRB_BEI;
			}
		}
		remainder = xhci_td_remainder(urb->transfer_buffer_length, running_total, max_packet, trb_buff_len);
		length_field = TRB_LEN(trb_buff_len) |
			remainder |
			TRB_INTR_TARGET(0);
		if (num_trbs > 1)
			more_trbs_coming = true;
		else
			more_trbs_coming = false;
//		xhci_dbg(xhci, "queue trb, len[%d], addr[0x%x]\n", trb_buff_len, addr);

		if(g_idt_transfer && !usb_endpoint_dir_in(&urb->ep->desc)){
			struct xhci_generic_trb *trb;
			u32 *idt_data;
			
			idt_data = urb->transfer_buffer;
			xhci_err(xhci, "idt_data: 0x%x\n", (unsigned int)idt_data);
			trb = &ep_ring->enqueue->generic;
			trb->field[0] = *idt_data;
			idt_data++;
			trb->field[1] = *idt_data;
			trb->field[2] = length_field;
			trb->field[3] = field | TRB_ISP | TRB_TYPE(TRB_NORMAL) | TRB_IDT;
			xhci_dbg(xhci, "Dump TRB: 0x%x 0x%x 0x%x 0x%x\n", trb->field[0], trb->field[1], trb->field[2], trb->field[3]);
			inc_enq(xhci, ep_ring, false, more_trbs_coming);
		}
		else{
			queue_trb(xhci, ep_ring, false, more_trbs_coming,
					lower_32_bits(addr),
					upper_32_bits(addr),
					length_field,
					/* We always want to know if the TRB was short,
					 * or we won't get an event when it completes.
					 * (Unless we use event data TRBs, which are a
					 * waste of space and HC resources.)
					 */
					field | TRB_ISP | TRB_TYPE(TRB_NORMAL));
		}
		--num_trbs;
		running_total += trb_buff_len;

		/* Calculate length for next transfer */
		addr += trb_buff_len;
		trb_buff_len = urb->transfer_buffer_length - running_total;
		if (trb_buff_len > TRB_MAX_BUFF_SIZE)
			trb_buff_len = TRB_MAX_BUFF_SIZE;
	} while (num_trbs > 0/*running_total < urb->transfer_buffer_length*/);

	check_trb_math(urb, num_trbs, running_total);
	if(g_td_to_noop){
		if (start_cycle)
			start_trb->field[3] |= start_cycle;
		else
			start_trb->field[3] &= ~0x1;
		td_to_noop(xhci, ep_ring, td);
		list_del(&td->td_list);
		return 0;
	}
	giveback_first_trb(xhci, slot_id, ep_index, urb->stream_id,
			start_cycle, start_trb, td);
	return 0;
}

static int count_isoc_trbs_needed(struct xhci_hcd *xhci,
		struct urb *urb, int i)
{
	int num_trbs = 0;
	u64 addr, td_len, running_total;

	addr = (u64) (urb->transfer_dma + urb->iso_frame_desc[i].offset);
	td_len = urb->iso_frame_desc[i].length;

	running_total = TRB_MAX_BUFF_SIZE -
			(addr & ((1 << TRB_MAX_BUFF_SHIFT) - 1));
	if (running_total != 0)
		num_trbs++;

	while (running_total < td_len) {
		num_trbs++;
		running_total += TRB_MAX_BUFF_SIZE;
	}

	return num_trbs;
}


/* This is for isoc transfer */
static int xhci_queue_isoc_tx(struct xhci_hcd *xhci, gfp_t mem_flags,
		struct urb *urb, int slot_id, unsigned int ep_index)
{
	struct xhci_ring *ep_ring;
	struct urb_priv *urb_priv;
	struct xhci_td *td;
	int num_tds, trbs_per_td;
	struct xhci_generic_trb *start_trb;
	bool first_trb;
	int start_cycle;
	u32 field, length_field;
	int running_total, trb_buff_len, td_len, td_remain_len, ret;
	u64 start_addr, addr;
	int i, j;
	bool more_trbs_coming;
	int max_packet;
//	int max_esit_payload;
	int frame_id;

	ep_ring = xhci->devs[slot_id]->eps[ep_index].ring;

	num_tds = urb->number_of_packets;
	if (num_tds < 1) {
		xhci_dbg(xhci, "Isoc URB with zero packets?\n");
		return -EINVAL;
	}

	if (!in_interrupt())
		dev_dbg(&urb->dev->dev, "ep %#x - urb len = %#x (%d),"
				" addr = %#llx, num_tds = %d\n",
				urb->ep->desc.bEndpointAddress,
				urb->transfer_buffer_length,
				urb->transfer_buffer_length,
				(unsigned long long)urb->transfer_dma,
				num_tds);

	start_addr = (u64) urb->transfer_dma;
	start_trb = &ep_ring->enqueue->generic;
	start_cycle = ep_ring->cycle_state;
	switch(urb->dev->speed){
		case USB_SPEED_SUPER:
			max_packet = urb->ep->desc.wMaxPacketSize;
			break;
		case USB_SPEED_HIGH:
		case USB_SPEED_FULL:
		case USB_SPEED_LOW:
			max_packet = urb->ep->desc.wMaxPacketSize & 0x7ff;
			break;
		default:
			break;
	}

	/* Queue the first TRB, even if it's zero-length */
	for (i = 0; i < num_tds; i++) {
		first_trb = true;

		running_total = 0;
		addr = start_addr + urb->iso_frame_desc[i].offset;
		td_len = urb->iso_frame_desc[i].length;
		td_remain_len = td_len;

		trbs_per_td = count_isoc_trbs_needed(xhci, urb, i);

		ret = prepare_transfer(xhci, xhci->devs[slot_id], ep_index,
				urb->stream_id, trbs_per_td, urb, i, mem_flags);
		if (ret < 0)
			return ret;

		urb_priv = urb->hcpriv;
		td = urb_priv->td[i];
		for (j = 0; j < trbs_per_td; j++) {
			u32 remainder = 0;
			field = 0;

			if (first_trb) {
				/* Queue the isoc TRB */
				field |= TRB_TYPE(TRB_ISOC);
				/* Assume URB_ISO_ASAP is set */
				if(g_iso_frame && i==0){
					frame_id = xhci_readl(xhci, &xhci->run_regs->microframe_index);
					frame_id = frame_id >> 3;
					frame_id &= 0x7ff;
					frame_id--;
					if(frame_id <0){
						frame_id = 0x7ff;
					}
					field |= ((frame_id) << 20);
					xhci_err(xhci, "[DBG]start frame id = %d\n", frame_id);
				}
				else{
					field |= TRB_SIA;
				}
				if (i == 0) {
					if (start_cycle == 0)
						field |= 0x1;
				} else
					field |= ep_ring->cycle_state;
				first_trb = false;
			} else {
				/* Queue other normal TRBs */
				field |= TRB_TYPE(TRB_NORMAL);
				field |= ep_ring->cycle_state;
			}

			/* Chain all the TRBs together; clear the chain bit in
			 * the last TRB to indicate it's the last TRB in the
			 * chain.
			 */
			if (j < trbs_per_td - 1) {
				field |= TRB_CHAIN;
				more_trbs_coming = true;
			} else {
				td->last_trb = ep_ring->enqueue;
				field |= TRB_IOC;
				more_trbs_coming = false;
			}

			/* Calculate TRB length */
			trb_buff_len = TRB_MAX_BUFF_SIZE -
				(addr & ((1 << TRB_MAX_BUFF_SHIFT) - 1));
			if (trb_buff_len > td_remain_len)
				trb_buff_len = td_remain_len;

//			remainder = xhci_td_remainder(td_len - running_total);
			remainder = xhci_td_remainder(td_len, running_total, max_packet, trb_buff_len);
			length_field = TRB_LEN(trb_buff_len) |
				remainder |
				TRB_INTR_TARGET(0);
			queue_trb(xhci, ep_ring, false, more_trbs_coming,
				lower_32_bits(addr),
				upper_32_bits(addr),
				length_field,
				/* We always want to know if the TRB was short,
				 * or we won't get an event when it completes.
				 * (Unless we use event data TRBs, which are a
				 * waste of space and HC resources.)
				 */
				field | TRB_ISP);
			running_total += trb_buff_len;

			addr += trb_buff_len;
			td_remain_len -= trb_buff_len;
		}

		/* Check TD length */
		if (running_total != td_len) {
			xhci_err(xhci, "ISOC TD length unmatch\n");
			return -EINVAL;
		}
	}

	giveback_first_trb(xhci, slot_id, ep_index, urb->stream_id,
			start_cycle, start_trb, td);
	return 0;
}

/*
 * Check transfer ring to guarantee there is enough room for the urb.
 * Update ISO URB start_frame and interval.
 * Update interval as xhci_queue_intr_tx does. Just use xhci frame_index to
 * update the urb->start_frame by now.
 * Always assume URB_ISO_ASAP set, and NEVER use urb->start_frame as input.
 */
int xhci_queue_isoc_tx_prepare(struct xhci_hcd *xhci, gfp_t mem_flags,
		struct urb *urb, int slot_id, unsigned int ep_index)
{
	struct xhci_virt_device *xdev;
	struct xhci_ring *ep_ring;
	struct xhci_ep_ctx *ep_ctx;
	int start_frame;
	int xhci_interval;
	int ep_interval;
	int num_tds, num_trbs, i;
	int ret;

	xdev = xhci->devs[slot_id];
	ep_ring = xdev->eps[ep_index].ring;
	ep_ctx = xhci_get_ep_ctx(xhci, xdev->out_ctx, ep_index);

	num_trbs = 0;
	num_tds = urb->number_of_packets;
	for (i = 0; i < num_tds; i++)
		num_trbs += count_isoc_trbs_needed(xhci, urb, i);

	/* Check the ring to guarantee there is enough room for the whole urb.
	 * Do not insert any td of the urb to the ring if the check failed.
	 */
	ret = prepare_ring(xhci, ep_ring, ep_ctx->ep_info & EP_STATE_MASK,
				num_trbs, mem_flags);
	if (ret)
		return ret;

	start_frame = xhci_readl(xhci, &xhci->run_regs->microframe_index);
	start_frame &= 0x3fff;

	urb->start_frame = start_frame;
	if (urb->dev->speed == USB_SPEED_LOW ||
			urb->dev->speed == USB_SPEED_FULL)
		urb->start_frame >>= 3;

	xhci_interval = EP_INTERVAL_TO_UFRAMES(ep_ctx->ep_info);
	ep_interval = urb->interval;
	/* Convert to microframes */
	if (urb->dev->speed == USB_SPEED_LOW ||
			urb->dev->speed == USB_SPEED_FULL)
		ep_interval *= 8;
	/* FIXME change this to a warning and a suggestion to use the new API
	 * to set the polling interval (once the API is added).
	 */
	if (xhci_interval != ep_interval) {
		if (printk_ratelimit())
			dev_dbg(&urb->dev->dev, "Driver uses different interval"
					" (%d microframe%s) than xHCI "
					"(%d microframe%s)\n",
					ep_interval,
					ep_interval == 1 ? "" : "s",
					xhci_interval,
					xhci_interval == 1 ? "" : "s");
		urb->interval = xhci_interval;
		/* Convert back to frames for LS/FS devices */
		if (urb->dev->speed == USB_SPEED_LOW ||
				urb->dev->speed == USB_SPEED_FULL)
			urb->interval /= 8;
	}
	return xhci_queue_isoc_tx(xhci, GFP_ATOMIC, urb, slot_id, ep_index);
}

/* Caller must have locked xhci->lock */
int xhci_queue_ctrl_tx(struct xhci_hcd *xhci, gfp_t mem_flags,
		struct urb *urb, int slot_id, unsigned int ep_index)
{
	struct xhci_ring *ep_ring;
	int num_trbs;
	int ret;
	struct usb_ctrlrequest *setup;
	struct xhci_generic_trb *start_trb;
	int start_cycle;
	u32 field, length_field;
	struct urb_priv *urb_priv;
	struct xhci_td *td;
	int max_packet;
//	int remainder;
	u32 trt;
#if 0
	xhci_dbg(xhci, "urb->ep->desc->bLength 0x%x\n", urb->ep->desc.bLength);
	xhci_dbg(xhci, "urb->ep->desc->bDescriptorType 0x%x\n", urb->ep->desc.bDescriptorType);
	xhci_dbg(xhci, "urb->ep->desc->bEndpointAddress 0x%x\n", urb->ep->desc.bEndpointAddress);
	xhci_dbg(xhci, "urb->ep->desc->bmAttributes 0x%x\n", urb->ep->desc.bmAttributes);
	xhci_dbg(xhci, "urb->ep->desc->wMaxPacketSize 0x%x\n", urb->ep->desc.wMaxPacketSize);
	xhci_dbg(xhci, "urb->ep->desc->bInterval 0x%x\n", urb->ep->desc.bInterval);
	xhci_dbg(xhci, "urb->ep->desc->bRefresh 0x%x\n", urb->ep->desc.bRefresh);
	xhci_dbg(xhci, "urb->ep->desc->bSynchAddress 0x%x\n", urb->ep->desc.bSynchAddress);	
	xhci_dbg(xhci, "urb->setup_packet 0x%x\n", urb->setup_packet);
#endif
	ep_ring = xhci_urb_to_transfer_ring(xhci, urb);
	if (!ep_ring)
		return -EINVAL;

	/*
	 * Need to copy setup packet into setup TRB, so we can't use the setup
	 * DMA address.
	 */
	if (!urb->setup_packet)
		return -EINVAL;

	if (!in_interrupt())
		xhci_dbg(xhci, "Queueing ctrl tx for slot id %d, ep %d\n",
				slot_id, ep_index);
	/* 1 TRB for setup, 1 for status */
	num_trbs = 2;
	/*
	 * Don't need to check if we need additional event data and normal TRBs,
	 * since data in control transfers will never get bigger than 16MB
	 * XXX: can we get a buffer that crosses 64KB boundaries?
	 */
	if (urb->transfer_buffer_length > 0)
		num_trbs++;
	ret = prepare_transfer(xhci, xhci->devs[slot_id],
			ep_index, urb->stream_id,
			num_trbs, urb, 0, mem_flags);
	if (ret < 0)
		return ret;
	
	urb_priv = urb->hcpriv;
	td = urb_priv->td[0];
	
	/*
	 * Don't give the first TRB to the hardware (by toggling the cycle bit)
	 * until we've finished creating all the other TRBs.  The ring's cycle
	 * state may change as we enqueue the other TRBs, so save it too.
	 */
	start_trb = &ep_ring->enqueue->generic;
	start_cycle = ep_ring->cycle_state;
#if 0
	xhci_dbg(xhci, "start_trb 0x%x\n", &ep_ring->enqueue->generic);
	xhci_dbg(xhci, "start_cycle 0x%x\n", ep_ring->cycle_state);
#endif
	/* Queue setup TRB - see section 6.4.1.2.1 */
	/* FIXME better way to translate setup_packet into two u32 fields? */

	setup = (struct usb_ctrlrequest *) urb->setup_packet;
#if 0
	xhci_dbg(xhci, "setup->bRequestType 0x%x\n", setup->bRequestType);
	xhci_dbg(xhci, "setup->bRequest 0x%x\n", setup->bRequest);
	xhci_dbg(xhci, "setup->wValue 0x%x\n", setup->wValue);
	xhci_dbg(xhci, "setup->wIndex 0x%x\n", setup->wIndex);
	xhci_dbg(xhci, "setup->wLength 0x%x\n", setup->wLength);	
#endif
	if(num_trbs ==2){
		trt = TRB_TRT(TRT_NO_DATA);
	}else if(setup->bRequestType & USB_DIR_IN){
		trt = TRB_TRT(TRT_IN_DATA);
	}else{
		trt = TRB_TRT(TRT_OUT_DATA);
	}
	field = 0;
	field |= TRB_IDT | TRB_TYPE(TRB_SETUP) | trt;
	if (start_cycle == 0)
		field |= 0x1;
	queue_trb(xhci, ep_ring, false, true,
			/* FIXME endianness is probably going to bite my ass here. */
			setup->bRequestType | setup->bRequest << 8 | setup->wValue << 16,
			setup->wIndex | setup->wLength << 16,
			TRB_LEN(8) | TRB_INTR_TARGET(0),
			/* Immediate data in pointer */
			field);
	/* If there's data, queue data TRBs */
	field = 0;
//	remainder = xhci_td_remainder(urb->transfer_buffer_length, 0, max_packet, urb->transfer_buffer_length);
	length_field = TRB_LEN(urb->transfer_buffer_length) |
//		remainder |
		TRB_INTR_TARGET(0);
	if (urb->transfer_buffer_length > 0) {
		if (setup->bRequestType & USB_DIR_IN)
			field |= TRB_DIR_IN;
		queue_trb(xhci, ep_ring, false, true,
				lower_32_bits(urb->transfer_dma),
				upper_32_bits(urb->transfer_dma),
				length_field,
				/* Event on short tx */
				field | TRB_ISP | TRB_TYPE(TRB_DATA) | ep_ring->cycle_state);
	}
#if 1
	max_packet = urb->ep->desc.wMaxPacketSize;
	if((urb->transfer_flags & URB_ZERO_PACKET) 
		&& ((urb->transfer_buffer_length % max_packet) == 0)){
		if (setup->bRequestType & USB_DIR_IN)
				field |= TRB_DIR_IN;
		queue_trb(xhci, ep_ring, false, true,
				lower_32_bits(urb->transfer_dma),
				upper_32_bits(urb->transfer_dma),
				0,
				/* Event on short tx */
				field | TRB_ISP | TRB_TYPE(TRB_DATA) | ep_ring->cycle_state);
	}
#endif
	/* Save the DMA address of the last TRB in the TD */
	td->last_trb = ep_ring->enqueue;

	/* Queue status TRB - see Table 7 and sections 4.11.2.2 and 6.4.1.2.3 */
	/* If the device sent data, the status stage is an OUT transfer */
	if (urb->transfer_buffer_length > 0 && setup->bRequestType & USB_DIR_IN)
		field = 0;
	else
		field = TRB_DIR_IN;
	queue_trb(xhci, ep_ring, false, false,
			0,
			0,
			TRB_INTR_TARGET(0),
			/* Event on completion */
			field | TRB_IOC | TRB_TYPE(TRB_STATUS) | ep_ring->cycle_state);
#if 1
	giveback_first_trb(xhci, slot_id, ep_index, 0,
			start_cycle, start_trb, td);
#endif
	return 0;
}

/****		Command Ring Operations		****/

/* Generic function for queueing a command TRB on the command ring.
 * Check to make sure there's room on the command ring for one command TRB.
 * Also check that there's room reserved for commands that must not fail.
 * If this is a command that must not fail, meaning command_must_succeed = TRUE,
 * then only check for the number of reserved spots.
 * Don't decrement xhci->cmd_ring_reserved_trbs after we've queued the TRB
 * because the command event handler may want to resubmit a failed command.
 */
static int queue_command(struct xhci_hcd *xhci, u32 field1, u32 field2,
		u32 field3, u32 field4, bool command_must_succeed)
{
	int reserved_trbs = xhci->cmd_ring_reserved_trbs;
	int ret;

	if (!command_must_succeed)
		reserved_trbs++;

	ret = prepare_ring(xhci, xhci->cmd_ring, EP_STATE_RUNNING,
			reserved_trbs, GFP_ATOMIC);
	if (ret < 0) {
		xhci_err(xhci, "[ERROR] No room for command on command ring\n");
		if (command_must_succeed)
			xhci_err(xhci, "[ERROR] Reserved TRB counting for "
					"unfailable commands failed.\n");
		return ret;
	}
	queue_trb(xhci, xhci->cmd_ring, false, false, field1, field2, field3,
			field4 | xhci->cmd_ring->cycle_state);
	return 0;
}

/* Queue a no-op command on the command ring */
static int queue_cmd_noop(struct xhci_hcd *xhci)
{
	return queue_command(xhci, 0, 0, 0, TRB_TYPE(TRB_CMD_NOOP), false);
}

/*
 * Place a no-op command on the command ring to test the command and
 * event ring.
 */
void *xhci_setup_one_noop(struct xhci_hcd *xhci)
{
	if (queue_cmd_noop(xhci) < 0)
		return NULL;
	xhci->noops_submitted++;
	return xhci_ring_cmd_db;
}

/*
 * Place a no-op command on the command ring to test the command and
 * event ring.
 */
void *mtk_xhci_setup_one_noop(struct xhci_hcd *xhci)
{
	if (queue_cmd_noop(xhci) < 0)
		return NULL;
	xhci_ring_cmd_db(xhci);
	return NULL;
}


/* Queue a slot enable or disable request on the command ring */
int xhci_queue_slot_control(struct xhci_hcd *xhci, u32 trb_type, u32 slot_id)
{
	return queue_command(xhci, 0, 0, 0,
			TRB_TYPE(trb_type) | SLOT_ID_FOR_TRB(slot_id), false);
}

/* Queue an address device command TRB */
int xhci_queue_address_device(struct xhci_hcd *xhci, dma_addr_t in_ctx_ptr,
		u32 slot_id, char isBSR)
{

	if(isBSR){
		return queue_command(xhci, lower_32_bits(in_ctx_ptr),
			upper_32_bits(in_ctx_ptr), 0,
			TRB_TYPE(TRB_ADDR_DEV) | SLOT_ID_FOR_TRB(slot_id) | ADDRESS_TRB_BSR,
			false);
	}
	else{
		return queue_command(xhci, lower_32_bits(in_ctx_ptr),
				upper_32_bits(in_ctx_ptr), 0,
				TRB_TYPE(TRB_ADDR_DEV) | SLOT_ID_FOR_TRB(slot_id),
				false);
	}
	return 0;
}

int xhci_queue_vendor_command(struct xhci_hcd *xhci,
		u32 field1, u32 field2, u32 field3, u32 field4)
{
	return queue_command(xhci, field1, field2, field3, field4, false);
}

/* Queue a reset device command TRB */
int xhci_queue_reset_device(struct xhci_hcd *xhci, u32 slot_id)
{
	return queue_command(xhci, 0, 0, 0,
			TRB_TYPE(TRB_RESET_DEV) | SLOT_ID_FOR_TRB(slot_id),
			false);
}

/* Queue a configure endpoint command TRB */
int xhci_queue_configure_endpoint(struct xhci_hcd *xhci, dma_addr_t in_ctx_ptr,
		u32 slot_id, bool command_must_succeed)
{
	return queue_command(xhci, lower_32_bits(in_ctx_ptr),
			upper_32_bits(in_ctx_ptr), 0,
			TRB_TYPE(TRB_CONFIG_EP) | SLOT_ID_FOR_TRB(slot_id),
			command_must_succeed);
}

int xhci_queue_deconfigure_endpoint(struct xhci_hcd *xhci, dma_addr_t in_ctx_ptr,
		u32 slot_id, bool command_must_succeed){
	return queue_command(xhci, lower_32_bits(in_ctx_ptr),
			upper_32_bits(in_ctx_ptr), 0,
			TRB_TYPE(TRB_CONFIG_EP) | SLOT_ID_FOR_TRB(slot_id) | CONFIG_EP_TRB_DC,
			command_must_succeed);
}

/* Queue an evaluate context command TRB */
int xhci_queue_evaluate_context(struct xhci_hcd *xhci, dma_addr_t in_ctx_ptr,
		u32 slot_id)
{
	return queue_command(xhci, lower_32_bits(in_ctx_ptr),
			upper_32_bits(in_ctx_ptr), 0,
			TRB_TYPE(TRB_EVAL_CONTEXT) | SLOT_ID_FOR_TRB(slot_id),
			false);
}

int xhci_queue_stop_endpoint(struct xhci_hcd *xhci, int slot_id,
		unsigned int ep_index)
{
	u32 trb_slot_id = SLOT_ID_FOR_TRB(slot_id);
	u32 trb_ep_index = EP_ID_FOR_TRB(ep_index);
	u32 type = TRB_TYPE(TRB_STOP_RING);
	//xhci_err(xhci, "[DBG] queue stop ep command, address 0x%x\n", xhci->cmd_ring->enqueue);
	if(ep_index == 1){
		if(TRB_FIELD_TO_TYPE(xhci->cmd_ring->enqueue->generic.field[3]) == TRB_LINK){
			g_cmd_ring_pointer1 = (((int)xhci->cmd_ring->enqueue->link.segment_ptr) & 0xff0);
		}
		else{
			g_cmd_ring_pointer1 = ((((int)xhci->cmd_ring->enqueue) & 0xff0));
		}
	}
	else if(ep_index == 2){
		if(TRB_FIELD_TO_TYPE(xhci->cmd_ring->enqueue->generic.field[3]) == TRB_LINK){
			g_cmd_ring_pointer2 = (((int)xhci->cmd_ring->enqueue->link.segment_ptr) & 0xff0);
		}
		else{
			g_cmd_ring_pointer2 = ((((int)xhci->cmd_ring->enqueue) & 0xff0));
		}
	}
	return queue_command(xhci, 0, 0, 0,
			trb_slot_id | trb_ep_index | type, false);
}

/* Set Transfer Ring Dequeue Pointer command.
 * This should not be used for endpoints that have streams enabled.
 */
static int queue_set_tr_deq(struct xhci_hcd *xhci, int slot_id,
		unsigned int ep_index, unsigned int stream_id,
		struct xhci_segment *deq_seg,
		union xhci_trb *deq_ptr, u32 cycle_state)
{
	dma_addr_t addr;
	u32 trb_slot_id = SLOT_ID_FOR_TRB(slot_id);
	u32 trb_ep_index = EP_ID_FOR_TRB(ep_index);
	u32 trb_stream_id = STREAM_ID_FOR_TRB(stream_id);
	u32 type = TRB_TYPE(TRB_SET_DEQ);
	struct xhci_virt_ep *ep;

	addr = xhci_trb_virt_to_dma(deq_seg, deq_ptr);
	if (addr == 0) {
		xhci_warn(xhci, "WARN Cannot submit Set TR Deq Ptr\n");
		xhci_warn(xhci, "WARN deq seg = %p, deq pt = %p\n",
				deq_seg, deq_ptr);
		return 0;
	}
	ep = &xhci->devs[slot_id]->eps[ep_index];
	if ((ep->ep_state & SET_DEQ_PENDING)) {
		xhci_warn(xhci, "WARN Cannot submit Set TR Deq Ptr\n");
		xhci_warn(xhci, "A Set TR Deq Ptr command is pending.\n");
		return 0;
	}
	ep->queued_deq_seg = deq_seg;
	ep->queued_deq_ptr = deq_ptr;
	return queue_command(xhci, lower_32_bits(addr) | cycle_state,
			upper_32_bits(addr), trb_stream_id,
			trb_slot_id | trb_ep_index | type, false);
}

int xhci_queue_reset_ep(struct xhci_hcd *xhci, int slot_id,
		unsigned int ep_index)
{
	u32 trb_slot_id = SLOT_ID_FOR_TRB(slot_id);
	u32 trb_ep_index = EP_ID_FOR_TRB(ep_index);
	u32 type = TRB_TYPE(TRB_RESET_EP);

	return queue_command(xhci, 0, 0, 0, trb_slot_id | trb_ep_index | type,
			false);
}
