#ifndef MNN_BST_EP_COMM_PCIE_HPP
#define MNN_BST_EP_COMM_PCIE_HPP

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace MNN {
namespace BST {

/**
 * BstEpCommPcie: EP-2 card interconnect (pcie-epf-infer-minlat v2)
 *
 * Dual-cable unidirectional PUSH:
 *   send  (RC): staging memcpy → wait credit → INFER_IOC_PUSH
 *   recv  (EP): POST_RECV(STAGING) → WAIT → mmap copy
 *
 * MoE all-to-all MUST use:
 *   for peer: post_recv(peer, cap);
 *   for peer: send(peer, data, size);
 *   for peer: wait_recv(peer, buf, size);
 * Never block on send() on both ranks before either POST_RECV.
 *
 * See EP2_COMM.md for failure analysis of test_moe_ep_pcie.
 */
class BstEpCommPcie {
public:
	BstEpCommPcie() = default;
	~BstEpCommPcie();

	bool init(int local_rank, int world_size,
		  const std::vector<std::string> &rc_devs,
		  const std::string &local_ep_dev = "/dev/pci_epf_infer0");

	void close();

	bool isInitialized() const { return mInitialized; }
	int localRank() const { return mLocalRank; }
	int worldSize() const { return mWorldSize; }

	bool send(int dst_rank, const void *data, size_t size);
	bool recv(int src_rank, void *buf, size_t size);
	bool broadcast(int src_rank, void *buf, size_t size);

	bool post_recv(int src_rank, size_t capacity);
	bool wait_recv(int src_rank, void *buf, size_t size);

private:
	struct RcConn {
		int fd = -1;
		void *map = nullptr;
		uint64_t buf_size = 0;
		uint64_t dma_addr = 0;
	};

	int mLocalRank = -1;
	int mWorldSize = 0;
	bool mInitialized = false;

	std::vector<RcConn> mRcConns;

	int mEpFd = -1;
	void *mEpMap = nullptr;
	size_t mEpStagingSize = 0;

	static constexpr uint32_t SLOT = 0;
};

} // namespace BST
} // namespace MNN

#endif
