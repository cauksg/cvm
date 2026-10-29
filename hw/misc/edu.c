/*
 * QEMU educational PCI device
 *
 * Copyright (c) 2012-2015 Jiri Slaby
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
#include "hw/pci/pcie.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "qemu/main-loop.h" /* iothread mutex */
#include "qemu/module.h"
#include "qapi/visitor.h"

#define TYPE_PCI_EDU_DEVICE "edu"
typedef struct EduState EduState;
DECLARE_INSTANCE_CHECKER(EduState, EDU,
                         TYPE_PCI_EDU_DEVICE)

#define FACT_IRQ        0x00000001
#define DMA_IRQ         0x00000100

#define DMA_START       0x40000
#define DMA_SIZE        4096

#define EDU_PRI_CAP_EXP_OFFSET  0x80
#define EDU_PRI_CAP_ATS_OFFSET  0x100
#define EDU_PRI_CAP_PASID_OFFSET 0x108
#define EDU_PRI_CAP_PRI_OFFSET  0x110

#define EDU_PRI_CTRL_ENABLE     0x1
#define EDU_PRI_CTRL_REQUEST    0x2
#define EDU_PRI_CTRL_READ       0x4
#define EDU_PRI_CTRL_WRITE      0x8
#define EDU_PRI_CTRL_PRIV       0x10
#define EDU_PRI_CTRL_LAST       0x20
#define EDU_PRI_CTRL_BURST      0x40
#define EDU_PRI_CTRL_CANCEL     0x80
#define EDU_PRI_CTRL_RESET_STATS 0x100
#define EDU_PRI_CTRL_STOP       0x200

#define EDU_PRI_STATUS_IDLE     0
#define EDU_PRI_STATUS_PENDING  1
#define EDU_PRI_STATUS_SUCCESS  2
#define EDU_PRI_STATUS_INVALID  3
#define EDU_PRI_STATUS_FAILURE  4
#define EDU_PRI_STATUS_ERROR    5
#define EDU_PRI_STATUS_CANCELLED 6
#define EDU_PRI_STATUS_STOPPED  7

#define EDU_PRI_PRGI_COUNT      512
#define EDU_PRI_PRGI_WORDS      (EDU_PRI_PRGI_COUNT / 64)
#define EDU_PRI_BURST_MAX       4096

struct EduState {
    PCIDevice pdev;
    MemoryRegion mmio;

    QemuThread thread;
    QemuMutex thr_mutex;
    QemuCond thr_cond;
    bool stopping;
    uint64_t reset_generation;

    uint32_t addr4;
    uint32_t fact;
#define EDU_STATUS_COMPUTING    0x01
#define EDU_STATUS_IRQFACT      0x80
    uint32_t status;

    uint32_t irq_status;

#define EDU_DMA_RUN             0x1
#define EDU_DMA_DIR(cmd)        (((cmd) & 0x2) >> 1)
# define EDU_DMA_FROM_PCI       0
# define EDU_DMA_TO_PCI         1
#define EDU_DMA_IRQ             0x4
    struct dma_state {
        dma_addr_t src;
        dma_addr_t dst;
        dma_addr_t cnt;
        dma_addr_t cmd;
    } dma;
    QEMUTimer dma_timer;
    char dma_buf[DMA_SIZE];
    uint64_t dma_mask;

    struct {
        uint64_t iova;
        uint32_t pasid;
        uint32_t ctrl;
        uint32_t status;
        uint32_t prgi;
        int32_t error;
        IOMMUPRINotifier notifier;
        bool notifier_registered;
        uint32_t registered_pasid;
        uint64_t pending[EDU_PRI_PRGI_WORDS];
        uint32_t outstanding;
        uint32_t requested;
        uint32_t completed;
        uint32_t success;
        uint32_t invalid;
        uint32_t failure;
        uint32_t error_count;
        uint32_t cancelled;
        uint32_t dropped;
        uint32_t last_prgi;
        uint32_t last_response;
        uint32_t burst_count;
        uint64_t stride;
    } pri;
};

static bool edu_msi_enabled(EduState *edu)
{
    return msi_enabled(&edu->pdev);
}

static void edu_raise_irq(EduState *edu, uint32_t val)
{
    edu->irq_status |= val;
    if (edu->irq_status) {
        if (edu_msi_enabled(edu)) {
            msi_notify(&edu->pdev, 0);
        } else {
            pci_set_irq(&edu->pdev, 1);
        }
    }
}

