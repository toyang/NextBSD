/*-
 * Copyright (c) 2014, Matthew Macy (kmacy@freebsd.org)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *  2. Neither the name of Matthew Macy nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/kobj.h>

#include <net/iflib.h>

/*
 * File organization:
 *  - private structures
 *  - iflib private utility functions
 *  - ifnet functions
 *  - vlan registry and other exported functions
 *  - iflib public core functions
 *
 *
 * Next steps:
 *  - validate the default tx path
 *  - validate the default rx path
 *
 *  - validate queue initialization paths
 *  - validate queue teardown
 *  - validate that all structure fields are initialized

 *  - validate the TSO path
 *  - validate SOP_EOP packet receipt
 *  - add rx_buf recycling
 *  - add multiple buf_ring support - i.e. fan in to single hardware queue
 *  - add SW RSS to demux received data packets to buf_rings for deferred processing
 *    look at handling tx ack processing
 *  - batch per-interface statistics updates
 *
 *  - document the kobj interface in iflib_if.m and export that to the wiki
 *  - port ixgbe to iflib
 *
 */

struct iflib_ctx {

	/*
	 * Pointer to hardware driver's softc
	 */
	iflib_shared_ctx_t *ifc_sctx;
	struct mtx ifc_mtx;
	char ifc_mtx_name[16];
	iflib_txq_t ifc_txqs;
	iflib_rxq_t ifc_rxqs;
	int32_t ifc_txq_size;
	int32_t ifc_rxq_size;
	struct callout *ifc_timer;
	int			ifc_in_detach;

	int ifc_link_state;
	int ifc_link_irq;
	eventhandler_tag ifc_vlan_attach_event;
	eventhandler_tag ifc_vlan_detach_event;
	struct cdev *ifc_led_dev;

	struct iflib_irq ifc_legacy_irq;
	struct task ifc_legacy_task;
	struct grouptask ifc_link_task;
};
#define LINK_ACTIVE(ctx) ((ctx)->ifc_link_state == LINK_STATE_UP)

typedef struct iflib_dma_info {
        bus_addr_t              ifd_paddr;
        caddr_t                 ifd_vaddr;
        bus_dma_tag_t           ifd_tag;
        bus_dmamap_t            ifd_map;
        bus_dma_segment_t       ifd_seg;
        int                     ifd_nseg;
} *iflib_dma_info_t;


#define RX_SW_DESC_MAP_CREATED	(1 << 0)
#define TX_SW_DESC_MAP_CREATED	(1 << 1)
#define RX_SW_DESC_INUSE        (1 << 3)
#define TX_SW_DESC_MAPPED       (1 << 4)

typedef struct iflib_sw_desc {
	bus_dmamap_t    ifsd_map;         /* bus_dma map for packet */
	struct mbuf    *ifsd_m;           /* rx: uninitialized mbuf
									   * tx: pkthdr for the packet
									   */
	caddr_t         ifsd_cl;          /* direct cluster pointer for rx */
	/* XXX needed? */
	int		        ifsd_next_eop;    /* Index of the last buffer */
	int             ifsd_flags;
} *iflib_sd_t;

/* magic number that should be high enough for any hardware */
#define IFLIB_MAX_TX_SEGS 128

typedef struct iflib_txq {
	iflib_ctx_t	ift_ctx;
	uint64_t	ift_flags;
	uint32_t	ift_in_use;
	uint32_t	ift_size;
	uint32_t	ift_processed; /* need to have device tx interrupt update this with credits */
	uint32_t	ift_cleaned;
	uint32_t	ift_stop_thres;
	uint32_t	ift_cidx;
	uint32_t	ift_pidx;
	uint32_t	ift_db_pending;
	uint32_t	ift_tqid;
	uint64_t	ift_tx_bytes;
	uint64_t	ift_tx_packets;
	bus_dma_tag_t		    ift_desc_tag;
	bus_dma_segment_t	ift_segs[IFLIB_MAX_TX_SEGS];

	struct mtx              ift_mtx;
	char                    ift_mtx_name[16];
	iflib_dma_info_t        ift_dma_info;
	int                     ift_id;
	iflib_sd_t              ift_sds;
	int                     ift_nbr;
	struct buf_ring        *ift_br;
	struct grouptask		ift_task;
	int			            ift_qstatus;
	int                     ift_active;
	int                     ift_watchdog_time;
	struct task             ift_task;
} *iflib_txq_t;

#define TXQ_AVAIL(txq) ((txq)->ift_size - (txq)->ift_pidx + (txq)->ift_cidx);

typedef struct iflib_global_context {
	struct taskqgroup	*igc_rx_tqs;		/* per-cpu taskqueues for rx */
	struct taskqgroup	*igc_tx_tqs;		/* per-cpu taskqueues for tx */
	struct taskqueue	 *igc_config_tq;	/* taskqueue for config operations */
} iflib_global_context_t;

struct iflib_global_context global_ctx, *gctx;

typedef struct iflib_rxq {
	iflib_ctx_t	ifr_ctx;
	uint32_t	ifr_buf_size;
	uint32_t	ifr_type;
	uint32_t	ifr_credits;
	uint32_t	ifr_size;
	uint32_t	ifr_cidx;
	uint32_t	ifr_pidx;
	uint32_t	ifr_db_pending;
	uint32_t	ifr_tqid;
	uint64_t	ifr_rx_bytes;
	uint64_t	ifr_rx_packets;
	struct iflib_dma_info	ifr_dma_info;
	struct mtx					ifr_mtx;
	char                    ifr_mtx_name[16];
	int                     ifr_id;
	struct grouptask        ifr_task;
	iflib_sd_t              ifr_sds;
	bus_dma_tag_t           ifr_desc_tag;
	uma_zone_t              ifr_zone;
} *iflib_rxq_t;

/* Our boot-time initialization hook */
static int	iflib_module_event_handler(module_t, int, void *);

static moduledata_t iflib_moduledata = {
	"iflib",
	iflib_module_event_handler,
	NULL
};

DECLARE_MODULE(iflib_mod, iflib_moduledata, SI_SUB_CONFIGURE, SI_ORDER_SECOND);

TASKQGROUP_DEFINE(if_rx_tqg, mp_ncpus, 1);
TASKQGROUP_DEFINE(if_tx_tqg, mp_ncpus, 1);

static void
_iflib_dmamap_cb(void *arg, bus_dma_segment_t *segs, int nseg, int err)
{
	if (err)
		return;
	*(bus_addr_t *) arg = segs[0].ds_addr;
}

static int
iflib_dma_alloc(iflib_ctx_t ctx, bus_size_t size, iflib_dma_info_t dma,
				int mapflags)
{
	int err;
	if_shared_ctx_t sctx = ctx->ifc_sctx;
	device_t dev = sctx->isc_dev;

	err = bus_dma_tag_create(bus_get_dma_tag(dev), /* parent */
				sctx->isc_q_align, 0,	/* alignment, bounds */
				BUS_SPACE_MAXADDR,	/* lowaddr */
				BUS_SPACE_MAXADDR,	/* highaddr */
				NULL, NULL,		/* filter, filterarg */
				size,			/* maxsize */
				1,			/* nsegments */
				size,			/* maxsegsize */
				0,			/* flags */
				NULL,			/* lockfunc */
				NULL,			/* lockarg */
				&dma->ifd_tag);
	if (err) {
		device_printf(dev,
		    "%s: bus_dma_tag_create failed: %d\n",
		    __func__, err);
		goto fail_0;
	}

	err = bus_dmamem_alloc(dma->ifd_tag, (void**) &dma->ifd_vaddr,
	    BUS_DMA_NOWAIT | BUS_DMA_COHERENT, &dma->ifd_map);
	if (err) {
		device_printf(dev,
		    "%s: bus_dmamem_alloc(%ju) failed: %d\n",
		    __func__, (uintmax_t)size, err);
		goto fail_2;
	}

	dma->ifd_paddr = 0;
	err = bus_dmamap_load(dma->ifd_tag, dma->ifd_map, dma->ifd_vaddr,
	    size, _iflib_dmamap_cb, &dma->ifd_paddr, mapflags | BUS_DMA_NOWAIT);
	if (err || dma->ifd_paddr == 0) {
		device_printf(dev,
		    "%s: bus_dmamap_load failed: %d\n",
		    __func__, err);
		goto fail_3;
	}

	return (0);

fail_3:
	bus_dmamap_unload(dma->ifd_tag, dma->ifd_map);
fail_2:
	bus_dmamem_free(dma->ifd_tag, dma->ifd_vaddr, dma->ifd_map);
	bus_dma_tag_destroy(dma->ifd_tag);
fail_0:
	dma->ifd_tag = NULL;

	return (err);
}

