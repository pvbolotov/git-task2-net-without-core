/*
 * nop (passthrough) Link Layer Control
 *
 * Copyright (C) 2012  Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <linux/types.h>
#include <linux/export.h>

#include "llc.h"

struct llc_nop {
	struct nfc_hci_dev *hdev;
	xmit_to_drv_t xmit_to_drv;
	rcv_to_hci_t rcv_to_hci;
	int tx_headroom;
	int tx_tailroom;
	llc_failure_t llc_failure;
};

static void *llc_nop_init(struct nfc_hci_dev *hdev, xmit_to_drv_t xmit_to_drv,
			  rcv_to_hci_t rcv_to_hci, int tx_headroom,
			  int tx_tailroom, int *rx_headroom, int *rx_tailroom,
			  llc_failure_t llc_failure)
{
	struct llc_nop *llc_nop;

	*rx_headroom = 0;
	*rx_tailroom = 0;

	llc_nop = kzalloc(sizeof(struct llc_nop), GFP_KERNEL);
	if (llc_nop == NULL)
		return NULL;

	llc_nop->hdev = hdev;
	llc_nop->xmit_to_drv = xmit_to_drv;
	llc_nop->rcv_to_hci = rcv_to_hci;
	llc_nop->tx_headroom = tx_headroom;
	llc_nop->tx_tailroom = tx_tailroom;
	llc_nop->llc_failure = llc_failure;

	return llc_nop;
}

static void llc_nop_deinit(struct nfc_llc *llc)
{
	kfree(nfc_llc_get_data(llc));
}

static int llc_nop_start(struct nfc_llc *llc)
{
	return 0;
}

static int llc_nop_stop(struct nfc_llc *llc)
{
	return 0;
}

static void llc_nop_rcv_from_drv(struct nfc_llc *llc, struct sk_buff *skb)
{
	struct llc_nop *llc_nop = nfc_llc_get_data(llc);

	llc_nop->rcv_to_hci(llc_nop->hdev, skb);
}

static int llc_nop_xmit_from_hci(struct nfc_llc *llc, struct sk_buff *skb)
{
	struct llc_nop *llc_nop = nfc_llc_get_data(llc);

	return llc_nop->xmit_to_drv(llc_nop->hdev, skb);
}

static struct nfc_llc_ops llc_nop_ops = {
	.init = llc_nop_init,
	.deinit = llc_nop_deinit,
	.start = llc_nop_start,
	.stop = llc_nop_stop,
	.rcv_from_drv = llc_nop_rcv_from_drv,
	.xmit_from_hci = llc_nop_xmit_from_hci,
};

int nfc_llc_nop_register()
{
	return nfc_llc_register(LLC_NOP_NAME, &llc_nop_ops);
}
EXPORT_SYMBOL(nfc_llc_nop_register);