static void edu_lower_irq(EduState *edu, uint32_t val)
{
    edu->irq_status &= ~val;

    if (!edu->irq_status && !edu_msi_enabled(edu)) {
        pci_set_irq(&edu->pdev, 0);
    }
}

static void edu_check_range(uint64_t xfer_start, uint64_t xfer_size,
                uint64_t dma_start, uint64_t dma_size)
{
    uint64_t xfer_end = xfer_start + xfer_size;
    uint64_t dma_end = dma_start + dma_size;

    /*
     * 1. ensure we aren't overflowing
     * 2. ensure that xfer is within dma address range
     */
    if (dma_end >= dma_start && xfer_end >= xfer_start &&
        xfer_start >= dma_start && xfer_end <= dma_end) {
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "EDU: DMA range 0x%016"PRIx64"-0x%016"PRIx64
                  " out of bounds (0x%016"PRIx64"-0x%016"PRIx64")!",
                  xfer_start, xfer_end - 1, dma_start, dma_end - 1);
}

static dma_addr_t edu_clamp_addr(const EduState *edu, dma_addr_t addr)
{
    dma_addr_t res = addr & edu->dma_mask;

    if (addr != res) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "EDU: clamping DMA 0x%016"PRIx64" to 0x%016"PRIx64"!",
                      addr, res);
    }

    return res;
}

static void edu_dma_timer(void *opaque)
{
    EduState *edu = opaque;
    bool raise_irq = false;

    if (!(edu->dma.cmd & EDU_DMA_RUN)) {
        return;
    }

    if (EDU_DMA_DIR(edu->dma.cmd) == EDU_DMA_FROM_PCI) {
        uint64_t dst = edu->dma.dst;
        edu_check_range(dst, edu->dma.cnt, DMA_START, DMA_SIZE);
        dst -= DMA_START;
        pci_dma_read(&edu->pdev, edu_clamp_addr(edu, edu->dma.src),
                edu->dma_buf + dst, edu->dma.cnt);
    } else {
        uint64_t src = edu->dma.src;
        edu_check_range(src, edu->dma.cnt, DMA_START, DMA_SIZE);
        src -= DMA_START;
        pci_dma_write(&edu->pdev, edu_clamp_addr(edu, edu->dma.dst),
                edu->dma_buf + src, edu->dma.cnt);
    }

    edu->dma.cmd &= ~EDU_DMA_RUN;
    if (edu->dma.cmd & EDU_DMA_IRQ) {
        raise_irq = true;
    }

    if (raise_irq) {
        edu_raise_irq(edu, DMA_IRQ);
    }
}

static void edu_pri_response(IOMMUPRINotifier *notifier,
                             IOMMUPRIResponse *response)
{
    EduState *edu = container_of(notifier, EduState, pri.notifier);
    uint32_t prgi = response->prgi & PCI_PRI_PRGI_MASK;
    uint64_t bit = 1ULL << (prgi & 63);
    uint32_t word = prgi / 64;

    edu->pri.last_prgi = prgi;
    edu->pri.last_response = response->response_code;

    if (!(edu->pri.pending[word] & bit)) {
        edu->pri.dropped++;
        edu->pri.status = EDU_PRI_STATUS_ERROR;
        edu->pri.error = -EINVAL;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "EDU: unexpected PRI response prgi=%u outstanding=%u\n",
                      prgi, edu->pri.outstanding);
        return;
    }

    edu->pri.pending[word] &= ~bit;
    edu->pri.outstanding--;
    edu->pri.completed++;

    switch (response->response_code) {
    case IOMMU_PRI_RESP_SUCCESS:
        edu->pri.success++;
        edu->pri.status = EDU_PRI_STATUS_SUCCESS;
        break;
    case IOMMU_PRI_RESP_INVALID_REQUEST:
        edu->pri.invalid++;
        edu->pri.status = EDU_PRI_STATUS_INVALID;
        break;
    case IOMMU_PRI_RESP_FAILURE:
    default:
        edu->pri.failure++;
        edu->pri.status = EDU_PRI_STATUS_FAILURE;
        break;
    }

    edu->pri.error = 0;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "EDU: PRI response prgi=%u code=%u status=%u iova=0x%"PRIx64"\n",
                  response->prgi, response->response_code, edu->pri.status,
                  edu->pri.iova);
}

