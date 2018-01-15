/*
 * Copyright (c) 2011 Jan Vesely
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup drvusbuhcihc
 * @{
 */
/** @file
 * @brief UHCI Host controller driver routines
 */

#include <adt/list.h>
#include <assert.h>
#include <async.h>
#include <ddi.h>
#include <device/hw_res_parsed.h>
#include <fibril.h>
#include <errno.h>
#include <macros.h>
#include <mem.h>
#include <stdlib.h>
#include <stdint.h>
#include <str_error.h>

#include <usb/debug.h>
#include <usb/usb.h>
#include <usb/host/utils/malloc32.h>
#include <usb/host/bandwidth.h>

#include "uhci_batch.h"
#include "transfer_list.h"
#include "hc.h"

#define UHCI_INTR_ALLOW_INTERRUPTS \
    (UHCI_INTR_CRC | UHCI_INTR_COMPLETE | UHCI_INTR_SHORT_PACKET)
#define UHCI_STATUS_USED_INTERRUPTS \
    (UHCI_STATUS_INTERRUPT | UHCI_STATUS_ERROR_INTERRUPT)

static const irq_pio_range_t uhci_irq_pio_ranges[] = {
	{
		.base = 0,
		.size = sizeof(uhci_regs_t)
	}
};

static const irq_cmd_t uhci_irq_commands[] = {
	{
		.cmd = CMD_PIO_READ_16,
		.dstarg = 1,
		.addr = NULL
	},
	{
		.cmd = CMD_AND,
		.srcarg = 1,
		.dstarg = 2,
		.value = UHCI_STATUS_USED_INTERRUPTS | UHCI_STATUS_NM_INTERRUPTS
	},
	{
		.cmd = CMD_PREDICATE,
		.srcarg = 2,
		.value = 2
	},
	{
		.cmd = CMD_PIO_WRITE_A_16,
		.srcarg = 1,
		.addr = NULL
	},
	{
		.cmd = CMD_ACCEPT
	}
};

static void hc_init_hw(const hc_t *instance);
static int hc_init_mem_structures(hc_t *instance, hc_device_t *);
static int hc_init_transfer_lists(hc_t *instance);

static int hc_debug_checker(void *arg);


/** Generate IRQ code.
 * @param[out] code IRQ code structure.
 * @param[in] hw_res Device's resources.
 *
 * @return Error code.
 */
int hc_gen_irq_code(irq_code_t *code, hc_device_t *hcd, const hw_res_list_parsed_t *hw_res)
{
	assert(code);
	assert(hw_res);

	if (hw_res->irqs.count != 1 || hw_res->io_ranges.count != 1)
		return EINVAL;
	const addr_range_t regs = hw_res->io_ranges.ranges[0];

	if (RNGSZ(regs) < sizeof(uhci_regs_t))
		return EOVERFLOW;

	code->ranges = malloc(sizeof(uhci_irq_pio_ranges));
	if (code->ranges == NULL)
		return ENOMEM;

	code->cmds = malloc(sizeof(uhci_irq_commands));
	if (code->cmds == NULL) {
		free(code->ranges);
		return ENOMEM;
	}

	code->rangecount = ARRAY_SIZE(uhci_irq_pio_ranges);
	code->cmdcount = ARRAY_SIZE(uhci_irq_commands);

	memcpy(code->ranges, uhci_irq_pio_ranges, sizeof(uhci_irq_pio_ranges));
	code->ranges[0].base = RNGABS(regs);

	memcpy(code->cmds, uhci_irq_commands, sizeof(uhci_irq_commands));
	uhci_regs_t *registers = (uhci_regs_t *) RNGABSPTR(regs);
	code->cmds[0].addr = (void*)&registers->usbsts;
	code->cmds[3].addr = (void*)&registers->usbsts;

	usb_log_debug("I/O regs at %p (size %zu), IRQ %d.",
	    RNGABSPTR(regs), RNGSZ(regs), hw_res->irqs.irqs[0]);

	return hw_res->irqs.irqs[0];
}

