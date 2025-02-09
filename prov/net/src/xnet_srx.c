/*
 * Copyright (c) 2018-2022 Intel Corporation. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *	   Redistribution and use in source and binary forms, with or
 *	   without modification, are permitted provided that the following
 *	   conditions are met:
 *
 *		- Redistributions of source code must retain the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer.
 *
 *		- Redistributions in binary form must reproduce the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer in the documentation and/or other materials
 *		  provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <rdma/fi_errno.h>
#include <ofi_prov.h>
#include "xnet.h"

#include <sys/types.h>
#include <ofi_util.h>
#include <unistd.h>
#include <ofi_iov.h>


/* The rdm ep calls directly through to the srx calls, so we need to use the
 * progress active_lock for protection.
 */

static ssize_t
xnet_srx_recvmsg(struct fid_ep *ep_fid, const struct fi_msg *msg,
		 uint64_t flags)
{
	struct xnet_xfer_entry *recv_entry;
	struct xnet_srx *srx;
	ssize_t ret = FI_SUCCESS;

	srx = container_of(ep_fid, struct xnet_srx, rx_fid);
	assert(msg->iov_count <= XNET_IOV_LIMIT);
	assert(!(flags & FI_MULTI_RECV) || msg->iov_count == 1);

	ofi_genlock_lock(xnet_srx2_progress(srx)->active_lock);
	recv_entry = xnet_alloc_xfer(xnet_srx2_progress(srx));
	if (!recv_entry) {
		ret = -FI_EAGAIN;
		goto unlock;
	}

	recv_entry->ctrl_flags = flags & FI_MULTI_RECV;
	recv_entry->cq_flags = FI_MSG | FI_RECV;
	recv_entry->context = msg->context;
	recv_entry->iov_cnt = msg->iov_count;
	memcpy(&recv_entry->iov[0], msg->msg_iov,
	       msg->iov_count * sizeof(*msg->msg_iov));

	slist_insert_tail(&recv_entry->entry, &srx->rx_queue);
unlock:
	ofi_genlock_unlock(xnet_srx2_progress(srx)->active_lock);
	return ret;
}

static ssize_t
xnet_srx_recv(struct fid_ep *ep_fid, void *buf, size_t len, void *desc,
	      fi_addr_t src_addr, void *context)
{
	struct xnet_xfer_entry *recv_entry;
	struct xnet_srx *srx;
	ssize_t ret = FI_SUCCESS;

	srx = container_of(ep_fid, struct xnet_srx, rx_fid);

	ofi_genlock_lock(xnet_srx2_progress(srx)->active_lock);
	recv_entry = xnet_alloc_xfer(xnet_srx2_progress(srx));
	if (!recv_entry) {
		ret = -FI_EAGAIN;
		goto unlock;
	}

	recv_entry->cq_flags = FI_MSG | FI_RECV;
	recv_entry->context = context;
	recv_entry->iov_cnt = 1;
	recv_entry->iov[0].iov_base = buf;
	recv_entry->iov[0].iov_len = len;

	slist_insert_tail(&recv_entry->entry, &srx->rx_queue);
unlock:
	ofi_genlock_unlock(xnet_srx2_progress(srx)->active_lock);
	return ret;
}

static ssize_t
xnet_srx_recvv(struct fid_ep *ep_fid, const struct iovec *iov, void **desc,
	       size_t count, fi_addr_t src_addr, void *context)
{
	struct xnet_xfer_entry *recv_entry;
	struct xnet_srx *srx;
	ssize_t ret = FI_SUCCESS;

	srx = container_of(ep_fid, struct xnet_srx, rx_fid);
	assert(count <= XNET_IOV_LIMIT);

	ofi_genlock_lock(xnet_srx2_progress(srx)->active_lock);
	recv_entry = xnet_alloc_xfer(xnet_srx2_progress(srx));
	if (!recv_entry) {
		ret = -FI_EAGAIN;
		goto unlock;
	}

	recv_entry->cq_flags = FI_MSG | FI_RECV;
	recv_entry->context = context;
	recv_entry->iov_cnt = count;
	memcpy(&recv_entry->iov[0], iov, count * sizeof(*iov));

	slist_insert_tail(&recv_entry->entry, &srx->rx_queue);
unlock:
	ofi_genlock_unlock(xnet_srx2_progress(srx)->active_lock);
	return ret;
}