static void edu_pri_unregister(EduState *edu)
{
    if (!edu->pri.notifier_registered) {
        return;
    }

    pci_pri_unregister_notifier(&edu->pdev, edu->pri.registered_pasid);
    edu->pri.notifier_registered = false;
}

static int edu_pri_register(EduState *edu)
{
    int ret;

    if (edu->pri.notifier_registered) {
        if (edu->pri.registered_pasid == edu->pri.pasid) {
            return 0;
        }
        edu_pri_unregister(edu);
    }

    edu->pri.notifier.notify = edu_pri_response;
    ret = pci_pri_register_notifier(&edu->pdev, edu->pri.pasid,
                                    &edu->pri.notifier);
    if (ret) {
        edu->pri.error = ret;
        edu->pri.status = EDU_PRI_STATUS_ERROR;
        return ret;
    }

    edu->pri.notifier_registered = true;
    edu->pri.registered_pasid = edu->pri.pasid;
    return 0;
}

static void edu_pri_reset_stats(EduState *edu)
{
    memset(edu->pri.pending, 0, sizeof(edu->pri.pending));
    edu->pri.outstanding = 0;
    edu->pri.requested = 0;
    edu->pri.completed = 0;
    edu->pri.success = 0;
    edu->pri.invalid = 0;
    edu->pri.failure = 0;
    edu->pri.error_count = 0;
    edu->pri.cancelled = 0;
    edu->pri.dropped = 0;
    edu->pri.last_prgi = 0;
    edu->pri.last_response = 0;
    edu->pri.error = 0;
    edu->pri.status = EDU_PRI_STATUS_IDLE;
}

static int edu_pri_request_one(EduState *edu, uint64_t iova, uint32_t prgi,
                               bool is_read, bool is_write, bool priv_req,
                               bool lpig)
{
    uint64_t bit = 1ULL << (prgi & 63);
    uint32_t word = prgi / 64;
    int ret;

    if (edu->pri.pending[word] & bit) {
        edu->pri.error = -EBUSY;
        edu->pri.error_count++;
        edu->pri.status = EDU_PRI_STATUS_ERROR;
        return -EBUSY;
    }

    ret = pci_pri_request_page(&edu->pdev, edu->pri.pasid, priv_req, false,
                               iova, lpig, prgi, is_read, is_write);
    if (ret) {
        edu->pri.error = ret;
        edu->pri.error_count++;
        edu->pri.status = EDU_PRI_STATUS_ERROR;
    } else {
        edu->pri.pending[word] |= bit;
        edu->pri.outstanding++;
        edu->pri.requested++;
        edu->pri.status = EDU_PRI_STATUS_PENDING;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "EDU: PRI request iova=0x%"PRIx64" pasid=%u prgi=%u flags=0x%x ret=%d outstanding=%u\n",
                  iova, edu->pri.pasid, prgi, edu->pri.ctrl, ret,
                  edu->pri.outstanding);

    return ret;
}

static void edu_pri_cancel(EduState *edu)
{
    edu_pri_unregister(edu);
    edu->pri.cancelled += edu->pri.outstanding;
    memset(edu->pri.pending, 0, sizeof(edu->pri.pending));
    edu->pri.outstanding = 0;
    edu->pri.status = EDU_PRI_STATUS_CANCELLED;
    edu->pri.error = 0;
}