/** Take action based on the interrupt cause.
 *
 * @param[in] hcd HCD structure to use.
 * @param[in] status Value of the status register at the time of interrupt.
 *
 * Interrupt might indicate:
 * - transaction completed, either by triggering IOC, SPD, or an error
 * - some kind of device error
 * - resume from suspend state (not implemented)
 */
static void hc_interrupt(bus_t *bus, uint32_t status)
{
	hc_t *instance = bus_to_hc(bus);

	/* Lower 2 bits are transaction error and transaction complete */
	if (status & (UHCI_STATUS_INTERRUPT | UHCI_STATUS_ERROR_INTERRUPT)) {
		LIST_INITIALIZE(done);
		transfer_list_remove_finished(
		    &instance->transfers_interrupt, &done);
		transfer_list_remove_finished(
		    &instance->transfers_control_slow, &done);
		transfer_list_remove_finished(
		    &instance->transfers_control_full, &done);
		transfer_list_remove_finished(
		    &instance->transfers_bulk_full, &done);

		list_foreach_safe(done, current, next) {
			list_remove(current);
			uhci_transfer_batch_t *batch =
			    uhci_transfer_batch_from_link(current);
			usb_transfer_batch_finish(&batch->base);
		}
	}
	/* Resume interrupts are not supported */
	if (status & UHCI_STATUS_RESUME) {
		usb_log_error("Resume interrupt!");
	}

	/* Bits 4 and 5 indicate hc error */
	if (status & (UHCI_STATUS_PROCESS_ERROR | UHCI_STATUS_SYSTEM_ERROR)) {
		usb_log_error("UHCI hardware failure!.");
		++instance->hw_failures;
		transfer_list_abort_all(&instance->transfers_interrupt);
		transfer_list_abort_all(&instance->transfers_control_slow);
		transfer_list_abort_all(&instance->transfers_control_full);
		transfer_list_abort_all(&instance->transfers_bulk_full);

		if (instance->hw_failures < UHCI_ALLOWED_HW_FAIL) {
			/* reinitialize hw, this triggers virtual disconnect*/
			hc_init_hw(instance);
		} else {
			usb_log_fatal("Too many UHCI hardware failures!.");
			hc_gone(&instance->base);
		}
	}
}

/** Initialize UHCI hc driver structure
 *
 * @param[in] instance Memory place to initialize.
 * @param[in] regs Range of device's I/O control registers.
 * @param[in] interrupts True if hw interrupts should be used.
 * @return Error code.
 * @note Should be called only once on any structure.
 *
 * Initializes memory structures, starts up hw, and launches debugger and
 * interrupt fibrils.
 */
int hc_add(hc_device_t *hcd, const hw_res_list_parsed_t *hw_res)
{
	hc_t *instance = hcd_to_hc(hcd);
	assert(hw_res);
	if (hw_res->io_ranges.count != 1 ||
	    hw_res->io_ranges.ranges[0].size < sizeof(uhci_regs_t))
	    return EINVAL;

	instance->hw_failures = 0;

	/* allow access to hc control registers */
	int ret = pio_enable_range(&hw_res->io_ranges.ranges[0],
	    (void **) &instance->registers);
	if (ret != EOK) {
		usb_log_error("Failed to gain access to registers: %s.",
	            str_error(ret));
		return ret;
	}

	usb_log_debug("Device registers at %" PRIx64 " (%zuB) accessible.",
	    hw_res->io_ranges.ranges[0].address.absolute,
	    hw_res->io_ranges.ranges[0].size);

	ret = hc_init_mem_structures(instance, hcd);
	if (ret != EOK) {
		usb_log_error("Failed to init UHCI memory structures: %s.",
		    str_error(ret));
		// TODO: we should disable pio here
		return ret;
	}

	return EOK;
}

int hc_start(hc_device_t *hcd)
{
	hc_t *instance = hcd_to_hc(hcd);
	hc_init_hw(instance);
	(void)hc_debug_checker;

	return uhci_rh_init(&instance->rh, instance->registers->ports, "uhci");
}

/** Safely dispose host controller internal structures
 *
 * @param[in] instance Host controller structure to use.
 */