static struct fi_ops_msg xnet_srx_msg_ops = {
	.size = sizeof(struct fi_ops_msg),
	.recv = xnet_srx_recv,
	.recvv = xnet_srx_recvv,
	.recvmsg = xnet_srx_recvmsg,
	.send = fi_no_msg_send,
	.sendv = fi_no_msg_sendv,
	.sendmsg = fi_no_msg_sendmsg,
	.inject = fi_no_msg_inject,
	.senddata = fi_no_msg_senddata,
	.injectdata = fi_no_msg_injectdata,
};

static void
xnet_srx_peek(struct xnet_srx *srx, const struct fi_msg_tagged *msg,
	      uint64_t flags)
{
	struct fi_cq_err_entry err_entry = {0};

	assert(xnet_progress_locked(xnet_srx2_progress(srx)));
	err_entry.op_context = msg->context;
	err_entry.flags = FI_RECV | FI_TAGGED;
	err_entry.tag = msg->tag;
	err_entry.err = FI_ENOMSG;

	ofi_cq_write_error(&srx->cq->util_cq, &err_entry);
}

static ssize_t
xnet_srx_trecvmsg(struct fid_ep *ep_fid, const struct fi_msg_tagged *msg,
		  uint64_t flags)
{
	struct xnet_xfer_entry *recv_entry;
	struct xnet_srx *srx;
	ssize_t ret = FI_SUCCESS;

	srx = container_of(ep_fid, struct xnet_srx, rx_fid);
	assert(msg->iov_count <= XNET_IOV_LIMIT);

	ofi_genlock_lock(xnet_srx2_progress(srx)->active_lock);
	if (flags & FI_PEEK) {
		xnet_srx_peek(srx, msg, flags);
		goto unlock;
	}

	recv_entry = xnet_alloc_xfer(xnet_srx2_progress(srx));
	if (!recv_entry) {
		ret = -FI_EAGAIN;
		goto unlock;
	}

	recv_entry->tag = msg->tag;
	recv_entry->ignore = msg->ignore;
	recv_entry->src_addr = msg->addr;
	recv_entry->cq_flags = FI_TAGGED | FI_RECV;
	recv_entry->context = msg->context;
	recv_entry->iov_cnt = msg->iov_count;
	memcpy(&recv_entry->iov[0], msg->msg_iov,
	       msg->iov_count * sizeof(*msg->msg_iov));

	slist_insert_tail(&recv_entry->entry, &srx->tag_queue);
unlock:
	ofi_genlock_unlock(xnet_srx2_progress(srx)->active_lock);
	return ret;
}

static ssize_t
xnet_srx_trecv(struct fid_ep *ep_fid, void *buf, size_t len, void *desc,
	       fi_addr_t src_addr, uint64_t tag, uint64_t ignore, void *context)
{
	struct xnet_xfer_entry *recv_entry;
	struct xnet_srx *srx;
	ssize_t ret = FI_SUCCESS;

	srx = container_of(ep_fid, struct xnet_srx, rx_fid);

	ofi_genlock_lock(xnet_srx2_progress(srx)->active_lock);
	recv_entry = xnet_alloc_xfer(xnet_srx2_progress(srx));
	if (!recv_entry) {
		ret = -FI_EAGAIN;
		goto unlock;
	}

	recv_entry->tag = tag;
	recv_entry->ignore = ignore;
	recv_entry->src_addr = src_addr;
	recv_entry->cq_flags = FI_TAGGED | FI_RECV;
	recv_entry->context = context;
	recv_entry->iov_cnt = 1;
	recv_entry->iov[0].iov_base = buf;
	recv_entry->iov[0].iov_len = len;

	slist_insert_tail(&recv_entry->entry, &srx->tag_queue);
unlock:
	ofi_genlock_unlock(xnet_srx2_progress(srx)->active_lock);
	return ret;
}