static void edu_pri_request(EduState *edu)
{
    bool is_read = edu->pri.ctrl & EDU_PRI_CTRL_READ;
    bool is_write = edu->pri.ctrl & EDU_PRI_CTRL_WRITE;
    bool priv_req = edu->pri.ctrl & EDU_PRI_CTRL_PRIV;
    bool lpig = edu->pri.ctrl & EDU_PRI_CTRL_LAST;
    uint32_t count = 1;
    uint32_t i;
    int ret = 0;

    if (!(edu->pri.ctrl & EDU_PRI_CTRL_ENABLE)) {
        edu_pri_unregister(edu);
        edu->pri.status = EDU_PRI_STATUS_IDLE;
        edu->pri.error = 0;
        return;
    }

    if (edu->pri.ctrl & EDU_PRI_CTRL_STOP) {
        edu_pri_unregister(edu);
        edu->pri.status = EDU_PRI_STATUS_STOPPED;
        edu->pri.error = 0;
        return;
    }

    if (edu->pri.ctrl & EDU_PRI_CTRL_CANCEL) {
        edu_pri_cancel(edu);
        return;
    }

    ret = edu_pri_register(edu);
    if (ret) {
        return;
    }

    if (!is_read && !is_write) {
        is_read = true;
        is_write = true;
    }

    edu->pri.error = 0;
    if (edu->pri.ctrl & EDU_PRI_CTRL_BURST) {
        count = edu->pri.burst_count ? edu->pri.burst_count : 1;
        count = MIN(count, EDU_PRI_BURST_MAX);
    }

    for (i = 0; i < count; i++) {
        uint32_t prgi = (edu->pri.prgi + i) & PCI_PRI_PRGI_MASK;
        uint64_t iova = edu->pri.iova + edu->pri.stride * i;
        int one_ret;

        one_ret = edu_pri_request_one(edu, iova, prgi, is_read, is_write,
                                      priv_req, lpig);
        if (one_ret && !ret) {
            ret = one_ret;
        }
    }
}

static void dma_rw(EduState *edu, bool write, dma_addr_t *val, dma_addr_t *dma,
                bool timer)
{
    if (write && (edu->dma.cmd & EDU_DMA_RUN)) {
        return;
    }

    if (write) {
        *dma = *val;
    } else {
        *val = *dma;
    }

    if (timer) {
        timer_mod(&edu->dma_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
    }
}

static uint64_t edu_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    EduState *edu = opaque;
    uint64_t val = ~0ULL;

    if (addr < 0x80 && size != 4) {
        return val;
    }

    if (addr >= 0x80 && size != 4 && size != 8) {
        return val;
    }

    switch (addr) {
    case 0x00:
        val = 0x010000edu;
        break;
    case 0x04:
        val = edu->addr4;
        break;
    case 0x08:
        qemu_mutex_lock(&edu->thr_mutex);
        val = edu->fact;
        qemu_mutex_unlock(&edu->thr_mutex);
        break;
    case 0x20:
        val = qatomic_read(&edu->status);
        break;
    case 0x24:
        val = edu->irq_status;
        break;
    case 0x80:
        dma_rw(edu, false, &val, &edu->dma.src, false);
        break;
    case 0x88:
        dma_rw(edu, false, &val, &edu->dma.dst, false);
        break;
    case 0x90:
        dma_rw(edu, false, &val, &edu->dma.cnt, false);
        break;
    case 0x98:
        dma_rw(edu, false, &val, &edu->dma.cmd, false);
        break;
    case 0xa0:
        val = edu->pri.iova;
        break;
    case 0xa8:
        val = edu->pri.pasid;
        break;
    case 0xac:
        val = edu->pri.ctrl;
        break;
    case 0xb0:
        val = edu->pri.status;
        break;
    case 0xb4:
        val = edu->pri.error;
        break;
    case 0xb8:
        val = edu->pri.prgi;
        break;
    case 0xbc:
        val = edu->pri.requested;
        break;
    case 0xc0:
        val = edu->pri.completed;
        break;
    case 0xc4:
        val = edu->pri.success;
        break;
    case 0xc8:
        val = edu->pri.invalid;
        break;
    case 0xcc:
        val = edu->pri.failure;
        break;
    case 0xd0:
        val = edu->pri.error_count;
        break;
    case 0xd4:
        val = edu->pri.burst_count;
        break;
    case 0xd8:
        val = edu->pri.stride;
        break;
    case 0xe0:
        val = edu->pri.outstanding;
        break;
    case 0xe4:
        val = edu->pri.cancelled;
        break;
    case 0xe8:
        val = edu->pri.dropped;
        break;
    case 0xec:
        val = edu->pri.last_prgi;
        break;
    case 0xf0:
        val = edu->pri.last_response;
        break;
    }

    return val;
}

