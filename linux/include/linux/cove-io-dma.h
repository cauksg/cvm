/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_COVE_IO_DMA_H
#define _LINUX_COVE_IO_DMA_H
#include <linux/kconfig.h>
struct pci_dev;
#if IS_ENABLED(CONFIG_COVE_IO_NIC)
int cove_io_nic_enable(struct pci_dev *pdev);
#else
static inline int cove_io_nic_enable(struct pci_dev *pdev) { return 0; }
#endif
#endif