static ssize_t
xnet_srx_trecvv(struct fid_ep *ep_fid, const struct iovec *iov, void **desc,
		size_t count, fi_addr_t src_addr, uint64_t tag,
		uint64_t ignore, void *context)
{
	struct xnet_xfer_entry *recv_entry;
	struct xnet_srx *srx;
	ssize_t ret = FI_SUCCESS;

	srx = container_of(ep_fid, struct xnet_srx, rx_fid);
	assert(count <= XNET_IOV_LIMIT);

	ofi_genlock_lock(xnet_srx2_progress(srx)->active_lock);
	recv_entry = xnet_alloc_xfer(xnet_srx2_progress(srx));
	if (!recv_entry) {
		ret = -FI_EAGAIN;
		goto unlock;
	}

	recv_entry->tag = tag;
	recv_entry->ignore = ignore;
	recv_entry->src_addr = src_addr;
	recv_entry->cq_flags = FI_TAGGED | FI_RECV;
	recv_entry->context = context;
	recv_entry->iov_cnt = count;
	memcpy(&recv_entry->iov[0], iov, count * sizeof(*iov));

	slist_insert_tail(&recv_entry->entry, &srx->tag_queue);
unlock:
	ofi_genlock_unlock(xnet_srx2_progress(srx)->active_lock);
	return ret;
}

static struct fi_ops_tagged xnet_srx_tag_ops = {
	.size = sizeof(struct fi_ops_tagged),
	.recv = xnet_srx_trecv,
	.recvv = xnet_srx_trecvv,
	.recvmsg = xnet_srx_trecvmsg,
	.send = fi_no_tagged_send,
	.sendv = fi_no_tagged_sendv,
	.sendmsg = fi_no_tagged_sendmsg,
	.inject = fi_no_tagged_inject,
	.senddata = fi_no_tagged_senddata,
	.injectdata = fi_no_tagged_injectdata,
};

static struct xnet_xfer_entry *
xnet_match_tag(struct xnet_srx *srx, struct xnet_ep *ep, uint64_t tag)
{
	struct xnet_xfer_entry *rx_entry;
	struct slist_entry *item, *prev;

	assert(xnet_progress_locked(xnet_srx2_progress(srx)));
	slist_foreach(&srx->tag_queue, item, prev) {
		rx_entry = container_of(item, struct xnet_xfer_entry, entry);
		if (ofi_match_tag(rx_entry->tag, rx_entry->ignore, tag)) {
			slist_remove(&srx->tag_queue, item, prev);
			return rx_entry;
		}
	}

	return NULL;
}

static struct xnet_xfer_entry *
xnet_match_tag_addr(struct xnet_srx *srx, struct xnet_ep *ep, uint64_t tag)
{
	struct xnet_xfer_entry *rx_entry;
	struct slist_entry *item, *prev;

	assert(xnet_progress_locked(xnet_srx2_progress(srx)));
	slist_foreach(&srx->tag_queue, item, prev) {
		rx_entry = container_of(item, struct xnet_xfer_entry, entry);
		if (ofi_match_tag(rx_entry->tag, rx_entry->ignore, tag) &&
		    ofi_match_addr(rx_entry->src_addr, ep->peer->fi_addr)) {
			slist_remove(&srx->tag_queue, item, prev);
			return rx_entry;
		}
	}

	return NULL;
}

static bool
xnet_srx_cancel_rx(struct xnet_srx *srx, struct slist *queue, void *context)
{
	struct slist_entry *cur, *prev;
	struct xnet_xfer_entry *xfer_entry;

	assert(xnet_progress_locked(xnet_srx2_progress(srx)));
	slist_foreach(queue, cur, prev) {
		xfer_entry = container_of(cur, struct xnet_xfer_entry, entry);
		if (xfer_entry->context == context) {
			slist_remove(queue, cur, prev);
			xnet_cq_report_error(&srx->cq->util_cq, xfer_entry,
					     FI_ECANCELED);
			ofi_buf_free(xfer_entry);
			return true;
		}
	}

	return false;
}