int hc_gone(hc_device_t *instance)
{
	assert(instance);
	//TODO Implement
	return ENOTSUP;
}

/** Initialize UHCI hc hw resources.
 *
 * @param[in] instance UHCI structure to use.
 * For magic values see UHCI Design Guide
 */
void hc_init_hw(const hc_t *instance)
{
	assert(instance);
	uhci_regs_t *registers = instance->registers;

	/* Reset everything, who knows what touched it before us */
	pio_write_16(&registers->usbcmd, UHCI_CMD_GLOBAL_RESET);
	async_usleep(50000); /* 50ms according to USB spec(root hub reset) */
	pio_write_16(&registers->usbcmd, 0);

	/* Reset hc, all states and counters. Hope that hw is not broken */
	pio_write_16(&registers->usbcmd, UHCI_CMD_HCRESET);
	do { async_usleep(10); }
	while ((pio_read_16(&registers->usbcmd) & UHCI_CMD_HCRESET) != 0);

	/* Set frame to exactly 1ms */
	pio_write_8(&registers->sofmod, 64);

	/* Set frame list pointer */
	const uint32_t pa = addr_to_phys(instance->frame_list);
	pio_write_32(&registers->flbaseadd, pa);

	if (instance->base.irq_cap >= 0) {
		/* Enable all interrupts, but resume interrupt */
		pio_write_16(&instance->registers->usbintr,
		    UHCI_INTR_ALLOW_INTERRUPTS);
	}

	const uint16_t cmd = pio_read_16(&registers->usbcmd);
	if (cmd != 0)
		usb_log_warning("Previous command value: %x.", cmd);

	/* Start the hc with large(64B) packet FSBR */
	pio_write_16(&registers->usbcmd,
	    UHCI_CMD_RUN_STOP | UHCI_CMD_MAX_PACKET | UHCI_CMD_CONFIGURE);
}

static usb_transfer_batch_t *create_transfer_batch(endpoint_t *ep)
{
	uhci_transfer_batch_t *batch = uhci_transfer_batch_create(ep);
	return &batch->base;
}

static void destroy_transfer_batch(usb_transfer_batch_t *batch)
{
	uhci_transfer_batch_destroy(uhci_transfer_batch_get(batch));
}

static void endpoint_unregister(endpoint_t *ep)
{
	hc_t * const hc = bus_to_hc(endpoint_get_bus(ep));
	usb2_bus_ops.endpoint_unregister(ep);

	uhci_transfer_batch_t *batch = NULL;

	fibril_mutex_lock(&ep->guard);
	if (ep->active_batch) {
		batch = uhci_transfer_batch_get(ep->active_batch);

		transfer_list_t *list = hc->transfers[ep->device->speed][ep->transfer_type];
		assert(list);

		fibril_mutex_lock(&list->guard);
		transfer_list_remove_batch(list, batch);
		fibril_mutex_unlock(&list->guard);

		endpoint_wait_timeout_locked(ep, 2000);

		batch = uhci_transfer_batch_get(ep->active_batch);
		if (ep->active_batch) {
			endpoint_deactivate_locked(ep);
		}
	}
	fibril_mutex_unlock(&ep->guard);

	if (batch) {
		batch->base.error = EINTR;
		batch->base.transfered_size = 0;
		usb_transfer_batch_finish(&batch->base);
	}
}

static int hc_status(bus_t *, uint32_t *);
static int hc_schedule(usb_transfer_batch_t *);

static const bus_ops_t uhci_bus_ops = {
	.parent = &usb2_bus_ops,

	.interrupt = hc_interrupt,
	.status = hc_status,

	.endpoint_unregister = endpoint_unregister,
	.endpoint_count_bw = bandwidth_count_usb11,

	.batch_create = create_transfer_batch,
	.batch_schedule = hc_schedule,
	.batch_destroy = destroy_transfer_batch,
};

/** Initialize UHCI hc memory structures.
 *
 * @param[in] instance UHCI structure to use.
 * @return Error code
 * @note Should be called only once on any structure.
 *
 * Structures:
 *  - transfer lists (queue heads need to be accessible by the hw)
 *  - frame list page (needs to be one UHCI hw accessible 4K page)
 */
