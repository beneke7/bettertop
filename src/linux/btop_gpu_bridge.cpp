/* SPDX-License-Identifier: Apache-2.0 */

#include "btop_gpu_bridge.hpp"

#if defined(__linux__) && defined(GPU_SUPPORT)

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <utility>

#include "../btop_config.hpp"

extern "C" char** environ;

namespace Gpu::Bridge {
namespace {

using Clock = std::chrono::steady_clock;
constexpr uint64_t stale_after_ns = 3'000'000'000ULL;
constexpr uint32_t initial_backoff_ms = 500;
constexpr uint32_t maximum_backoff_ms = 30'000;

// ponytail: one helper serves the fleet; per-GPU workers only if partial driver hangs need live sibling metrics.
struct Controller {
	Snapshot snapshot;
	pid_t pid = -1;
	int read_fd = -1;
	std::array<unsigned char, BETTERTOP_GPU_MAX_FRAME_BYTES> input{};
	size_t input_size = 0;
	Clock::time_point retry_at{};
	Clock::time_point last_frame_at{};
	Clock::time_point kill_at{};
	uint32_t backoff_ms = initial_backoff_ms;
	uint64_t last_wire_sequence = 0;
	bool started = false;
	bool enabled = false;
	bool diagnostics = false;
	bool force_retry = false;
	bool terminate_sent = false;
	bool kill_sent = false;
	bool has_wire_sequence = false;
};

Controller& controller() {
	static Controller instance;
	return instance;
}

std::atomic_bool retry_requested{};

auto monotonic_ns() noexcept -> uint64_t {
	timespec ts{};
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
	return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

auto nvidia_enabled() noexcept -> bool {
	try {
		return Config::getS("shown_gpus").contains("nvidia");
	} catch (...) {
		return false;
	}
}

auto diagnostics_requested() noexcept -> bool {
	try {
		return Config::getB("nvml_measure_pcie_speeds");
	} catch (...) {
		return false;
	}
}

void close_pipe(Controller& state) noexcept {
	if (state.read_fd >= 0) {
		close(state.read_fd);
		state.read_fd = -1;
	}
	state.input_size = 0;
}

void set_failure_backoff(Controller& state) noexcept {
	state.retry_at = Clock::now() + std::chrono::milliseconds(state.backoff_ms);
	state.backoff_ms = std::min(state.backoff_ms * 2U, maximum_backoff_ms);
}

auto helper_path() -> std::string {
	std::array<char, 4096> executable{};
	const ssize_t count = readlink("/proc/self/exe", executable.data(), executable.size() - 1);
	if (count <= 0 || static_cast<size_t>(count) == executable.size() - 1) return {};
	executable[static_cast<size_t>(count)] = '\0';
	return (std::filesystem::path(executable.data()).parent_path() / "bettertop-gpu").string();
}

void try_spawn(Controller& state) noexcept {
	if (!state.started || !state.enabled || state.pid > 0 || Clock::now() < state.retry_at) return;

	std::string path;
	try {
		path = helper_path();
	} catch (...) {
		state.snapshot.helper_status = BETTERTOP_GPU_STATUS_UNAVAILABLE;
		set_failure_backoff(state);
		return;
	}
	if (path.empty()) {
		state.snapshot.helper_status = BETTERTOP_GPU_STATUS_UNAVAILABLE;
		set_failure_backoff(state);
		return;
	}

	int pipe_fds[2] = {-1, -1};
	if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
		state.snapshot.helper_status = BETTERTOP_GPU_STATUS_ERROR;
		set_failure_backoff(state);
		return;
	}

	posix_spawn_file_actions_t actions;
	int error = posix_spawn_file_actions_init(&actions);
	const bool actions_initialized = error == 0;
	if (error == 0) error = posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
	if (error == 0) error = posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
	if (error == 0) error = posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
	if (error != 0) {
		if (actions_initialized) posix_spawn_file_actions_destroy(&actions);
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		state.snapshot.helper_status = BETTERTOP_GPU_STATUS_ERROR;
		set_failure_backoff(state);
		return;
	}

	char* argv[5];
	int argc = 0;
	argv[argc++] = const_cast<char*>(path.c_str());
	argv[argc++] = const_cast<char*>("--interval-ms");
	argv[argc++] = const_cast<char*>("1000");
	if (state.diagnostics) argv[argc++] = const_cast<char*>("--diagnostics");
	argv[argc] = nullptr;
	pid_t child = -1;
	error = posix_spawn(&child, path.c_str(), &actions, nullptr, argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(pipe_fds[1]);
	if (error != 0) {
		close(pipe_fds[0]);
		state.snapshot.helper_status = BETTERTOP_GPU_STATUS_UNAVAILABLE;
		set_failure_backoff(state);
		return;
	}

	const int old_flags = fcntl(pipe_fds[0], F_GETFL, 0);
	if (old_flags < 0 || fcntl(pipe_fds[0], F_SETFL, old_flags | O_NONBLOCK) < 0) {
		state.pid = child;
		state.read_fd = pipe_fds[0];
		state.terminate_sent = kill(child, SIGTERM) == 0;
		close_pipe(state);
		state.snapshot.helper_status = BETTERTOP_GPU_STATUS_ERROR;
		return;
	}

	state.pid = child;
	state.read_fd = pipe_fds[0];
	state.input_size = 0;
	state.last_frame_at = Clock::now();
	state.has_wire_sequence = false;
	state.terminate_sent = false;
	state.kill_sent = false;
	state.snapshot.helper_status = BETTERTOP_GPU_STATUS_UNAVAILABLE;
}

auto frame_size_is_valid(const bettertop_gpu_frame_header& header) noexcept -> bool {
	if (std::memcmp(header.magic, BETTERTOP_GPU_MAGIC, sizeof(header.magic)) != 0 ||
		header.version != BETTERTOP_GPU_PROTOCOL_VERSION || header.header_size != sizeof(header) ||
		header.device_count > BETTERTOP_GPU_MAX_DEVICES || header.process_count > BETTERTOP_GPU_MAX_PROCESSES ||
		header.status > BETTERTOP_GPU_STATUS_ERROR || header.sequence == 0 ||
		(header.flags & ~(BETTERTOP_GPU_TRUNCATED_DEVICES | BETTERTOP_GPU_TRUNCATED_PROCESSES)) != 0)
		return false;
	const uint64_t expected = sizeof(header) + static_cast<uint64_t>(header.device_count) * sizeof(bettertop_gpu_device_record) +
						  static_cast<uint64_t>(header.process_count) * sizeof(bettertop_gpu_process_record);
	return expected == header.frame_size && expected <= BETTERTOP_GPU_MAX_FRAME_BYTES;
}

auto decode_frame(Controller& state, const unsigned char* frame, const bettertop_gpu_frame_header& header) -> bool {
	Snapshot& snapshot = state.snapshot;
	if (state.has_wire_sequence && header.sequence <= state.last_wire_sequence) return false;
	if (header.status != BETTERTOP_GPU_STATUS_OK && (header.device_count != 0 || header.process_count != 0)) return false;
	std::vector<bettertop_gpu_device_record> devices(header.device_count);
	std::vector<bettertop_gpu_process_record> processes(header.process_count);
	const unsigned char* device_bytes = frame + sizeof(header);
	const unsigned char* process_bytes = device_bytes + static_cast<size_t>(header.device_count) * sizeof(bettertop_gpu_device_record);
	if (!devices.empty()) std::memcpy(devices.data(), device_bytes, devices.size() * sizeof(devices.front()));
	if (!processes.empty()) std::memcpy(processes.data(), process_bytes, processes.size() * sizeof(processes.front()));
	constexpr uint64_t known_device_fields = (1ULL << 11) - 1;
	for (size_t i = 0; i < devices.size(); ++i) {
		const auto& device = devices[i];
		if (device.display_index != i || (device.valid_fields & ~known_device_fields) != 0 ||
			((device.valid_fields & BETTERTOP_GPU_DEVICE_GPU_UTIL) && device.gpu_util_pct > 100) ||
			((device.valid_fields & BETTERTOP_GPU_DEVICE_MEMORY_UTIL) && device.memory_util_pct > 100) ||
			!std::memchr(device.name, '\0', sizeof(device.name)) ||
			!std::memchr(device.uuid, '\0', sizeof(device.uuid)) ||
			!std::memchr(device.pci_bus_id, '\0', sizeof(device.pci_bus_id)))
			return false;
	}
	for (uint32_t i = 0; i < header.process_count; ++i) {
		if (processes[i].device_record_index >= header.device_count || processes[i].pid == 0 ||
			(processes[i].kind_flags & ~(BETTERTOP_GPU_PROCESS_COMPUTE | BETTERTOP_GPU_PROCESS_GRAPHICS)) != 0 ||
			(processes[i].valid_fields & ~(BETTERTOP_GPU_PROCESS_MEMORY | BETTERTOP_GPU_PROCESS_GPU_UTIL)) != 0 ||
			((processes[i].valid_fields & BETTERTOP_GPU_PROCESS_GPU_UTIL) && processes[i].gpu_util_pct > 100))
			return false;
	}
	if (header.monotonic_sample_ns == 0 || header.monotonic_sample_ns > monotonic_ns()) return false;

	snapshot.helper_status = header.status;
	snapshot.status_detail = header.status_detail;
	snapshot.flags = header.flags;
	state.last_wire_sequence = header.sequence;
	state.has_wire_sequence = true;
	state.last_frame_at = Clock::now();
	if (header.status != BETTERTOP_GPU_STATUS_OK) return true;

	snapshot.devices = std::move(devices);
	snapshot.processes = std::move(processes);
	++snapshot.sequence; // Keep collector generations monotonic across helper restarts.
	snapshot.sampled_at_ns = header.monotonic_sample_ns;
	snapshot.has_sample = true;
	state.backoff_ms = initial_backoff_ms;
	state.retry_at = {};
	return true;
}

auto consume_frames(Controller& state, uint32_t& consumed, uint32_t maximum) -> bool {
	while (consumed < maximum && state.input_size >= sizeof(bettertop_gpu_frame_header)) {
		bettertop_gpu_frame_header header{};
		std::memcpy(&header, state.input.data(), sizeof(header));
		if (!frame_size_is_valid(header)) return false;
		if (state.input_size < header.frame_size) return true;
		if (!decode_frame(state, state.input.data(), header)) return false;
		++consumed;
		const size_t remaining = state.input_size - header.frame_size;
		if (remaining != 0)
			std::memmove(state.input.data(), state.input.data() + header.frame_size, remaining);
		state.input_size = remaining;
	}
	return true;
}

void request_termination(Controller& state) noexcept {
	if (state.pid <= 0 || state.terminate_sent) return;
	if (kill(state.pid, SIGTERM) == 0 || errno == ESRCH) {
		state.terminate_sent = true;
		state.kill_sent = false;
		state.kill_at = Clock::now() + std::chrono::milliseconds(250);
	}
}

void mark_pipe_failed(Controller& state) noexcept {
	close_pipe(state);
	request_termination(state);
}

void read_available(Controller& state) noexcept {
	if (state.read_fd < 0) return;
	size_t bytes_left = BETTERTOP_GPU_MAX_FRAME_BYTES;
	uint32_t frames_read = 0;
	for (;;) {
		bool valid = false;
		try {
			valid = consume_frames(state, frames_read, 4);
		} catch (...) {
			valid = false;
		}
		if (!valid) {
			state.snapshot.helper_status = BETTERTOP_GPU_STATUS_ERROR;
			mark_pipe_failed(state);
			return;
		}
		if (frames_read == 4 || bytes_left == 0) return;
		if (state.input_size == state.input.size()) {
			state.snapshot.helper_status = BETTERTOP_GPU_STATUS_ERROR;
			mark_pipe_failed(state);
			return;
		}
		const size_t room = std::min(state.input.size() - state.input_size, bytes_left);
		const ssize_t count = read(state.read_fd, state.input.data() + state.input_size, room);
		if (count > 0) {
			state.input_size += static_cast<size_t>(count);
			bytes_left -= static_cast<size_t>(count);
			continue;
		}
		if (count == 0) {
			close_pipe(state);
			request_termination(state);
			return;
		}
		if (errno == EINTR) continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) return;
		state.snapshot.helper_status = BETTERTOP_GPU_STATUS_ERROR;
		mark_pipe_failed(state);
		return;
	}
}

auto reap_if_exited(Controller& state) noexcept -> bool {
	if (state.pid <= 0) return true;
	int status = 0;
	const pid_t result = waitpid(state.pid, &status, WNOHANG);
	if (result == 0) return false;
	if (result < 0 && errno != ECHILD) return false;
	state.pid = -1;
	state.terminate_sent = false;
	state.kill_sent = false;
	close_pipe(state);
	if (state.force_retry) {
		state.retry_at = Clock::now();
		state.backoff_ms = initial_backoff_ms;
		state.force_retry = false;
	} else set_failure_backoff(state);
	return true;
}

void update_health(Controller& state) noexcept {
	Snapshot& snapshot = state.snapshot;
	if (!state.enabled) {
		snapshot.state = State::disabled;
		if (snapshot.has_sample) {
			const uint64_t now = monotonic_ns();
			snapshot.age_ms = now >= snapshot.sampled_at_ns ? (now - snapshot.sampled_at_ns) / 1'000'000ULL : 0;
		}
		return;
	}
	if (snapshot.has_sample) {
		const uint64_t now = monotonic_ns();
		snapshot.age_ms = now >= snapshot.sampled_at_ns ? (now - snapshot.sampled_at_ns) / 1'000'000ULL : 0;
		snapshot.state = now >= snapshot.sampled_at_ns && now - snapshot.sampled_at_ns <= stale_after_ns
						 ? State::healthy
						 : State::stale;
	} else if (snapshot.helper_status == BETTERTOP_GPU_STATUS_ERROR ||
			   snapshot.helper_status == BETTERTOP_GPU_STATUS_UNAVAILABLE) {
		snapshot.state = state.pid > 0 ? State::starting : State::unavailable;
	} else {
		snapshot.state = State::starting;
	}
}

} // namespace

void start() {
	Controller& state = controller();
	if (state.started) return;
	state.started = true;
	state.enabled = nvidia_enabled();
	state.diagnostics = diagnostics_requested();
	state.snapshot.state = state.enabled ? State::starting : State::disabled;
	if (state.enabled) try_spawn(state);
}

void retry() noexcept {
	retry_requested.store(true, std::memory_order_release);
}

auto poll() noexcept -> const Snapshot& {
	Controller& state = controller();
	if (!state.started) {
		state.started = true;
		state.enabled = nvidia_enabled();
		state.diagnostics = diagnostics_requested();
	}
	try {
		if (retry_requested.exchange(false, std::memory_order_acquire)) {
			state.backoff_ms = initial_backoff_ms;
			state.retry_at = Clock::now();
			if (state.pid > 0) {
				state.force_retry = true;
				close_pipe(state);
				request_termination(state);
			}
		}
		const bool diagnostics = diagnostics_requested();
		if (state.diagnostics != diagnostics) {
			close_pipe(state);
			request_termination(state);
			state.diagnostics = diagnostics;
		}
		const bool enabled = nvidia_enabled();
		if (state.enabled && !enabled) {
			request_termination(state);
			close_pipe(state);
		}
		state.enabled = enabled;
		if (state.enabled) {
			try_spawn(state);
			read_available(state);
		}
		reap_if_exited(state);
		update_health(state);
		const bool startup_timed_out = !state.snapshot.has_sample && state.pid > 0 &&
			Clock::now() - state.last_frame_at > std::chrono::nanoseconds(stale_after_ns);
		const bool data_timed_out = state.snapshot.has_sample && state.snapshot.age_ms > stale_after_ns / 1'000'000ULL &&
			Clock::now() - state.last_frame_at > std::chrono::nanoseconds(stale_after_ns);
		if (state.pid > 0 && (startup_timed_out || data_timed_out || !state.enabled)) {
			close_pipe(state);
			request_termination(state);
		}
		if (state.pid > 0 && state.terminate_sent && !state.kill_sent && Clock::now() >= state.kill_at) {
			if (kill(state.pid, SIGKILL) == 0 || errno == ESRCH) state.kill_sent = true;
		}
		reap_if_exited(state);
		try_spawn(state);
		update_health(state);
	} catch (...) {
		state.snapshot.helper_status = BETTERTOP_GPU_STATUS_ERROR;
		close_pipe(state);
		request_termination(state);
		update_health(state);
	}
	return state.snapshot;
}

void shutdown() noexcept {
	Controller& state = controller();
	state.enabled = false;
	request_termination(state);
	close_pipe(state);
	if (state.pid > 0) {
		const auto wait_until_reaped = [&](std::chrono::milliseconds duration) {
			const auto deadline = Clock::now() + duration;
			while (state.pid > 0 && Clock::now() < deadline) {
				if (reap_if_exited(state)) return;
				timespec pause{0, 10'000'000};
				nanosleep(&pause, nullptr);
			}
		};
		wait_until_reaped(std::chrono::milliseconds(300));
		if (state.pid > 0) {
			kill(state.pid, SIGKILL);
			state.terminate_sent = true;
			state.kill_sent = true;
			wait_until_reaped(std::chrono::milliseconds(300));
		}
	}
	state.started = false;
	state.snapshot.state = state.snapshot.has_sample ? State::stale : State::unavailable;
}

} // namespace Gpu::Bridge

#endif