static ssize_t xnet_srx_cancel(fid_t fid, void *context)
{
	struct xnet_srx *srx;

	srx = container_of(fid, struct xnet_srx, rx_fid.fid);

	ofi_genlock_lock(xnet_srx2_progress(srx)->active_lock);
	if (!xnet_srx_cancel_rx(srx, &srx->tag_queue, context))
		xnet_srx_cancel_rx(srx, &srx->rx_queue, context);
	ofi_genlock_unlock(xnet_srx2_progress(srx)->active_lock);

	return 0;
}

static struct fi_ops_ep xnet_srx_ops = {
	.size = sizeof(struct fi_ops_ep),
	.cancel = xnet_srx_cancel,
	.getopt = fi_no_getopt,
	.setopt = fi_no_setopt,
	.tx_ctx = fi_no_tx_ctx,
	.rx_ctx = fi_no_rx_ctx,
	.rx_size_left = fi_no_rx_size_left,
	.tx_size_left = fi_no_tx_size_left,
};

static int xnet_srx_bind(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	struct xnet_srx *srx;

	if (flags != FI_RECV || bfid->fclass != FI_CLASS_CQ)
		return -FI_EINVAL;

	srx = container_of(fid, struct xnet_srx, rx_fid.fid);
	srx->cq = container_of(bfid, struct xnet_cq, util_cq.cq_fid.fid);
	ofi_atomic_inc32(&srx->cq->util_cq.ref);
	return FI_SUCCESS;
}

static int xnet_srx_close(struct fid *fid)
{
	struct xnet_srx *srx;
	struct slist_entry *entry;
	struct xnet_xfer_entry *xfer_entry;

	srx = container_of(fid, struct xnet_srx, rx_fid.fid);

	while (!slist_empty(&srx->rx_queue)) {
		entry = slist_remove_head(&srx->rx_queue);
		xfer_entry = container_of(entry, struct xnet_xfer_entry, entry);
		if (srx->cq) {
			xnet_cq_report_error(&srx->cq->util_cq, xfer_entry,
					      FI_ECANCELED);
		}
		ofi_buf_free(xfer_entry);
	}

	while (!slist_empty(&srx->tag_queue)) {
		entry = slist_remove_head(&srx->tag_queue);
		xfer_entry = container_of(entry, struct xnet_xfer_entry, entry);
		if (srx->cq) {
			xnet_cq_report_error(&srx->cq->util_cq, xfer_entry,
					      FI_ECANCELED);
		}
		ofi_buf_free(xfer_entry);
	}

	if (srx->cq)
		ofi_atomic_dec32(&srx->cq->util_cq.ref);
	ofi_atomic_dec32(&srx->domain->util_domain.ref);
	free(srx);
	return FI_SUCCESS;
}

static struct fi_ops xnet_srx_fid_ops = {
	.size = sizeof(struct fi_ops),
	.close = xnet_srx_close,
	.bind = xnet_srx_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

int xnet_srx_context(struct fid_domain *domain, struct fi_rx_attr *attr,
		     struct fid_ep **rx_ep, void *context)
{
	struct xnet_srx *srx;

	srx = calloc(1, sizeof(*srx));
	if (!srx)
		return -FI_ENOMEM;

	srx->rx_fid.fid.fclass = FI_CLASS_SRX_CTX;
	srx->rx_fid.fid.context = context;
	srx->rx_fid.fid.ops = &xnet_srx_fid_ops;
	srx->rx_fid.ops = &xnet_srx_ops;

	srx->rx_fid.msg = &xnet_srx_msg_ops;
	srx->rx_fid.tagged = &xnet_srx_tag_ops;
	slist_init(&srx->rx_queue);
	slist_init(&srx->tag_queue);

	srx->domain = container_of(domain, struct xnet_domain,
				   util_domain.domain_fid);
	ofi_atomic_inc32(&srx->domain->util_domain.ref);
	srx->match_tag_rx = (attr->caps & FI_DIRECTED_RECV) ?
			    xnet_match_tag_addr : xnet_match_tag;
	srx->op_flags = attr->op_flags;
	srx->min_multi_recv_size = XNET_MIN_MULTI_RECV;
	*rx_ep = &srx->rx_fid;
	return FI_SUCCESS;
}
