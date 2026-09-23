/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#if defined(__linux__) && defined(GPU_SUPPORT)

#include <cstdint>
#include <vector>

#include "../bettertop_gpu_protocol.h"

namespace Gpu::Bridge {

enum class State : uint8_t { starting, healthy, stale, unavailable, disabled };

struct Snapshot {
	State state = State::starting;
	std::vector<bettertop_gpu_device_record> devices;
	std::vector<bettertop_gpu_process_record> processes;
	uint64_t sequence = 0;
	uint64_t sampled_at_ns = 0;
	uint64_t age_ms = 0;
	uint32_t helper_status = BETTERTOP_GPU_STATUS_UNAVAILABLE;
	uint32_t status_detail = 0;
	uint32_t flags = 0;
	bool has_sample = false;
};

void start();
void shutdown() noexcept;
void retry() noexcept;
auto poll() noexcept -> const Snapshot&;

} // namespace Gpu::Bridge

#endif