static void
iflib_dma_free(iflib_dma_info_t dma)
{
	if (dma->ifd_tag == NULL)
		return;
	if (dma->ifd_paddr != 0) {
		bus_dmamap_sync(dma->ifd_tag, dma->ifd_map,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(dma->ifd_tag, dma->ifd_map);
		dma->ifd_paddr = 0;
	}
	if (dma->ifd_vaddr != NULL) {
		bus_dmamem_free(dma->ifd_tag, dma->ifd_vaddr, dma->ifd_map);
		dma->ifd_vaddr = NULL;
	}
	bus_dma_tag_destroy(dma->ifd_tag);
	dma->ifd_tag = NULL;
}

static int
iflib_fast_intr(void *arg)
{
	struct grouptask *gtask = arg;

	GROUPTASK_ENQUEUE(gtask);
	return (FILTER_HANDLED);
}

static int
_iflib_irq_alloc(iflib_ctx_t ctx, iflib_irq_t *irq, int rid,
	driver_filter_t filter, driver_intr_t handler, void *arg,
	char *name)
{
	int rc;
	struct resource *res;
	void *tag;
	device_t dev = ctx->ifc_sctx->isc_dev;

	irq->ifi_rid = rid;
	res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &irq->ifi_rid,
	    RF_SHAREABLE | RF_ACTIVE);
	if (res == NULL) {
		device_printf(dev,
		    "failed to allocate IRQ for rid %d, name %s.\n", rid, name);
		return (ENOMEM);
	}

	/*
	 * Sort out handler versus filter XXX
	 */
	rc = bus_setup_intr(dev, res, INTR_MPSAFE | INTR_TYPE_NET,
	    NULL, handler, arg, &tag);
	if (rc != 0) {
		device_printf(dev,
		    "failed to setup interrupt for rid %d, name %s: %d\n",
					  rid, name : name ? "unknown", rc);
	} else if (name)
		bus_describe_intr(dev, res, tag, name);

	irq->ifi_tag = tag;
	irq->ifi_res = res;
	return (0);
}


/*********************************************************************
 *
 *  Allocate memory for tx_buffer structures. The tx_buffer stores all
 *  the information needed to transmit a packet on the wire. This is
 *  called only once at attach, setup is done every reset.
 *
 **********************************************************************/

static int
iflib_txsd_alloc(iflib_txq_t txq)
{
	iflib_ctx_t ctx = txq->ift_ctx;
	if_shared_ctx_t sctx = ctx->ifc_sctx;
	device_t dev = sctx->isc_dev;
	iflib_sd_t txsd;
	int err, i;

	/*
	 * Setup DMA descriptor areas.
	 */
	if ((err = bus_dma_tag_create(bus_get_dma_tag(dev),
			       1, 0,			/* alignment, bounds */
			       BUS_SPACE_MAXADDR,	/* lowaddr */
			       BUS_SPACE_MAXADDR,	/* highaddr */
			       NULL, NULL,		/* filter, filterarg */
			       sctx->isc_tx_maxsize,		/* maxsize */
			       sctx->isc_tx_nsegments,	/* nsegments */
			       sctx->isc_tx_maxsegsize,	/* maxsegsize */
			       0,			/* flags */
			       NULL,			/* lockfunc */
			       NULL,			/* lockfuncarg */
			       &txq->ift_desc_tag))) {
		device_printf(dev,"Unable to allocate TX DMA tag\n");
		goto fail;
	}

	if (!(txq->ift_sds =
	    (iflib_sd_t) malloc(sizeof(struct iflib_sw_desc) *
	    sctx->isc_ntxd, M_DEVBUF, M_NOWAIT | M_ZERO))) {
		device_printf(dev, "Unable to allocate tx_buffer memory\n");
		err = ENOMEM;
		goto fail;
	}

        /* Create the descriptor buffer dma maps */
	txsd = txq->ift_sds;
	for (i = 0; i < sctx->isc_ntxd; i++, txsd++) {
		err = bus_dmamap_create(txq->ift_desc_tag, 0, &txsd->ifsd_map);
		if (err != 0) {
			device_printf(dev, "Unable to create TX DMA map\n");
			goto fail;
		}
	}

	return 0;
fail:
	/* We free all, it handles case where we are in the middle */
	iflib_tx_structures_free(ctx);
	return (err);
}

/*
 * XXX Review tx cleaning and buffer mapping
 *
 */

static void
iflib_txq_destroy(iflib_txq_t txq)
{
	iflib_ctx_t ctx = txq->ift_ctx;
	iflib_sd_t sd = txq->ift_sds;

	for (int i = 0; i < sctx->isc_ntxd; i++, sd++)
		iflib_txsd_destroy(ctx, txq, sd);
	if (txq->ift_sds != NULL) {
		free(txq->ift_sds, M_DEVBUF);
		txq->ift_sds = NULL;
	}
	if (txq->ift_desc_tag != NULL) {
		bus_dma_tag_destroy(txq->ift_desc_tag);
		txq->ift_desc_tag = NULL;
	}
	TXQ_LOCK_DESTROY(txq);
}

static void
iflib_txsd_free(iflib_txq_t txq)
{
	if (txq->ift_sds == NULL)
		return;
	iflib_txq_destroy(txq);
}

/*
 *
 * XXX very busted - fix to reflect new naming
 * and value locations
 */

