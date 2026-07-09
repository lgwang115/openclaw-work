/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Forward declarations for BST doorbell APIs.
 * Prefer including the real header from the kernel tree via Makefile -I.
 */
#ifndef _BST_DOORBELL_H_
#define _BST_DOORBELL_H_

struct pci_epc;

int bst_pcie_ep_db_irq_alloc(struct pci_epc *epc, u8 func_no, u8 vfunc_no);
int bst_pcie_ep_db_irq_request(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			       u32 irq_no, int (*handler)(int irq, void *arg),
			       void *arg);
void bst_pcie_ep_db_irq_free(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			     u32 irq_no);
int bst_pcie_ep_db_info_get(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			    u32 irq_no, u32 *bar_no, u32 *offset, u32 *msg);

#endif