int hc_init_mem_structures(hc_t *instance, hc_device_t *hcd)
{
	assert(instance);

	usb2_bus_init(&instance->bus, BANDWIDTH_AVAILABLE_USB11);

	bus_t *bus = (bus_t *) &instance->bus;
	bus->ops = &uhci_bus_ops;

	hc_device_setup(&instance->base, bus);

	/* Init USB frame list page */
	instance->frame_list = get_page();
	if (!instance->frame_list) {
		return ENOMEM;
	}
	usb_log_debug("Initialized frame list at %p.", instance->frame_list);

	/* Init transfer lists */
	int ret = hc_init_transfer_lists(instance);
	if (ret != EOK) {
		usb_log_error("Failed to initialize transfer lists.");
		return_page(instance->frame_list);
		return ENOMEM;
	}
	usb_log_debug("Initialized transfer lists.");


	/* Set all frames to point to the first queue head */
	const uint32_t queue = LINK_POINTER_QH(
	        addr_to_phys(instance->transfers_interrupt.queue_head));

	for (unsigned i = 0; i < UHCI_FRAME_LIST_COUNT; ++i) {
		instance->frame_list[i] = queue;
	}

	return EOK;
}

/** Initialize UHCI hc transfer lists.
 *
 * @param[in] instance UHCI structure to use.
 * @return Error code
 * @note Should be called only once on any structure.
 *
 * Initializes transfer lists and sets them in one chain to support proper
 * USB scheduling. Sets pointer table for quick access.
 */