static void
iflib_txq_setup(iflib_txq_t txq)
{
	iflib_ctx_t ctx = txq->ift_ctx;
	if_shared_ctx_t sctx = ctx->isc_sctx;
	iflib_sd_t txsd;
#ifdef DEV_NETMAP
	struct netmap_slot *slot;
	struct netmap_adapter *na = netmap_getna(sctx->isc_ifp);
#endif /* DEV_NETMAP */

	TXQ_LOCK(txr);
#ifdef DEV_NETMAP
	slot = netmap_reset(na, NR_TX, txq->ift_id, 0);
#endif /* DEV_NETMAP */

    /* Set number of descriptors available */
	txr->qstatus = IFLIB_QUEUE_IDLE;

	/* Reset indices */
	txq->ift_cidx = 0;
	txq->ift_pidx = 0;

	/* Free any existing tx buffers. */
	txsd = txq->ift_sds;
	for (i = 0; i < sctx->isc_ntxd; i++, txsd++) {
		iflib_tx_free(ctx, txq, txsd);
#ifdef DEV_NETMAP
		if (slot) {
			int si = netmap_idx_n2k(&na->tx_rings[txq->ift_id], i);
			uint64_t paddr;
			void *addr;

			addr = PNMB(na, slot + si, &paddr);
			/*
			 * XXX need netmap down call
			 */
			txr->tx_base[i].buffer_addr = htole64(paddr);
			/* reload the map for netmap mode */
			netmap_load_map(na, txq->ift_desc_tag, txsd->ifsd_map, addr);
		}
#endif /* DEV_NETMAP */

		/* clear the watch index  XXX really needed?*/
		txsd->next_eop = -1;
	}

	bzero((void *)txq->ift_dma_info.ifd_vaddr, ctx->ifc_txq_size);
	IFC_TXQ_SETUP(sctx, txq->ift_id);
	bus_dmamap_sync(txq->ift_dma_info.ifd_tag, txq->ift_dma_info.ifd_map,
					BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
	TXQ_UNLOCK(txq);
}

static void
iflib_txsd_free(iflib_ctx_t ctx, iflib_txq_t txq, iflib_sd_t txsd)
{
	if (txsd->ifsd_m == NULL)
		return;
	bus_dmamap_sync(txq->ift_entry_tag,
				    txsd->ifsd_map,
				    BUS_DMASYNC_POSTWRITE);
	bus_dmamap_unload(txq->ift_entry_tag,
					  txsd->ifsd_map);
	m_freem(txsd->ifsd_m);
	txsd->ifsd_m = NULL;
}

static void
iflib_txsd_destroy(iflib_ctx_t ctx, iflib_txq_t txq, iflib_sd_t txsd)
{
	if (txsd->ifsd_m != NULL) {
		iflib_txsd_free(ctx, txq, txsd);
		if (txsd->ifsd_map != NULL) {
			bus_dmamap_destroy(txq->ift_desc_tag, txsd->ifsd_map);
			txsd->ifsd_map = NULL;
		}
	} else if (txsd->ifsd_map != NULL) {
		bus_dmamap_unload(txq->ift_desc_tag,
						  txsd->ifsd_map);
		bus_dmamap_destroy(txq->ift_desc_tag,
						   txsd->ifsd_map);
		txsd->ifsd_map = NULL;
	}
}

/*********************************************************************
 *
 *  Allocate memory for rx_buffer structures. Since we use one
 *  rx_buffer per received packet, the maximum number of rx_buffer's
 *  that we'll need is equal to the number of receive descriptors
 *  that we've allocated.
 *
 **********************************************************************/
static int
iflib_rxsd_alloc(struct iflib_rxq_t rxq)
{
	iflib_ctx_t ctx = rxq->ifr_ctx;
	if_shared_ctx_t sctx = ctx->ifc_sctx;
	device_t dev = sctx->isc_dev;
	iflib_sd_t	rxsd;
	int			err;

	rxq->ifr_sds = malloc(sizeof(struct iflib_sw_desc) *
	    sctx->isc_nrxd, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (rxq->ifr_sds == NULL) {
		device_printf(dev, "Unable to allocate rx_buffer memory\n");
		return (ENOMEM);
	}

	err = bus_dma_tag_create(bus_get_dma_tag(dev), /* parent */
				1, 0,			/* alignment, bounds */
				BUS_SPACE_MAXADDR,	/* lowaddr */
				BUS_SPACE_MAXADDR,	/* highaddr */
				NULL, NULL,		/* filter, filterarg */
				sctx->isc_rx_maxsize,	/* maxsize */
				sctx->isc_rx_nsegments,	/* nsegments */
				sctx->isc_rx_maxsegsize,	/* maxsegsize */
				0,			/* flags */
				NULL,			/* lockfunc */
				NULL,			/* lockarg */
				&rxq->ifr_desc_tag);
	if (err) {
		device_printf(dev, "%s: bus_dma_tag_create failed %d\n",
		    __func__, err);
		goto fail;
	}

	rxsd = rxq->ifr_sds;
	for (int i = 0; i < sctx->isc_nrxd; i++, rxsd++) {
		err = bus_dmamap_create(rxq->ifr_desc_tag, 0, &rxsd->ifsd_map);
		if (err) {
			device_printf(dev, "%s: bus_dmamap_create failed: %d\n",
			    __func__, err);
			goto fail;
		}
	}

	return (0);

fail:
	iflib_rx_structures_free(ctx);
	return (err);
}

static void
iflib_rx_bufs_free(iflib_rxq_t rxq)
{
	uint32_t cidx = rxq->ifr_cidx;

	while (rxq->ifr_credits--) {
		iflib_sd_t d = rxq->ifr_sds[cidx];

		if (d->ifsd_flags & RX_SW_DESC_INUSE) {
			bus_dmamap_unload(rxq->ifr_desc_tag, d->ifsd_map);
			bus_dmamap_destroy(rxq->ifr_desc_tag, d->ifsd_map);
			m_init(d->ifsd_m, zone_mbuf, MLEN,
				   M_NOWAIT, MT_DATA, 0);
			uma_zfree(zone_mbuf, d->ifsd_m);
			uma_zfree(q->ifr_zone, d->ifsd_cl);
		}				
		d->ifsd_cl = NULL;
		d->ifsd_m = NULL;
		if (++cidx == rxq->ifr_size)
			cidx = 0;
	}
}

/*********************************************************************
 *
 *  Initialize a receive ring and its buffers.
 *
 **********************************************************************/
static int
iflib_rxq_setup(iflib_rxq_t rxq)
{
	iflib_ctx_t ctx = rxq->ifr_ctx;
	if_shared_ctx_t sctx = ctx->ifc_sctx;
	iflib_sd_t	rxsd;
	bus_dma_segment_t	seg[1];
	int			i, nsegs, err = 0;
#ifdef DEV_NETMAP
	struct netmap_slot *slot;
	struct netmap_adapter *na = netmap_getna(sctx->isc_ifp);
#endif

	/* Clear the ring contents */
	RXQ_LOCK(rxq);
	bzero((void *)rxq->ifr_dma_info.ifd_vaddr, ctx->ifc_rxq_size);
#ifdef DEV_NETMAP
	slot = netmap_reset(na, NR_RX, rxq->ifr_id, 0);
#endif

	/*
	** Free current RX buffer structs and their mbufs
	*/
	iflib_rx_bufs_free(rxq);

	/* Now replenish the mbufs */
	_iflib_refill_rxq(ctx, rxq, sctx->isc_nrxd);

	IFC_RXQ_SETUP(sctx, rxq->ifr_id);
	bus_dmamap_sync(rxq->ifr_ifd.ifd_tag, rxq->ifr_ifd.ifd_map,
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);

fail:
	RXQ_UNLOCK(rxq);
	return (err);
}

/*********************************************************************
 *
 *  Free receive ring data structures
 *
 **********************************************************************/
static void
iflib_rx_sds_free(iflib_rxq_t rxq)
{
	iflib_ctx_t ctx = rxq->ifr_ctx;

	INIT_DEBUGOUT("free_receive_buffers: begin");

	if (rxq->ifr_sds != NULL) {
		free(rxq->ifr_sds, M_DEVBUF);
		rxq->ifr_sds = NULL;
		rxq->ifr_cidx = rxq->ifr_pidx = 0;
	}

	if (rxq->ifr_desc_tag != NULL) {
		bus_dma_tag_destroy(rxq->ifr_desc_tag);
		rxq->ifr_desc_tag = NULL;
	}
}

static void
iflib_init_locked(if_shared_ctx_t sctx)
{
	iflib_ctx_t ctx = sctx->isc_ctx;

	IFC_DISABLE_INTR(sctx);
	callout_stop(ctx->ifc_timer);
	IFC_INIT(sctx);
	if_setdrvflagbits(sctx->isc_ifp, IFF_DRV_RUNNING, 0);
	callout_reset(ctx->ifc_timer, hz, iflib_timer, ctx);
}

static int
iflib_media_change(if_t ifp)
{
	iflib_ctx_t ctx = if_getsoftc(ifp);
	int err;

	CTX_LOCK(ctx);
	err = IFC_MEDIA_CHANGE(ctx->ifc_sctx);
	CTX_UNLOCK(ctx);
	return (err);
}

static int
iflib_media_status(if_t ifp, struct ifmediareq *ifmr)
{
	iflib_ctx_t ctx = if_getsoftc(ifp);

	CTX_LOCK(ctx);
	IFC_MEDIA_STATUS(ctx->ifc_sctx, ifmr);
	CTX_UNLOCK(ctx);
}

static void
iflib_stop(iflib_ctx_t ctx)
{
	iflib_txq_t txq = ctx->ifc_txqs;
	if_shared_ctx_t sctx = ctx->ifc_sctx;

	IFC_DISABLE_INTR(sctx);
	callout_stop(ctx->ifc_timer);
	/* Tell the stack that the interface is no longer active */
	if_setdrvflagbits(sctx->isc_ifp, IFF_DRV_OACTIVE, IFF_DRV_RUNNING);

	/* Wait for curren tx queue users to exit to disarm watchdog timer. */
	for (int i = 0; i < sctx->isc_nqsets; i++, txq++) {
		TXQ_LOCK(txq);
		txq->ift_qstatus = IFLIB_QUEUE_IDLE;
		TXQ_UNLOCK(txq);
	}
	IFC_STOP(sctx);
}

static int
_recycle_rx_buf(if_shared_ctx_t ctx, iflib_rxq_t rxq, uint32_t idx)
{

	if ((err = IFC_RECYCLE_RX_BUF(sctx, rxq, idx)) != 0)
		return (err);

	rxq->ifr_sds[rxq->ifr_pidx] = rxq->ifr_sds[idx];
	rxq->ifr_credits++;
	if (++rxq->ifr_pidx == rxq->ifr_size)
		rxq->ifr_pidx = 0;
	return (0);
}

/*
 * Internal service routines
 */

#if !defined(__i386__) && !defined(__amd64__)
struct refill_rxq_cb_arg {
	int               error;
	bus_dma_segment_t seg;
	int               nseg;
};

static void
_refill_rxq_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	struct refill_rxq_cb_arg *cb_arg = arg;
	
	cb_arg->error = error;
	cb_arg->seg = segs[0];
	cb_arg->nseg = nseg;
}
#endif

/*
 * Process one software descriptor
 */
static struct mbuf *
iflib_rxd_pkt_get(iflib_ctx_t ctx, if_rxd_info_t ri)
{
	iflib_rxq_t rxq = ctx->ifc_rxqs[ri->iri_qidx];
	iflib_sd_t sd = &rxq->ift_sds[ri->iri_cidx];
	uint32_t flags = M_EXT;
	caddr_t cl;
	struct mbuf *m;
	int len = ri->iri_len;
	/* 
	 * most drivers only support 1 segment on receive
	 * so ignore chaining to start
	 */
	if (ri->iri_flags != RXD_SOP_EOP)
		panic("chaining unsupported");

	if (iflib_recycle_enable & ri->iri_len <= IFLIB_RX_COPY_THRES) {
		if ((m = m_gethdr(M_NOWAIT, MT_DATA)) == NULL)
			goto skip_recycle;
		cl = mtod(m, void *);
		memcpy(cl, sd->ifsd_cl, ri->iri_len);
		iflib_recycle_rx_buf(ctx, rxq, ri->iri_cidx);
		m->m_pkthdr.len = m->m_len = len;
		m->m_flags = 0;
	} else {
	skip_recycle:
		bus_dmamap_unload(rxq->ifr_dtag, sd->map);
		cl = sd->ifs_cl;
		m = sd->ifs_m;
		flags |= M_PKTHDR;
		m_init(m, rxq->ifr_zone, rxq->ifr_buf_size, M_NOWAIT, MT_DATA, flags);
		m_cljset(m, cl, rxq->ifr_cltype);
		m->m_len = len;
		m->m_pkthdr.len = len;
	}
	m->m_pkthdr.rcvif = (struct ifnet *)sctx->isc_ifp;
	if (ri->iri_flags & RXD_VLAN) {
		if_setvtag(m, ri->iri_vtag);
		m->m_flags |= M_VLANTAG;
	}

	rxq->ifr_rx_packets++;
	rxq->ifr_rx_bytes += m->m_pkthdr.len;
	return (m);
}

/**
 *	refill_rxq - refill an SGE free-buffer list
 *	@sc: the controller softc
 *	@q: the free-list to refill
 *	@n: the number of new buffers to allocate
 *
 *	(Re)populate an SGE free-buffer list with up to @n new packet buffers.
 *	The caller must assure that @n does not exceed the queue's capacity.
 */
static void
_iflib_refill_rxq(iflib_ctx_t ctx, iflib_rxq_t rxq, int n)
{
	struct rx_sw_desc *rxsd = &rxq->ifr_sd[rxq->ifr_pidx];
	struct mbuf *m;
	caddr_t cl;
	int err;
	uint64_t phys_addr;
	if_shared_ctx_t sctx = ctx->ifc_sctx;

	while (n--) {
		/*
		 * We allocate an uninitialized mbuf + cluster, mbuf is
		 * initialized after rx.
		 */
		if ((cl = m_cljget(NULL, M_NOWAIT, rxq->ifr_buf_size)) == NULL)
			break;
		if ((m = m_gethdr(M_NOWAIT, MT_NOINIT)) == NULL) {
			uma_zfree(rxq->ifr_zone, cl);
			break;
		}
		if ((rxsd->ifsd_flags & RX_SW_DESC_MAP_CREATED) == 0) {
			if ((err = bus_dmamap_create(rxq->ifr_qtag, 0, &rxsd->ifsd_map))) {
				log(LOG_WARNING, "bus_dmamap_create failed %d\n", err);
				uma_zfree(rxq->ifr_zone, cl);
				goto done;
			}
			rxsd->ifsd_flags |= RX_SW_DESC_MAP_CREATED;
		}
#if !defined(__i386__) && !defined(__amd64__)
		{
			struct refill_rxq_cb_arg cb_arg;
			cb_arg.error = 0;
			err = bus_dmamap_load(q->ifr_desc_tag, sd->ifsd_map,
		         cl, q->ifr_buf_size, refill_rxq_cb, &cb_arg, 0);

			if (err != 0 || cb_arg.error) {
				/*
				 * !zone_pack ?
				 */
				if (q->zone == zone_pack)
					uma_zfree(q->ifr_zone, cl);
				m_free(m);
				goto done;
			}
			phys_addr = cb_arg.seg.ds_addr;
		}
#else
		phys_addr = pmap_kextract((vm_offset_t)cl);
#endif
		rxsd->ifsd_flags |= RX_SW_DESC_INUSE;
		rxsd->ifsd_cl = cl;
		rxsd->ifsd_m = m;
		sctx->isc_rxd_refill(sctx, rxq->ifr_id, rxq->ifr_pidx, phys_addr);

		if (++rxq->ifr_pidx == rxq->ifr_qsize) {
			rxq->ifr_pidx = 0;
			rxsd = rxq->ifr_sd;
		}
		rxq->ifr_credits++;
	}

done:
	sctx->isc_rxd_flush(sctx, q->ifr_id, q->ifr_pidx);
}

static bool
iflib_rxeof(iflib_rxq_t rxq, int budget)
{
	iflib_ctx_t ctx = rxq->ctx;
	if_shared_ctx_t sctx = ctx->isc_sctx;
	int cidx = rxq->ifr_cidx;
	struct if_rxd_info ri;
	iflib_dma_info_t di;

	/*
	 * XXX early demux data packets so that if_input processing only handles
	 * acks in interrupt context
	 */
	struct mbuf *mh, *mt;

	if (!RXQ_TRYLOCK(rxq))
		return (false);
#ifdef DEV_NETMAP
	if (netmap_rx_irq(ifp, rxq->ifr_id, &processed)) {
		RXQ_UNLOCK(rxq);
		return (FALSE);
	}
#endif /* DEV_NETMAP */

	ri->iri_qidx = rxq->ifr_id;

	mh = mt = NULL;
	while (__predict_true(budget_left--)) {
		if (__predict_false(!iflib_getactive(ctx)))
			break;
		di = &rxq->ifr_dma_info;
		bus_dmamap_sync(di->ifsd_tag, di->ifsd_map,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
		if (__predict_false(!sctx->isc_rxd_is_new(sctx, rxq->ifr_id, cidx)))
			return (false);
		err = sctx->isc_rxd_pkt_get(sctx, rxq->ifr_id, cidx, &ri);
		bus_dmamap_unload(rxq->ifr_desc_tag, rxq->ifr_sds[cidx].ifsd_map);

		if (++cidx == sctx->isc_nrxd)
			cidx = 0;
		if (err) {
			_recycle_rx_buf(ctx, rxq, ri->ri_cidx);
			continue;
		}

		m = iflib_rxd_pkt_get(ctx, &ri);

		if (mh == NULL)
			mh = mt = m;
		else {
			mt->m_nextpkt = m;
			mt = m;
		}
		_iflib_rxq_refill_lt(ctx, rxq, /* XXX em value */ 8);
	}
	rxq->ifr_cidx = cidx;
	RXQ_UNLOCK(rxq);

	while (mh != NULL) {
		m = mh;
		mh = mh->m_nextpkt;
		m->m_nextpkt = NULL;
		if_input(sctx->isc_ifp, m);
	}

	return sctx->isc_rxd_is_new(sctx, rxq->ifr_id, rxq->ifr_cidx);
}

static void
_task_fn_legacy_intr(void *context, int pending)
{
	if_shared_ctx_t sctx = context;
	iflib_ctx_t ctx = sctx->isc_ctx;
	if_t ifp = sctx->isc_ifp;
	iflib_txq_t	txq = ctx->ifc_txqs;
	iflib_rxq_t	rxq = ctx->ifc_rxqs;
	bool more;

	/* legacy do it all crap function */
	if ((if_getdrvflags(ifp) & IFF_DRV_RUNNING)  == 0)
		goto enable;

	more = iflib_rxeof(rxq, ctx->rx_process_limit);

	if(TXQ_TRYLOCK(txq)) {
		iflib_txq_transmit(ifp, txq, NULL);
		TXQ_UNLOCK(txq);
	}

	if (more) {
		iflib_legacy_intr_deferred(sctx);
		return;
	}

enable:
	IFC_INTR_ENABLE(sctx);
}

static void
_task_fn_tx(void *context, int pending)
{
	iflib_txq_t txq = context;
	if_shared_ctx_t sctx = txq->ift_ctx->ifc_sctx;

	TXQ_LOCK(txq);
	_iflib_txq_transmit(txq, NULL);
	IFC_TX_INTR_ENABLE(sctx, txq->ift_id);
	TXQ_UNLOCK(txq);
}

static void
_task_fn_rx(void *context, int pending)
{
	iflib_rxq_t rxq = context;
	iflib_ctx_t ctx = rxq->ift_ctx;
	if_shared_ctx_t sctx = ctx->ifc_sctx;
	int more = 0;

	if (!(if_getdrvflags(sctx->isc_ifp) & IFF_DRV_RUNNING))
		return;

	if (RXQ_TRY_LOCK(rxq)) {
		if ((more = iflib_rxeof(rxq, ctx->rx_process_limit)) == 0)
			IFC_RX_INTR_ENABLE(sctx, rxq);
		RXQ_UNLOCK(rxq);
	}
	if (more)
		GROUPTASK_ENQUEUE(rxq->ifr_task);
}

static void
_task_fn_link(void *context, int pending)
{
	if_shared_ctx_t sctx = context;
	iflib_ctx_t ctx = sctx->isc_ctx;
	iflib_txq_t txq = ctx->ifc_txqs;

	if (!(if_getdrvflags(ifp) & IFF_DRV_RUNNING))
		return;

	CTX_LOCK(ctx);
	callout_stop(&ctx->ifc_timer);
	IFC_UPDATE_LINK_STATUS(sctx);
	callout_reset(&ctx->ifc_timer, hz, iflib_timer, ctx);
	IFC_LINK_INTR_ENABLE(sctx);
	CTX_UNLOCK(ctx);

	if (LINK_ACTIVE(ctx) == 0)
		return;

	for (int i = 0; i < sctx->isc_nqsets; i++, txq++) {
		if (TXQ_TRYLOCK(txq) == 0)
			continue;
		_iflib_txq_transmit(txq, NULL);
		TXQ_UNLOCK(txq);
	}
}

/*
 * MI independent logic
 *
 */
static void
iflib_timer(void *arg)
{
	iflib_ctx_t ctx = arg;
	if_shared_ctx_t sctx = ctx->ifc_sctx;
	iflib_txq_t txq = ctx->ifc_txqs;
	CTX_LOCK(ctx);
	
	/*
	** Check on the state of the TX queue(s), this 
	** can be done without the lock because its RO
	** and the HUNG state will be static if set.
	*/
	IFC_TIMER(sctx);
	for (int i = 0; i < sctx->isc_nqsets; i++, txq++) {
		if ((txq->ift_qstatus == IFLIB_QUEUE_HUNG) &&
		    (sctx->isc_pause_frames == 0))
			goto hung;

		if (TXQ_AVAIL(txq) <= sctx->isc_tx_nsegments)
			GROUPTASK_ENQUEUE(&txq->ift_task);
	}
	sctx->isc_pause_frames = 0;
	callout_reset(&ctx->ifc_timer, hz, iflib_timer, ctx);
	goto unlock;
	
hung:
	if_setdrvflagbits(sctx->isc_ifp, 0, IFF_DRV_RUNNING);
	device_printf(dev,  "TX(%d) desc avail = %d, pidx = %d\n",
				  txq->ift_id, TXQ_AVAIL(ift), txq->ift_pidx);

	IFC_WATCHDOG_RESET(sctx);
	sctx->isc_watchdog_events++;
	sctx->isc_pause_frames = 0;

	IFC_INIT(sctx);
unlock:
	CTX_UNLOCK(ctx);
}

#define M_CSUM_FLAGS(m) ((m)->m_pkthdr.csum_flags)
#define M_HAS_VLANTAG(m) (m->m_flags & M_VLANTAG)

static int
iflib_encap(iflib_txq_t txq, struct mbuf **m_headp)
{
	iflib_ctx_t ctx = txq->ift_ctx;
	bus_dma_segment_t	*segs = txq->ift_segs;
	struct mbuf		*m, *m_head = *m_headp;
	int pidx = txq->ift_pidx;
	iflib_sd_t txsd = &txq->ift_sds[pidx];
	bus_dmamap_t		map = txsd->ifsd_map;
	bool remap = TRUE;
	int err;

retry:

	err = bus_dmamap_load_mbuf_sg(txq->ift_desc_tag, map,
	    *m_headp, segs, &nsegs, BUS_DMA_NOWAIT);

	if (__predict_false(err)) {
		switch (err) {
		case EFBIG:
			/* try defrag once */
			if (remap == TRUE) {
				remap = FALSE;
				m = m_defrag(*m_headp, M_NOWAIT);
				if (m == NULL) {
					ctx->ift_mbuf_defrag_failed++;
					m_freem(*m_headp);
					*m_headp = NULL;
					error = ENOBUFS;
				} else {
					*m_headp = m;
					goto retry;
				}
			}
			break;
		case ENOMEM:
			txq->ift_no_tx_dma_setup++;
			break;
		default:
			txq->ift_no_tx_dma_setup++;
			m_freem(*m_headp);
			*m_headp = NULL;
			break;
		}
		return (err);
	}

	/*
	 * XXX assumes a 1 to 1 relationship between segments and
	 *        descriptors - this does not hold true on all drivers, e.g.
	 *        cxgb
	 */
	if (nsegs > TXQ_AVAIL(txq)) {
		txq->ift_no_desc_avail++;
		bus_dmamap_unload(txq->ift_desc_tag, map);
		return (ENOBUFS);
	}
	m_head = *m_headp;
	pi->ipi_pkt_hdr = mtod(m_head);
	pi->ipi_segs = segs;
	pi->ipi_nsegs = nsegs;
	pi->ipi_pidx = pidx;
	pi->ipi_csum_flags = M_CSUM_FLAGS(m_head);

	if (M_HAS_VLANTAG(m_head)) {
		pi->ipi_vtag = m->m_pkthdr.ether_vtag;
		pi->ipi_flags |= M_VLANTAG;
	}

	if ((err = sctx->isc_txd_encap(sctx, txq->ift_id, pi)) == 0) {
		bus_dmamap_sync(txq->ift_dma_info.idi_tag, txq->ift_dma_info.idi_map,
						BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
		txq->ift_in_use += pi->ipi_ndesc;
		/* XXX need CTASSERT that ntxd is power of 2 */
		txq->ift_pidx = (pidx + pi->ipi_ndesc) & (sctx->isc_ntxd-1);
		iflib_txd_db_check(ctx, txq, 0);
	}
	return (err);
}

#define BRBITS 8
#define FIRST_QSET(ctx) 0
#define NQSETS(ctx) ((ctx)->ifc_sctx->isc_nqsets)
#define QIDX(ctx, m) ((((m)->m_pkthdr.flowid >> BRBITS) % NQSETS(ctx)) + FIRST_QSET(ctx))
#define BRIDX(txq, m) ((m)->m_pkthdr.flowid % txq->ift_br)
#define DESC_RECLAIMABLE(q) ((int)((q)->ift_processed - (q)->ift_cleaned - (q)->ift_ctx->ifc_nsegments))
#define RECLAIM_THRESH(ctx) ((ctx)->ifc_sctx->isc_tx_reclaim_thresh)
#define MAX_TX_DESC(ctx) ((ctx)->ifc_sctx->isc_tx_nsegments)


static inline int
iflib_enqueue_pkt(if_t ifp, iflib_txq_t txq, struct mbuf *m)
{
	int bridx = 0;

	if (m->m_flags & M_FLOWID)
		bridx = BRIDX(txq, m);

	return (drbr_enqueue(ifp, &txq->ift_br[bridx], m));
}

static inline int
iflib_txq_softq_empty(if_t ifp, iflib_txq_t txq)
{
	int i;

	for (i = 0; i < txq->ift_nbr; i++)
		if (drbr_peek(ifp, txq->ift_br[i]) != NULL)
			return (FALSE);

	return (TRUE);
}

static void
iflib_tx_desc_free(iflib_txq_t txq, int n)
{
	iflib_sd_t txsd;
	uint32_t cidx, mask;

	TXQ_LOCK_ASSERT(txq);
	cidx = txq->ift_cidx;
	mask = txq->ift_size;
	txsd = txq->ift_sds[cidx];

	while (n--) {
		prefetch(txq->ift_sds[(cidx + 1) & mask].ifsd_m);
		prefetch(txq->ift_sds[(cidx + 2) & mask].ifsd_m);

		if (txsd->ifsd_m != NULL) {
			if (txsd->ifsd_flags & TX_SW_DESC_MAPPED) {
				bus_dmamap_unload(txq->ift_desc_tag, txsd->ifsd_map);
				txsd->ifsd_flags &= ~TX_SW_DESC_MAPPED;
			}
			m_freem(txsd->ifsd_m);
			txsd->ifsd_m = NULL;
		}

		++txsd;
		if (++cidx == q->ift_size) {
			cidx = 0;
			txsd = txq->ift_sds;
		}
	}
	txq->ift_cidx = cidx;
}

static __inline int
iflib_completed_tx_reclaim(iflib_txq_t txq, int thresh)
{
	int reclaim = DESC_RECLAIMABLE(txq);

	KASSERT(thresh >= 0, ("invalid threshold to reclaim"));
	TXQ_LOCK_ASSERT(txq);

	if (reclaim <= thresh)
		return (0);

	iflib_tx_desc_free(txq, reclaim);
	txq->ift_cleaned += reclaim;
	txq->ift_in_use -= reclaim;

	if (txq->ift_active == FALSE)
		txq->ift_active = TRUE;

	return (reclaim);
}

static __inline void
iflib_txd_db_check(iflib_ctx_t ctx, iflib_txq_t txq, int ring)
{
	if_shared_ctx_t sctx;

	if (ring || ++txq->ift_db_pending >= 32) {
		sctx = ctx->ifc_sctx;
		wmb();
		sctx->isc_txd_flush(sctx, txq->ift_id, txq->ift_pidx);
	}
}

static void
iflib_start(iflib_txq_t txq)
{
	int bridx;
	struct mbuf *next;
	struct buf_ring *br;
	int resid, enq;

	enq = 0;
	do {
		resid = FALSE;
		for (bridx = 0; bridx < txq->ift_br; bridx++) {
			if ((next = drbr_peek(ifp, txq->ift_br[bridx])) == NULL)
				continue;
			if (!(ifp->if_drv_flags & IFF_DRV_RUNNING) ||
				!LINK_ACTIVE(txq->ift_ctx))
				goto done;
			resid = TRUE;
			iflib_completed_tx_reclaim(txq, RECLAIM_THRESH(ctx));
			if (TXQ_AVAIL(txq) < MAX_TX_DESC(ctx))
				break;
			if ((err = iflib_encap(txr, &next)) != 0) {
				if (next == NULL)
					drbr_advance(ifp, txq->ift_br[bridx]);
				else
					drbr_putback(ifp, txr->br, next);
				goto done;
			}
			drbr_advance(ifp, txr->br);
			enq++;

			txq->ift_tx_bytes += next->m_pkthdr.len;
			txq->ift_tx_packets++;
			if (next->m_flags & M_MCAST)
				if_incomcasts(ifp, 1);
			if_etherbpfmtap(ifp, next);
		}
	} while (resid);

	done:
	if (enq > 0) {
		/* Set the watchdog */
		txq->ift_qstatus = IFLIB_QUEUE_WORKING;
		txq->ift_watchdog_time = ticks;
	}
	if (txq->ift_db_pending)
		iflib_txd_db_check(ctx, txq, 1);
	if (!iflib_txq_softq_empty(ifp, txq) && LINK_ACTIVE(ctx))
		callout_reset_on(&txq->ift_timer, 1, iflib_tx_timeout, txq,
						 txq->ift_timer.c_cpu);

	return (err);
}

static int
iflib_txq_transmit(if_t ifp, iflib_txq_t txq, struct mbuf *m)
{
	iflib_ctx_t ctx = txq->ift_ctx;
	struct mbuf     *next, *mp = m;
	int             avail, err = 0, enq = 0;

	avail = txq->ift_size - txq->ift_in_use;
	TXQ_LOCK_ASSERT(txq);

	if (iflib_txq_softq_empty(ifp, txq) && avail >= MAX_TX_DESC(ctx)) {
		if (iflib_encap(txq, &m)) {
			if (m != NULL && (err = iflib_txq_transmit(ifp, txq, m)) != 0)
				return (err);
		} else {
			if (txq->ift_db_pending)
				iflib_check_txq_db(ctx, txq, 1);
			txq->ift_direct_packets++;
			txq->ift_direct_bytes += m->m_pkthdr.len;
		}
	} else if ((err = iflib_enqueue_pkt(ifp, txq, m)) != 0)
		return (err);

	iflib_completed_tx_reclaim(txq, RECLAIM_THRESH(ctx));

	if (!iflib_txq_softq_empty(ifp, txq) && LINK_ACTIVE(ctx))
		iflib_start(txq);

	return (0);
}

void
iflib_intr_rx(void *arg)
{
	struct rx_ring *rxr = arg;

	++rxr->rx_irq;
	_task_fn_rx(arg, 0);
}

void
iflib_intr_tx(void *arg)
{
	iflib_txq_t txr = arg;

	++txr->tx_irq;
	_task_fn_tx(arg, 0);
}

void
iflib_intr_link(void *arg)
{
	iflib_ctx_t ctx = arg;

	++ctx->ifc_link_irq;
	_task_fn_link(arg, 0);
}

static int
iflib_sysctl_int_delay(SYSCTL_HANDLER_ARGS)
{
	int err;
	if_int_delay_info_t info;
	if_shared_ctx_t sctx;

	info = (if_int_delay_info_t)arg1;
	sctx = info->iidi_sctx;
	info->iidi_req = req;
	info->iidi_oid = oidp;
	SCTX_LOCK(sctx);
	err = IFDI_SYSCTL_INT_DELAY(sctx, info);
	SCTX_UNLOCK(sctx);
	return (err);
}

/*********************************************************************
 *
 *  IFNET FUNCTIONS
 *
 **********************************************************************/

static void
iflib_if_init(void *arg)
{
	iflib_ctx_t ctx = arg;

	iflib_init(ctx->ifc_sctx);
}

static int
iflib_if_transmit(if_t ifp, struct mbuf *m)
{
	iflib_ctx_t	ctx = if_getsoftc(ifp);
	iflib_txq_t txq;
	int err, qidx = 0;

	if ((ifp->if_drv_flags & IFF_DRV_RUNNING) == 0 || !LINK_ACTIVE(ctx)) {
		m_freem(m);
		return (0);
	}

	if ((NQSETS(ctx) > 1) && (m->m_flags & M_FLOWID))
		qidx = QIDX(ctx, m);
	/*
	 * XXX calculate buf_ring based on flowid (divvy up bits?)
	 */
	txq = &ctx->ifc_txqs[qidx];

	if (TXQ_TRYLOCK(txq)) {
		err = iflib_txq_transmit(ifp, txq, m);
		TXQ_UNLOCK(txq);
	} else if (m != NULL)
		err = drbr_enqueue(ifp, &txq->ift_br[0], m);

	return (err);
}

static void
iflib_if_qflush(if_t ifp)
{
	iflib_ctx_t ctx = if_getsoftc(ifp);
	iflib_txq_t txq = ctx->ifc_txqs;
	struct mbuf     *m;

	for (int i = 0; i < NQSETS(ctx); i++, txq++) {
		TXQ_LOCK(txq);
		for (int j = 0; j < txq->ift_nbr; j++) {
			while ((m = buf_ring_dequeue_sc(&txq->ift_br[j])) != NULL)
				m_freem(m);
		}
		TXQ_UNLOCK(txq);
	}
	if_qflush(ifp);
}

static int
iflib_if_ioctl(if_t ifp, u_long command, caddr_t data)
{

	iflib_ctx_t ctx = if_getsoftc(ifp);
	if_shared_ctx_t sctx = ctx->ifc_sctx;
	struct ifreq	*ifr = (struct ifreq *)data;
#if defined(INET) || defined(INET6)
	struct ifaddr	*ifa = (struct ifaddr *)data;
#endif
	bool		avoid_reset = FALSE;
	int		err = 0;

	switch (command) {
	case SIOCSIFADDR:
#ifdef INET
		if (ifa->ifa_addr->sa_family == AF_INET)
			avoid_reset = TRUE;
#endif
#ifdef INET6
		if (ifa->ifa_addr->sa_family == AF_INET6)
			avoid_reset = TRUE;
#endif
		/*
		** Calling init results in link renegotiation,
		** so we avoid doing it when possible.
		*/
		if (avoid_reset) {
			if_setflagbits(ifp, IFF_UP,0);
			if (!(if_getdrvflags(ifp)& IFF_DRV_RUNNING))
				iflib_init(ctx);
#ifdef INET
			if (!(if_getflags(ifp) & IFF_NOARP))
				arp_ifinit_drv(ifp, ifa);
#endif
		} else
			err = ether_ioctl_drv(ifp, command, data);
		break;
	case SIOCSIFMTU:
		CTX_LOCK(ctx);
		IFC_MTU_SET(sctx, mtu);
		iflib_init_locked(ctx);
		if_setmtu(ifp, mtu);
		CTX_UNLOCK(ctx);
		break;
	case SIOCSIFFLAGS:
		CTX_LOCK(ctx);
		if (if_getflags(ifp) & IFF_UP) {
			if (if_getdrvflags(ifp) & IFF_DRV_RUNNING) {
				if ((if_getflags(ifp) ^ adapter->if_flags) &
				    (IFF_PROMISC | IFF_ALLMULTI)) {
					IFC_PROMISC_CONFIG(sctx, if_getflags(ifp));
				}
			} else
				IFC_INIT(sctx);
		} else
			if (if_getdrvflags(ifp) & IFF_DRV_RUNNING)
				IFC_STOP(sctx);
		adapter->if_flags = if_getflags(ifp);
		CTX_UNLOCK(ctx);
		break;

		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		IOCTL_DEBUGOUT("ioctl rcv'd: SIOC(ADD|DEL)MULTI");
		if (if_getdrvflags(ifp) & IFF_DRV_RUNNING) {
			CTX_LOCK(ctx);
			IFC_INTR_DISABLE(sctx);
			IFC_MULTI_SET(sctx);
			IFC_INTR_ENABLE(sctx);
			CTX_LOCK(ctx);
		}
		break;
	case SIOCSIFMEDIA:
		CTX_LOCK(ctx);
		IFC_MEDIA_SET(sctx);
		CTX_UNLOCK(ctx);
		/* falls thru */
	case SIOCGIFMEDIA:
		err = ifmedia_ioctl_drv(ifp, ifr, &adapter->media, command);
		break;
	case SIOCSIFCAP:
	    {
		int mask, reinit;

		IOCTL_DEBUGOUT("ioctl rcv'd: SIOCSIFCAP (Set Capabilities)");
		reinit = 0;
		mask = ifr->ifr_reqcap ^ if_getcapenable(ifp);
		if (mask & IFCAP_POLLING) {
			if (ifr->ifr_reqcap & IFCAP_POLLING) {
#ifdef notyet
				err = ether_poll_register_drv(em_poll, ifp);
#else
				err = ENOTSUP;
#endif
				if (err)
					return (err);
				CTX_LOCK(ctx);
				IFC_DISABLE_INTR(sctx);
				if_setcapenablebit(ifp, IFCAP_POLLING, 0);
				CTX_UNLOCK(ctx);
			} else {
				err = ether_poll_deregister_drv(ifp);
				/* Enable interrupt even in err case */
				CTX_LOCK(ctx);
				IFC_INTR_ENABLE(sctx);
				if_setcapenablebit(ifp, 0, IFCAP_POLLING);
				CTX_UNLOCK(ctx);
			}
		}
		if (mask & IFCAP_HWCSUM) {
			if_togglecapenable(ifp, IFCAP_HWCSUM);
			reinit = 1;
		}
		if (mask & IFCAP_TSO4) {
			if_togglecapenable(ifp, IFCAP_TSO4);
			reinit = 1;
		}
		if (mask & IFCAP_VLAN_HWTAGGING) {
			if_togglecapenable(ifp, IFCAP_VLAN_HWTAGGING);
			reinit = 1;
		}
		if (mask & IFCAP_VLAN_HWFILTER) {
			if_togglecapenable(ifp, IFCAP_VLAN_HWFILTER);
			reinit = 1;
		}
		if (mask & IFCAP_VLAN_HWTSO) {
			if_togglecapenable(ifp, IFCAP_VLAN_HWTSO);
			reinit = 1;
		}
		if ((mask & IFCAP_WOL) &&
		    (if_getcapabilities(ifp) & IFCAP_WOL) != 0) {
			if (mask & IFCAP_WOL_MCAST)
				if_togglecapenable(ifp, IFCAP_WOL_MCAST);
			if (mask & IFCAP_WOL_MAGIC)
				if_togglecapenable(ifp, IFCAP_WOL_MAGIC);
		}
		if (reinit && (if_getdrvflags(ifp) & IFF_DRV_RUNNING)) {
			iflib_init(ctx);
		}
		if_vlancap(ifp);
		break;
	    }

	default:
		err = ether_ioctl_drv(ifp, command, data);
		break;
	}

	return (err);
}

/*********************************************************************
 *
 *  OTHER FUNCTIONS EXPORTED TO THE STACK
 *
 **********************************************************************/

static void
iflib_vlan_register(void *arg, if_t ifp, u16 vtag)
{
	iflib_ctx_t ctx = if_getsoftc(ifp);
	u32 index, bit;

	if ((void *)ctx != arg)
		return;

	if ((vtag == 0) || (vtag > 4095))
		return;

	CTX_LOCK(ctx);
	IFC_VLAN_REGISTER(ctx->ifc_sctx, vtag);
	/* Re-init to load the changes */
	if (if_getcapenable(ifp) & IFCAP_VLAN_HWFILTER)
		iflib_init_locked(ctx);
	CTX_UNLOCK(ctx);
}

static void
iflib_vlan_unregister(void *arg, if_t ifp, u16 vtag)
{
	iflib_ctx_t ctx = if_getsoftc(ifp);

	if ((void *)ctx != arg)
		return;

	if ((vtag == 0) || (vtag > 4095))
		return;

	CTX_LOCK(ctx);
	IFC_VLAN_UNREGISTER(ctx->ifc_sctx, vtag);
	/* Re-init to load the changes */
	if (if_getcapenable(ifp) & IFCAP_VLAN_HWFILTER)
		iflib_init_locked(ctx);
	CTX_UNLOCK(ctx);
}

static void
iflib_led_func(void *arg, int onoff)
{
	if_shared_ctx_t sctx = arg;

	SCTX_LOCK(sctx);
	IFC_LED_FUNC(sctx, onoff);
	SCTX_UNLOCK(sctx);
}

/*********************************************************************
 *
 *  BUS FUNCTION DEFINITIONS
 *
 **********************************************************************/

int
iflib_device_detach(device_t dev)
{
	iflib_shared_ctx_t sctx = device_get_softc(dev);
	iflib_ctx_t ctx = sctx->isc_ctx;
	if_t ifp = sctx->isc_ifp;

	/* Make sure VLANS are not using driver */
	if (if_vlantrunkinuse(ifp)) {
		device_printf(dev,"Vlan in use, detach first\n");
		return (EBUSY);
	}

	CTX_LOCK(ctx);
	ctx->ifc_in_detach = 1;
	iflib_stop(ctx);
	CTX_UNLOCK(ctx);
	CTX_LOCK_DESTROY(ctx);

	/* Unregister VLAN events */
	if (ctx->ifc_vlan_attach_event != NULL)
		EVENTHANDLER_DEREGISTER(vlan_config, ctx->ifc_vlan_attach_event);
	if (ctx->ifc_vlan_detach_event != NULL)
		EVENTHANDLER_DEREGISTER(vlan_unconfig, ctx->ifc_vlan_detach_event);

	ether_ifdetach_drv(ctx->ifc_ifp);
	if (ctx->ifc_led_dev != NULL)
		led_destroy(ctx->ifc_led_dev);
	IFC_DETACH(sctx);
	callout_drain(&ctx->ifc_timer);

#ifdef DEV_NETMAP
	netmap_detach(ifp);
#endif /* DEV_NETMAP */

	bus_generic_detach(dev);
	if_free_drv(sctx->isc_ifp);

	iflib_tx_structures_free(sctx);
	iflib_rx_structures_free(sctx);
	return (0);
}

int
iflib_device_suspend(device_t dev)
{
	if_shared_ctx_t sctx = device_get_softc(dev);

	SCTX_LOCK(sctx);
	IFC_SUSPEND(sctx);
	SCTX_UNLOCK(sctx);

	return bus_generic_suspend(dev);
}

int
iflib_resume(device_t dev)
{
	iflib_txq_t txq = &sctx->isc_ctx->ifc_txqs;
	if_shared_ctx_t sctx = device_get_softc(dev);

	SCTX_LOCK(sctx);
	IFC_RESUME(sctx);
	CTX_UNLOCK(sctx);
	for (int i = 0; i < sctx->isc_nqsets; i++, txq++)
		if (TXQ_TRYLOCK(txq)) {
			iflib_start(txq);
			TXQ_UNLOCK(txq);
		}
	return (bus_generic_resume(dev));
}

/*********************************************************************
 *
 *  MODULE FUNCTION DEFINITIONS
 *
 **********************************************************************/

/*
 * - Start a fast taskqueue thread for each core
 * - Start a taskqueue for control operations
 */
static int
iflib_module_init(void)
{

	gctx = &global_ctx;
	gct->igc_tx_tqg = qgroup_if_tx_tqg;
	gct->igc_rx_tqg = qgroup_if_rx_tqg;

	/*
	 * XXX handle memory allocation failure
	 */
	gctx->igc_config_tq =
		taskqueue_create("if config tq", M_WAITOK, taskqueue_thread_enqueue,
						 &gctx->igc_config_tq);
	taskqueue_start_threads(&gctx->igc_config_tq, 1, PSOCK, "if config tq");

	return (0);
}

static int
iflib_module_event_handler(module_t mod, int what, void *arg)
{
	int err;

	switch (what) {
	case MOD_LOAD:
		if ((err = iflib_module_init()) != 0)
			return (err);
		break;
	case MOD_UNLOAD:
		return (EBUSY);
	default:
		return (EOPNOTSUPP);
	}

	return (0);
}

/*********************************************************************
 *
 *  PUBLIC FUNCTION DEFINITIONS
 *     ordered as in iflib.h
 *
 **********************************************************************/

int
iflib_register(device_t dev, driver_t *driver, uint8_t addr[ETH_ADDR_LEN])
{
	iflib_shared_ctx_t sctx = device_get_softc(dev);
	iflib_ctx_t ctx;
	if_t ifp;

	ctx = malloc(sizeof(struct iflib_ctx), M_DEVBUF, M_WAITOK);
	if (ctx == NULL)
		return (ENOMEM);
	CTX_LOCK_INIT(ctx);
	callout_init_mtx(&ctx->ifc_timer, &ctx->ifc_mtx, 0);
	sctx->isc_ctx = ctx;
	ctx->ifc_sctx = sctx;
	ifp = sctx->isc_ifp = if_gethandle(IFT_ETHER);
	if (ifp == NULL) {
		device_printf(dev, "can not allocate ifnet structure\n");
		return (-1);
	}

	/*
	 * Initialize our context's device specific methods
	 */
	kobj_init((kobj_t) sctx, (kobj_class_t) driver);
	kobj_class_compile((kobj_class_t) driver);
	driver->refs++;

	if_initname_drv(ifp, device_get_name(dev), device_get_unit(dev));
	if_setsoftc(ifp, ctx);
	if_setdev(ifp, dev);
	if_setinitfn(ifp, iflib_if_init);
	if_setioctlfn(ifp, iflib_if_ioctl);
	if_settransmitfn(ifp, iflib_if_transmit);
	if_setqflushfn(ifp, iflib_if_qflush);
	if_setflags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST);
	ether_ifattach_drv(ifp, addr);

	if_setcapabilities(ifp, 0);
	if_setcapenable(ifp, 0);

	ctx->ifc_vlan_attach_event =
		EVENTHANDLER_REGISTER(vlan_config, iflib_vlan_register, ctx,
							  EVENTHANDLER_PRI_FIRST);
	ctx->ifc_vlan_detach_event =
		EVENTHANDLER_REGISTER(vlan_unconfig, iflib_vlan_unregister, adapter,
							  EVENTHANDLER_PRI_FIRST);

	ifmedia_init_drv(&sctx->isc_media, IFM_IMASK,
					 iflib_media_change, iflib_media_status);

	return (ctx);
}

int
iflib_queues_alloc(if_shared_ctx_t sctx, int txq_size, int rxq_size)
{
	iflib_ctx_t ctx = sctx->isc_ctx;
	device_t dev = sctx->isc_dev;
	int nqueues = ctx->ifc_nqsets;
	iflib_txq_t rxq = NULL;
	iflib_rxq_t rxq = NULL;
	int i, err;

	/* Allocate the TX ring struct memory */
	if (!(txq =
	    (iflib_txq_t) malloc(sizeof(struct iflib_txq) *
	    nqueues, M_DEVBUF, M_NOWAIT | M_ZERO))) {
		device_printf(dev, "Unable to allocate TX ring memory\n");
		err = ENOMEM;
		goto fail;
	}

	/* Now allocate the RX */
	if (!(rxq =
	    (iflib_rxq_t) malloc(sizeof(struct iflib_rxq) *
	    nqueues, M_DEVBUF, M_NOWAIT | M_ZERO))) {
		device_printf(dev, "Unable to allocate RX ring memory\n");
		err = ENOMEM;
		goto rx_fail;
	}

	/*
	 * XXX handle allocation failure
	 */
	for (i = 0; i < nqueues; i++, txconf++) {
		/* Set up some basics */
		txq = &ctx->ifc_txqs[i];
		txq->ift_ctx = ctx;
		txq->ift_id = i;

		if (iflib_tx_sds_alloc(txr)) {
			device_printf(dev,
						  "Critical Failure setting up transmit buffers\n");
			err = ENOMEM;
			goto err_tx_desc;
		}

		/* Initialize the TX lock */
		snprintf(txq->ift_mtx_name, sizeof(txq->ift_mtx_name), "%s:tx(%d)",
		    device_get_nameunit(dev), txq->ift_id);
		mtx_init(&txq->ift_mtx, txq->ift_mtx_name, NULL, MTX_DEF);

		if (iflib_dma_alloc(ctx, txq_size,
			&txq->ift_dma_info, BUS_DMA_NOWAIT)) {
			device_printf(dev,
			    "Unable to allocate TX Descriptor memory\n");
			err = ENOMEM;
			goto err_tx_desc;
		}
		bzero((void *)txq->ift_dma_info.ifd_vaddr, txq_size);
		/* Allocate a buf ring */
		txr->br = buf_ring_alloc(4096, M_DEVBUF,
		    M_WAITOK, &txr->ift_mtx);
		/*
		 * Next the RX queues...
		 */
		rxq = &ctx->ifc_rxqs[i];
		rxq->ift_ctx = ctx;
		rxq->ift_id = i;

        /* Allocate receive buffers for the ring*/
		if (iflib_rxsd_alloc(rxq)) {
			device_printf(dev,
			    "Critical Failure setting up receive buffers\n");
			err = ENOMEM;
			goto err_rx_desc;
		}

		/* Initialize the RX lock */
		snprintf(rxq->ifr_mtx_name, sizeof(rxq->ifr_mtx_name), "%s:rx(%d)",
		    device_get_nameunit(dev), rxq->ifr_id);
		mtx_init(&rxq->ifr_mtx, rxq->ifr_mtx_name, NULL, MTX_DEF);

		if (iflib_dma_alloc(ctx, rxq_size,
			&rxq->ift_dma_info, BUS_DMA_NOWAIT)) {
			device_printf(dev,
			    "Unable to allocate TX Descriptor memory\n");
			err = ENOMEM;
			goto err_tx_desc;
		}
		bzero((void *)rxq->ifr_dma_info.ifd_vaddr, rxq_size);
	}
	ctx->ifc_txqs = txqs;
	ctx->ifc_rxqs = rxqs;
	ctx->ifc_txq_size = txq_size;
	ctx->ifc_rxq_size = rxq_size;
	if ((err = IFC_QUEUES_ALLOC(sctx)) != 0)
		iflib_tx_structures_free(ctx);

	return (0);
err_rx_desc:
	for (rxq = ctx->ifc_rxqs; rxconf > 0; rxq++, rxconf--)
		iflib_dma_free(ctx, &rxq->ifr_dma_info);
err_tx_desc:
	for (txq = ctx->ifc_txqs; txconf > 0; txq++, txconf--)
		iflib_dma_free(adapter, &txq->ifr_dma_info);
	free(ctx->ifc_rxqs, M_DEVBUF);
rx_fail:
	free(ctx->ifc_txqs, M_DEVBUF);
fail:
	return (err);
}

void
iflib_tx_structures_setup(if_shared_ctx_t sctx)
{
	iflib_ctx_t ctx = sctx->isc_ctx;
	iflib_txq_t txq = ctx->ifc_txqs;

	for (int i = 0; i < ctx->ifc_nqsets; i++, txq++)
		iflib_txq_setup(txq);
}

void
iflib_tx_structures_free(if_shared_ctx_t sctx)
{
	iflib_ctx_t ctx = sctx->isc_ctx;
	iflib_txq_t txq = ctx->ifc_txqs;

	for (int i = 0; i < ctx->ifc_nqsets; i++, txq++) {
		iflib_txq_destroy(txq);
		iflib_dma_free(&txq->ift_dma_info);
	}
	free(ctx->ifc_txqs, M_DEVBUF);
}

/*********************************************************************
 *
 *  Initialize all receive rings.
 *
 **********************************************************************/
int
iflib_rx_structures_setup(if_shared_ctx_t sctx)
{
	iflib_ctx_t ctx = sctx->isc_ctx;
	iflib_rxq_t rxq = ctx->ifc_rxqs;
	iflib_sd_t rxsd;
	int i, n, q;

	for (q = 0; q < ctx->ifc_nrxq; q++, rxq++)
		if (iflib_rxq_setup(rxq))
			goto fail;

	return (0);
fail:
	/*
	 * Free RX software descriptors allocated so far, we will only handle
	 * the rings that completed, the failing case will have
	 * cleaned up for itself. 'q' failed, so its the terminus.
	 */
	rxq = ctx->ifc_rxqs;
	for (i = 0; i < q; ++i; rxq++) {
		iflib_rx_sds_free(ctx, rxq);
		rxq->ifr_cidx = rxq->ifr_pidx = 0;
	}

	return (ENOBUFS);
}

/*********************************************************************
 *
 *  Free all receive rings.
 *
 **********************************************************************/
void
iflib_rx_structures_free(if_shared_ctx_t sctx)
{
	iflib_ctx_t ctx = sctx->isc_ctx;
	iflib_txq_t rxq = ctx->ifc_rxqs;

	for (int i = 0; i < ctx->ifc_nrxq; i++, rxq++) {
		iflib_rx_sds_free(rxq);
		iflib_dma_free(&rxq->ift_dma_info);
	}
}

void
iflib_txq_addr_get(if_shared_ctx_t sctx, int qidx, uint64_t addrs[2])
{
	iflib_dma_info_t *di = &sctx->isc_ctx->ifc_txqs[qidx].ift_dma_info;

	addrs[0] = di->ifd_vaddr;
	addrs[1] = di->ifd_paddr;
}

void
iflib_rxq_addr_get(if_shared_ctx_t ctx, int qidx, uint64_t addrs[2])
{
	iflib_dma_info_t *di = &sctx->isc_ctx->ifc_rxqs[qidx].ift_dma_info;

	addrs[0] = di->ifd_vaddr;
	addrs[1] = di->ifd_paddr;
}

int
iflib_irq_alloc(if_shared_ctx_t sctx, iflib_irq_t *irq, int rid,
				driver_intr_t handler, void *arg, char *name)
{

	return (_iflib_irq_alloc(sctx->isc_ctx, irq, rid, NULL, handler, arg, name));
}

int
iflib_irq_alloc_generic(if_shared_ctx_t ctx, iflib_irq_t *irq, int rid,
						intr_type_t type, int qid, char *name)
{
	iflib_ctx_t ctx = sctx->isc_ctx;
	struct grouptask *gtask;
	void *q;
	int err;

	switch (type) {
	case IFLIB_INTR_TX:
		q = &ctx->ifc_txqs[qid];
		gtask = &ctx->ifc_txqs[qid].ift_task;
		break;
	case IFLIB_INTR_RX:
		q = &ctx->ifc_rxqs[qid];
		gtask = &ctx->ifr_rxqs[qid].ifr_task;
		break;
	case IFLIB_INTR_LINK:
		q = ctx;
		gtask = &ctx->ifc_link_task;
		break;
	default:
		panic("unknown net intr type");
	}

	err = _iflib_irq_alloc(ctx, irq, rid, iflib_fast_intr, q, gtask, name);
	if (err != 0)
		return (err);
	taskqgroup_attach(gctx->igc_tx_tqs, gtask, q, irq->ii_rid, name);
}

int
iflib_legacy_setup(if_shared_ctx_t sctx, driver_filter_t filter, int *rid)
{
	struct resource *res;
	struct taskqueue *tq, *tx_tq;
	device_t dev = sctx->isc_dev;
	iflib_ctx_t ctx = sctx->isc_ctx;
	iflib_irq_t irq = &ctx->ifc_legacy_irq;

	/* We allocate a single interrupt resource */
	if ((err = iflib_irq_alloc(sctx, &irq, *rid, filter, sctx, NULL)) != 0)
		return (err);

	/*
	 * Allocate a fast interrupt and the associated
	 * deferred processing contexts.
	 *
	 * XXX call taskqgroup_attach
	 */
	GROUPTASK_INIT(&ctx->ifc_legacy_task, 0, _task_fn_legacy_intr, ctx);
	GROUPTASK_INIT(&txq->ift_task, 0, _task_fn_tx, txr);
	GROUPTASK_INIT(&ctx->ifc_link_task, 0, _task_fn_link, ctx);
	return (0);
}

void
iflib_led_create(if_shared_ctx_t sctx)
{
	iflib_ctx_t ctx = sctx->isc_ctx;

	ctx->ifc_led_dev = led_create(iflib_led_func, sctx,
								  device_get_nameunit(sctx->isc_dev));
}

void
iflib_init(if_shared_ctx_t *sctx)
{
	SCTX_LOCK(sctx);
	iflib_init_locked(sctx);
	SCTX_UNLOCK(sctx);
}

void
iflib_legacy_intr_deferred(if_shared_ctx_t sctx)
{
	/*
	 * XXX how does legacy fit in
	 */
	GROUPTASK_ENQUEUE(&sctx->isc_ctx->ifc_legacy_task);
}

void
iflib_link_intr_deferred(if_shared_ctx_t sctx)
{

	GROUPTASK_ENQUEUE(&sctx->isc_ctx->ifc_link_task);
}

void
iflib_linkstate_change(if_shared_ctx_t sctx, uint64_t baudrate, int link_state)
{
	if_t ifp = sctx->isc_ifp;
	iflib_ctx_t ctx = sctx->isc_ctx;
	iflib_txq_t txq = ctx->ifc_txqs;

	if_setbaudrate(ifp, baudrate);
	/* If link down, disable watchdog */
	if ((ctx->ifc_link_state == LINK_STATE_UP) && (link_state == LINK_STATE_DOWN)) {
		for (int i = 0; i < ctx->ifc_nqsets; i++, txq++)
			txq->ift_qstatus = IFLIB_QUEUE_IDLE;
	}
	ctx->ifc_link_state = link_state;
	if_linkstate_change_drv(ifp, link_state);
}

void
iflib_stats_update(if_shared_ctx_t ctx)
{
	struct if_common_stats *stats = &sctx->isc_common_stats;
	if_t ifp = sctx->isc_ifp;
	iflib_ctx_t ctx = sctx->isc_ctx;
	uint64_t bytes, packets;
	int i;

	for (packets = bytes = 0, i = 0; i < sctx->isc_nqsets; i++) {
		bytes += sctx->isc_txqs[i].ift_tx_bytes;
		packets += sctx->isc_txqs[i].ift_tx_packets;
	}
	if_setobytes(ifp, bytes);
	if_setopackets(ifp, packets);
	for (packets = bytes = 0, i = 0; i < sctx->isc_nqsets; i++) {
		bytes += sctx->isc_rxqs[i].ifr_rx_bytes;
		packets += sctx->isc_rxqs[i].ifr_rx_packets;
	}
	if_setibytes(ifp, bytes);
	if_setipackets(ifp, packets);
	if_setcollisions(ifp, stats->ics_colls);
	if_setierrors(ifp, stats->ics_ierrs);
	if_setoerrors(ifp, stats->ics_oerrs);
}

void
iflib_add_int_delay_sysctl(if_shared_ctx_t sctx, const char *name,
	const char *description, if_int_delay_info_t info,
	int offset, int value)
{
	info->iidi_sctx = sctx;
	info->iidi_offset = offset;
	info->iidi_value = value;
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(sctx->isc_dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(sctx->isc_dev)),
	    OID_AUTO, name, CTLTYPE_INT|CTLFLAG_RW,
	    info, 0, iflib_sysctl_int_delay, "I", description);
}
