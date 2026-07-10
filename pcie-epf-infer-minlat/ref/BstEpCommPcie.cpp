/*
 * BstEpCommPcie — reference userspace for pcie-epf-infer-minlat v2
 *
 * Copy into MNN tree as backend/bst/execution/BstEpCommPcie.{hpp,cpp}
 * (this file is the .cpp body; header is documented in EP2_COMM.md).
 *
 * CRITICAL for MoE EP-2 all-to-all:
 *   Use post_recv() on ALL peers FIRST, then send(), then wait_recv().
 *   Do NOT call blocking send() on both ranks before either POST_RECV —
 *   that deadlocks on credit (posted_mask) for 5s.
 */
#include "BstEpCommPcie.hpp"
#include <MNN/MNNDefine.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cstring>

#include "infer_proto_v2.h"

namespace MNN {
namespace BST {

BstEpCommPcie::~BstEpCommPcie()
{
	close();
}

void BstEpCommPcie::close()
{
	for (auto &c : mRcConns) {
		if (c.map && c.map != MAP_FAILED)
			munmap(c.map, c.buf_size);
		if (c.fd >= 0)
			::close(c.fd);
		c = RcConn{};
	}
	mRcConns.clear();

	if (mEpMap && mEpMap != MAP_FAILED) {
		munmap(mEpMap, mEpStagingSize);
		mEpMap = nullptr;
	}
	if (mEpFd >= 0) {
		::close(mEpFd);
		mEpFd = -1;
	}
	mInitialized = false;
}

bool BstEpCommPcie::init(int local_rank, int world_size,
			 const std::vector<std::string> &rc_devs,
			 const std::string &local_ep_dev)
{
	if ((int)rc_devs.size() != world_size) {
		MNN_ERROR("[BstEpCommPcie] rc_devs size mismatch\n");
		return false;
	}

	mLocalRank = local_rank;
	mWorldSize = world_size;
	mRcConns.resize(world_size);

	for (int r = 0; r < world_size; ++r) {
		if (r == local_rank)
			continue;
		if (rc_devs[r].empty()) {
			MNN_ERROR("[BstEpCommPcie] rc_devs[%d] empty\n", r);
			return false;
		}

		int fd = open(rc_devs[r].c_str(), O_RDWR);
		if (fd < 0) {
			MNN_ERROR("[BstEpCommPcie] open %s failed\n",
				  rc_devs[r].c_str());
			return false;
		}
		mRcConns[r].fd = fd;

		struct infer_buf_req br = {};
		if (ioctl(fd, INFER_IOC_ALLOC, &br) < 0) {
			MNN_ERROR("[BstEpCommPcie] INFER_IOC_ALLOC failed rank %d\n",
				  r);
			return false;
		}
		mRcConns[r].buf_size = br.size;
		mRcConns[r].dma_addr = br.dma_addr;

		void *m = mmap(nullptr, br.size, PROT_READ | PROT_WRITE,
			       MAP_SHARED, fd, 0);
		if (m == MAP_FAILED) {
			MNN_ERROR("[BstEpCommPcie] mmap RC staging failed rank %d\n",
				  r);
			return false;
		}
		mRcConns[r].map = m;

		MNN_PRINT("[BstEpCommPcie] RC rank%d→rank%d: dev=%s staging=%zu KB dma=0x%llx\n",
			  local_rank, r, rc_devs[r].c_str(),
			  (size_t)(br.size / 1024),
			  (unsigned long long)br.dma_addr);
	}

	mEpFd = open(local_ep_dev.c_str(), O_RDWR);
	if (mEpFd < 0) {
		MNN_ERROR("[BstEpCommPcie] open %s failed\n", local_ep_dev.c_str());
		return false;
	}

	struct infer_ep_info info = {};
	if (ioctl(mEpFd, INFER_EP_IOC_GET_INFO, &info) < 0) {
		MNN_ERROR("[BstEpCommPcie] INFER_EP_IOC_GET_INFO failed\n");
		return false;
	}
	mEpStagingSize = info.staging_size;

	if (mEpStagingSize > 0) {
		mEpMap = mmap(nullptr, mEpStagingSize, PROT_READ | PROT_WRITE,
			      MAP_SHARED, mEpFd, 0);
		if (mEpMap == MAP_FAILED) {
			MNN_ERROR("[BstEpCommPcie] mmap EP staging failed\n");
			mEpMap = nullptr;
			return false;
		}
	}

	MNN_PRINT("[BstEpCommPcie] EP dev=%s staging=%zu KB ep_flags=0x%x\n",
		  local_ep_dev.c_str(), mEpStagingSize / 1024, info.ep_flags);

	mInitialized = true;
	MNN_PRINT("[BstEpCommPcie] rank %d ready, world_size=%d\n",
		  local_rank, world_size);
	return true;
}

bool BstEpCommPcie::send(int dst_rank, const void *data, size_t size)
{
	if (dst_rank == mLocalRank)
		return true;
	if (dst_rank < 0 || dst_rank >= mWorldSize || mRcConns[dst_rank].fd < 0) {
		MNN_ERROR("[BstEpCommPcie] send: invalid dst_rank=%d\n", dst_rank);
		return false;
	}

	RcConn &c = mRcConns[dst_rank];
	if (size > c.buf_size) {
		MNN_ERROR("[BstEpCommPcie] send: size %zu > staging %zu\n",
			  size, (size_t)c.buf_size);
		return false;
	}

	memcpy(c.map, data, size);

	{
		struct infer_credit cr = {};
		const uint32_t slot_bit = (1u << SLOT);
		const int max_poll = 50000;
		int n = 0;

		do {
			if (ioctl(c.fd, INFER_IOC_GET_CREDIT, &cr) == 0 &&
			    (cr.posted_mask & slot_bit))
				break;
			usleep(100);
		} while (++n < max_poll);
		if (n >= max_poll) {
			MNN_ERROR("[BstEpCommPcie] send: slot %u not POSTED after 5s "
				  "(peer must post_recv FIRST; posted=0x%x)\n",
				  SLOT, cr.posted_mask);
			return false;
		}
	}

	struct infer_push p = {};
	p.slot = SLOT;
	p.size = static_cast<uint32_t>(size);
	p.pci_addr = c.dma_addr;
	p.timeout_us = 5000000;

	if (ioctl(c.fd, INFER_IOC_PUSH, &p) < 0 || p.result != 0) {
		MNN_ERROR("[BstEpCommPcie] send PUSH failed result=%d\n", p.result);
		return false;
	}
	return true;
}

bool BstEpCommPcie::post_recv(int src_rank, size_t capacity)
{
	if (src_rank == mLocalRank)
		return true;
	if (src_rank < 0 || src_rank >= mWorldSize || mEpFd < 0) {
		MNN_ERROR("[BstEpCommPcie] post_recv: invalid src_rank=%d\n",
			  src_rank);
		return false;
	}
	if (!mEpMap || mEpStagingSize == 0) {
		MNN_ERROR("[BstEpCommPcie] post_recv: EP staging not available\n");
		return false;
	}

	struct infer_ep_recv_reg post = {};
	post.slot = SLOT;
	post.flags = INFER_EP_REG_F_STAGING;
	post.capacity = (capacity < mEpStagingSize) ? capacity : mEpStagingSize;

	if (ioctl(mEpFd, INFER_EP_IOC_POST_RECV, &post) < 0) {
		MNN_ERROR("[BstEpCommPcie] post_recv POST_RECV failed\n");
		return false;
	}
	return true;
}

bool BstEpCommPcie::wait_recv(int src_rank, void *buf, size_t size)
{
	if (src_rank == mLocalRank)
		return true;
	if (src_rank < 0 || src_rank >= mWorldSize || mEpFd < 0) {
		MNN_ERROR("[BstEpCommPcie] wait_recv: invalid src_rank=%d\n",
			  src_rank);
		return false;
	}

	struct infer_ep_wait w = {};
	w.slot = SLOT;
	w.timeout_us = 10000000;

	if (ioctl(mEpFd, INFER_EP_IOC_WAIT, &w) < 0 || w.result != 0) {
		MNN_ERROR("[BstEpCommPcie] wait_recv WAIT failed result=%d\n",
			  w.result);
		return false;
	}

	size_t actual = w.size ? w.size : size;
	if (actual > size)
		actual = size;
	memcpy(buf, mEpMap, actual);
	return true;
}

bool BstEpCommPcie::recv(int src_rank, void *buf, size_t size)
{
	/* Convenient one-shot; for all-to-all prefer post_recv/send/wait_recv. */
	if (!post_recv(src_rank, size))
		return false;
	return wait_recv(src_rank, buf, size);
}

bool BstEpCommPcie::broadcast(int src_rank, void *buf, size_t size)
{
	if (mLocalRank == src_rank) {
		for (int r = 0; r < mWorldSize; ++r) {
			if (r == mLocalRank)
				continue;
			if (!send(r, buf, size))
				return false;
		}
	} else {
		if (!recv(src_rank, buf, size))
			return false;
	}
	return true;
}

} // namespace BST
} // namespace MNN