static void edu_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                unsigned size)
{
    EduState *edu = opaque;

    if (addr < 0x80 && size != 4) {
        return;
    }

    if (addr >= 0x80 && size != 4 && size != 8) {
        return;
    }

    switch (addr) {
    case 0x04:
        edu->addr4 = ~val;
        break;
    case 0x08:
        if (qatomic_read(&edu->status) & EDU_STATUS_COMPUTING) {
            break;
        }
        /* EDU_STATUS_COMPUTING cannot go 0->1 concurrently, because it is only
         * set in this function and it is under the iothread mutex.
         */
        qemu_mutex_lock(&edu->thr_mutex);
        edu->fact = val;
        qatomic_or(&edu->status, EDU_STATUS_COMPUTING);
        qemu_cond_signal(&edu->thr_cond);
        qemu_mutex_unlock(&edu->thr_mutex);
        break;
    case 0x20:
        if (val & EDU_STATUS_IRQFACT) {
            qatomic_or(&edu->status, EDU_STATUS_IRQFACT);
            /* Order check of the COMPUTING flag after setting IRQFACT.  */
            smp_mb__after_rmw();
        } else {
            qatomic_and(&edu->status, ~EDU_STATUS_IRQFACT);
        }
        break;
    case 0x60:
        edu_raise_irq(edu, val);
        break;
    case 0x64:
        edu_lower_irq(edu, val);
        break;
    case 0x80:
        dma_rw(edu, true, &val, &edu->dma.src, false);
        break;
    case 0x88:
        dma_rw(edu, true, &val, &edu->dma.dst, false);
        break;
    case 0x90:
        dma_rw(edu, true, &val, &edu->dma.cnt, false);
        break;
    case 0x98:
        if (!(val & EDU_DMA_RUN)) {
            break;
        }
        dma_rw(edu, true, &val, &edu->dma.cmd, true);
        break;
    case 0xa0:
        edu->pri.iova = val;
        break;
    case 0xa8:
        edu->pri.pasid = val;
        break;
    case 0xac:
        edu->pri.ctrl = val;
        if (val & EDU_PRI_CTRL_RESET_STATS) {
            edu_pri_reset_stats(edu);
        }
        if (!(val & EDU_PRI_CTRL_ENABLE)) {
            edu_pri_unregister(edu);
            edu->pri.status = EDU_PRI_STATUS_IDLE;
            edu->pri.error = 0;
        } else if (val & EDU_PRI_CTRL_CANCEL) {
            edu_pri_cancel(edu);
        } else if (val & EDU_PRI_CTRL_STOP) {
            edu_pri_unregister(edu);
            edu->pri.status = EDU_PRI_STATUS_STOPPED;
            edu->pri.error = 0;
        } else if (val & EDU_PRI_CTRL_REQUEST) {
            edu_pri_request(edu);
        }
        break;
    case 0xb0:
        edu->pri.status = val;
        break;
    case 0xb4:
        edu->pri.error = val;
        break;
    case 0xb8:
        edu->pri.prgi = val & PCI_PRI_PRGI_MASK;
        break;
    case 0xbc:
        edu->pri.requested = val;
        break;
    case 0xc0:
        edu->pri.completed = val;
        break;
    case 0xc4:
        edu->pri.success = val;
        break;
    case 0xc8:
        edu->pri.invalid = val;
        break;
    case 0xcc:
        edu->pri.failure = val;
        break;
    case 0xd0:
        edu->pri.error_count = val;
        break;
    case 0xd4:
        edu->pri.burst_count = val;
        break;
    case 0xd8:
        edu->pri.stride = val;
        break;
    case 0xe4:
        edu->pri.cancelled = val;
        break;
    case 0xe8:
        edu->pri.dropped = val;
        break;
    }
}

static const MemoryRegionOps edu_mmio_ops = {
    .read = edu_mmio_read,
    .write = edu_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },

};

/*
 * We purposely use a thread, so that users are forced to wait for the status
 * register.
 */
static void *edu_fact_thread(void *opaque)
{
    EduState *edu = opaque;

    while (1) {
        uint32_t val, ret = 1;
        uint64_t generation;

        qemu_mutex_lock(&edu->thr_mutex);
        while ((qatomic_read(&edu->status) & EDU_STATUS_COMPUTING) == 0 &&
                        !edu->stopping) {
            qemu_cond_wait(&edu->thr_cond, &edu->thr_mutex);
        }

        if (edu->stopping) {
            qemu_mutex_unlock(&edu->thr_mutex);
            break;
        }

        val = edu->fact;
        generation = edu->reset_generation;
        qemu_mutex_unlock(&edu->thr_mutex);

        while (val > 0) {
            ret *= val--;
        }

        /*
         * We should sleep for a random period here, so that students are
         * forced to check the status properly.
         */

        qemu_mutex_lock(&edu->thr_mutex);
        if (generation != edu->reset_generation) {
            qemu_mutex_unlock(&edu->thr_mutex);
            continue;
        }
        edu->fact = ret;
        qemu_mutex_unlock(&edu->thr_mutex);
        qatomic_and(&edu->status, ~EDU_STATUS_COMPUTING);

        /* Clear COMPUTING flag before checking IRQFACT.  */
        smp_mb__after_rmw();

        if (qatomic_read(&edu->status) & EDU_STATUS_IRQFACT) {
            bql_lock();
            qemu_mutex_lock(&edu->thr_mutex);
            if (generation == edu->reset_generation) {
                edu_raise_irq(edu, FACT_IRQ);
            }
            qemu_mutex_unlock(&edu->thr_mutex);
            bql_unlock();
        }
    }

    return NULL;
}