int hc_init_transfer_lists(hc_t *instance)
{
	assert(instance);
#define SETUP_TRANSFER_LIST(type, name) \
do { \
	int ret = transfer_list_init(&instance->transfers_##type, name); \
	if (ret != EOK) { \
		usb_log_error("Failed to setup %s transfer list: %s.", \
		    name, str_error(ret)); \
		transfer_list_fini(&instance->transfers_bulk_full); \
		transfer_list_fini(&instance->transfers_control_full); \
		transfer_list_fini(&instance->transfers_control_slow); \
		transfer_list_fini(&instance->transfers_interrupt); \
		return ret; \
	} \
} while (0)

	SETUP_TRANSFER_LIST(bulk_full, "BULK FULL");
	SETUP_TRANSFER_LIST(control_full, "CONTROL FULL");
	SETUP_TRANSFER_LIST(control_slow, "CONTROL LOW");
	SETUP_TRANSFER_LIST(interrupt, "INTERRUPT");
#undef SETUP_TRANSFER_LIST
	/* Connect lists into one schedule */
	transfer_list_set_next(&instance->transfers_control_full,
		&instance->transfers_bulk_full);
	transfer_list_set_next(&instance->transfers_control_slow,
		&instance->transfers_control_full);
	transfer_list_set_next(&instance->transfers_interrupt,
		&instance->transfers_control_slow);

	/*FSBR, This feature is not needed (adds no benefit) and is supposedly
	 * buggy on certain hw, enable at your own risk. */
#ifdef FSBR
	transfer_list_set_next(&instance->transfers_bulk_full,
	    &instance->transfers_control_full);
#endif

	/* Assign pointers to be used during scheduling */
	instance->transfers[USB_SPEED_FULL][USB_TRANSFER_INTERRUPT] =
	  &instance->transfers_interrupt;
	instance->transfers[USB_SPEED_LOW][USB_TRANSFER_INTERRUPT] =
	  &instance->transfers_interrupt;
	instance->transfers[USB_SPEED_FULL][USB_TRANSFER_CONTROL] =
	  &instance->transfers_control_full;
	instance->transfers[USB_SPEED_LOW][USB_TRANSFER_CONTROL] =
	  &instance->transfers_control_slow;
	instance->transfers[USB_SPEED_FULL][USB_TRANSFER_BULK] =
	  &instance->transfers_bulk_full;

	return EOK;
}

static int hc_status(bus_t *bus, uint32_t *status)
{
	hc_t *instance = bus_to_hc(bus);
	assert(status);

	*status = 0;
	if (instance->registers) {
		uint16_t s = pio_read_16(&instance->registers->usbsts);
		pio_write_16(&instance->registers->usbsts, s);
		*status = s;
	}
	return EOK;
}

/** Schedule batch for execution.
 *
 * @param[in] instance UHCI structure to use.
 * @param[in] batch Transfer batch to schedule.
 * @return Error code
 *
 * Checks for bandwidth availability and appends the batch to the proper queue.
 */
static int hc_schedule(usb_transfer_batch_t *batch)
{
	uhci_transfer_batch_t *uhci_batch = uhci_transfer_batch_get(batch);
	endpoint_t *ep = batch->ep;
	hc_t *hc = bus_to_hc(endpoint_get_bus(ep));

	if (batch->target.address == uhci_rh_get_address(&hc->rh))
		return uhci_rh_schedule(&hc->rh, batch);


	const int err = uhci_transfer_batch_prepare(uhci_batch);
	if (err)
		return err;

	transfer_list_t *list = hc->transfers[ep->device->speed][ep->transfer_type];
	assert(list);
	transfer_list_add_batch(list, uhci_batch);

	return EOK;
}

int hc_unschedule_batch(usb_transfer_batch_t *batch)
{

	return EOK;
}

/** Debug function, checks consistency of memory structures.
 *
 * @param[in] arg UHCI structure to use.
 * @return EOK (should never return)
 */
int hc_debug_checker(void *arg)
{
	hc_t *instance = arg;
	assert(instance);

#define QH(queue) \
	instance->transfers_##queue.queue_head

	while (1) {
		const uint16_t cmd = pio_read_16(&instance->registers->usbcmd);
		const uint16_t sts = pio_read_16(&instance->registers->usbsts);
		const uint16_t intr =
		    pio_read_16(&instance->registers->usbintr);

		if (((cmd & UHCI_CMD_RUN_STOP) != 1) || (sts != 0)) {
			usb_log_debug2("Command: %X Status: %X Intr: %x",
			    cmd, sts, intr);
		}

		const uintptr_t frame_list =
		    pio_read_32(&instance->registers->flbaseadd) & ~0xfff;
		if (frame_list != addr_to_phys(instance->frame_list)) {
			usb_log_debug("Framelist address: %p vs. %p.",
			    (void *) frame_list,
			    (void *) addr_to_phys(instance->frame_list));
		}

		int frnum = pio_read_16(&instance->registers->frnum) & 0x3ff;

		uintptr_t expected_pa = instance->frame_list[frnum]
		    & LINK_POINTER_ADDRESS_MASK;
		uintptr_t real_pa = addr_to_phys(QH(interrupt));
		if (expected_pa != real_pa) {
			usb_log_debug("Interrupt QH: %p (frame %d) vs. %p.",
			    (void *) expected_pa, frnum, (void *) real_pa);
		}

		expected_pa = QH(interrupt)->next & LINK_POINTER_ADDRESS_MASK;
		real_pa = addr_to_phys(QH(control_slow));
		if (expected_pa != real_pa) {
			usb_log_debug("Control Slow QH: %p vs. %p.",
			    (void *) expected_pa, (void *) real_pa);
		}

		expected_pa = QH(control_slow)->next & LINK_POINTER_ADDRESS_MASK;
		real_pa = addr_to_phys(QH(control_full));
		if (expected_pa != real_pa) {
			usb_log_debug("Control Full QH: %p vs. %p.",
			    (void *) expected_pa, (void *) real_pa);
		}

		expected_pa = QH(control_full)->next & LINK_POINTER_ADDRESS_MASK;
		real_pa = addr_to_phys(QH(bulk_full));
		if (expected_pa != real_pa ) {
			usb_log_debug("Bulk QH: %p vs. %p.",
			    (void *) expected_pa, (void *) real_pa);
		}
		async_usleep(UHCI_DEBUGER_TIMEOUT);
	}
	return EOK;
#undef QH
}
/**
 * @}
 */