static void pci_edu_realize(PCIDevice *pdev, Error **errp)
{
    EduState *edu = EDU(pdev);
    uint8_t *pci_conf = pdev->config;

    pci_config_set_interrupt_pin(pci_conf, 1);

    if (pci_is_express(pdev)) {
        pcie_endpoint_cap_init(pdev, EDU_PRI_CAP_EXP_OFFSET);
        pcie_ats_init(pdev, EDU_PRI_CAP_ATS_OFFSET, true);
        pcie_pasid_init(pdev, EDU_PRI_CAP_PASID_OFFSET, 8, false, true);
        pcie_pri_init(pdev, EDU_PRI_CAP_PRI_OFFSET, 32, false);
    }

    if (msi_init(pdev, 0, 1, true, false, errp)) {
        return;
    }

    timer_init_ms(&edu->dma_timer, QEMU_CLOCK_VIRTUAL, edu_dma_timer, edu);

    qemu_mutex_init(&edu->thr_mutex);
    qemu_cond_init(&edu->thr_cond);
    qemu_thread_create(&edu->thread, "edu", edu_fact_thread,
                       edu, QEMU_THREAD_JOINABLE);

    memory_region_init_io(&edu->mmio, OBJECT(edu), &edu_mmio_ops, edu,
                    "edu-mmio", 1 * MiB);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &edu->mmio);
}

static void pci_edu_uninit(PCIDevice *pdev)
{
    EduState *edu = EDU(pdev);

    qemu_mutex_lock(&edu->thr_mutex);
    edu->stopping = true;
    qemu_mutex_unlock(&edu->thr_mutex);
    qemu_cond_signal(&edu->thr_cond);
    qemu_thread_join(&edu->thread);

    qemu_cond_destroy(&edu->thr_cond);
    qemu_mutex_destroy(&edu->thr_mutex);

    edu_pri_unregister(edu);
    timer_del(&edu->dma_timer);
    msi_uninit(pdev);
}

static void edu_instance_init(Object *obj)
{
    EduState *edu = EDU(obj);

    edu->dma_mask = (1UL << 28) - 1;
    object_property_add_uint64_ptr(obj, "dma_mask",
                                   &edu->dma_mask, OBJ_PROP_FLAG_READWRITE);
}

static void edu_reset_hold(Object *obj, ResetType type)
{
    EduState *edu = EDU(obj);

    timer_del(&edu->dma_timer);
    edu_pri_cancel(edu);
    edu_lower_irq(edu, ~0U);
    qemu_mutex_lock(&edu->thr_mutex);
    edu->reset_generation++;
    edu->addr4 = 0;
    edu->fact = 0;
    qatomic_set(&edu->status, 0);
    memset(&edu->dma, 0, sizeof(edu->dma));
    memset(edu->dma_buf, 0, sizeof(edu->dma_buf));
    qemu_mutex_unlock(&edu->thr_mutex);
}

static void edu_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);
    ResettableClass *rc = RESETTABLE_CLASS(class);

    rc->phases.hold = edu_reset_hold;

    k->realize = pci_edu_realize;
    k->exit = pci_edu_uninit;
    k->vendor_id = PCI_VENDOR_ID_QEMU;
    k->device_id = 0x11e8;
    k->revision = 0x10;
    k->class_id = PCI_CLASS_OTHERS;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo edu_types[] = {
    {
        .name          = TYPE_PCI_EDU_DEVICE,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(EduState),
        .instance_init = edu_instance_init,
        .class_init    = edu_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_PCIE_DEVICE },
            { },
        },
    }
};

DEFINE_TYPES(edu_types)
