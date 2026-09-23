/* Copyright 2026 BetterTop contributors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0
*/

#include "btop_ml.hpp"

#include <algorithm>
#include <cassert>
#include <fmt/format.h>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "btop_config.hpp"
#include "btop_theme.hpp"
#include "btop_tools.hpp"

using namespace Tools;

namespace Ml {
	Layout layout(const int width, const int height, const size_t gpu_count) {
		Layout result{};
		if (height <= 0) return result;

		result.too_small = width < 80 || height < 16;
		result.footer = height;
		result.fun_row = std::max(1, height - 1);
		result.gpu_first = std::min(6, height);
		const int room = std::max(0, result.fun_row - result.gpu_first - 7);
		result.gpu_rows = static_cast<int>(std::min(gpu_count, static_cast<size_t>(room)));
		const int max_process_header = std::max(1, result.fun_row - 2);
		result.process_header = std::min(max_process_header, result.gpu_first + (result.gpu_rows == 0 ? 2 : result.gpu_rows + 1));
		result.process_first = std::min(result.fun_row, result.process_header + 2);
		result.process_rows = std::max(0, result.fun_row - result.process_first);
		return result;
	}

	bool self_test() {
		const auto standard = layout(100, 30, 4);
		const auto narrow = layout(40, 10, 4);
		bool valid = standard.gpu_rows == 4 && standard.gpu_first == 6
			&& standard.process_header == 11 && standard.process_first == 13 && standard.process_rows == 16
			&& standard.gpu_first + standard.gpu_rows <= standard.process_header
			&& standard.process_first + standard.process_rows <= standard.fun_row
			&& narrow.too_small && narrow.gpu_rows == 0 && narrow.process_rows == 0
			&& narrow.process_first <= narrow.fun_row && narrow.footer == 10;
		for (int height = 1; height <= 40; ++height) {
			const auto frame = layout(40, height, 64);
			valid = valid && frame.gpu_rows >= 0 && frame.process_rows >= 0
				&& frame.footer <= height && frame.fun_row <= height
				&& frame.process_first + frame.process_rows <= frame.fun_row
				&& (frame.gpu_rows == 0 || frame.gpu_first + frame.gpu_rows <= frame.process_header);
		}
		assert(valid);
		return valid;
	}

	namespace {
		struct ProcessRow {
			size_t pid;
			const Proc::proc_info* host{};
#if defined(GPU_SUPPORT)
			const Gpu::process_info* gpu{};
#endif
		};

		string fit(string value, const int width) {
			if (width <= 0) return {};
			value = replace_ascii_control(std::move(value));
			value = uresize(std::move(value), static_cast<size_t>(width), true);
			return value + string(std::max(0, width - static_cast<int>(ulen(value, true))), ' ');
		}

#if defined(GPU_SUPPORT)
		string percent_or_dash(const bool valid, const long long value) {
			return valid ? fmt::format("{}%", std::clamp(value, 0ll, 100ll)) : "--";
		}
#endif

		string metric_rate(const deque<long long>& values) {
			return values.empty() ? "--" : floating_humanizer(static_cast<uint64_t>(std::max(0ll, values.back())), true, 0, false, true);
		}

		string uptime_text() {
			const auto seconds = static_cast<uint64_t>(std::max(0.0, Tools::system_uptime()));
			const auto days = seconds / 86400;
			const auto hours = (seconds / 3600) % 24;
			const auto minutes = (seconds / 60) % 60;
			return days > 0 ? fmt::format("{}d {:02}:{:02}", days, hours, minutes)
				: fmt::format("{:02}:{:02}:{:02}", hours, minutes, seconds % 60);
		}

#if defined(GPU_SUPPORT)
		string gpu_bar(const int percent, const int width) {
			if (width <= 0) return {};
			const int filled = (std::clamp(percent, 0, 100) * width + 50) / 100;
			return string(filled, '#') + string(width - filled, '-');
		}

		string gpu_history(const Gpu::gpu_info& gpu, const int width) {
			if (width <= 0) return {};
			const auto found = gpu.gpu_percent.find("gpu-totals");
			if (!gpu.supported_functions.gpu_utilization || found == gpu.gpu_percent.end() || found->second.empty())
				return string(width, ' ');
			const auto count = std::min<size_t>(static_cast<size_t>(width), found->second.size());
			string history(static_cast<size_t>(width) - count, ' ');
			for (size_t i = found->second.size() - count; i < found->second.size(); ++i) {
				const auto value = std::clamp(found->second[i], 0ll, 100ll);
				history += value == 0 ? '-' : value < 25 ? '.' : value < 60 ? '=' : '#';
			}
			return history;
		}

		string memory_triplet(const Gpu::gpu_info& gpu) {
			const auto& valid = gpu.supported_functions;
			if (valid.mem_used && valid.mem_total) {
				const auto used = static_cast<uint64_t>(std::max(0ll, gpu.mem_used));
				const auto total = static_cast<uint64_t>(std::max(0ll, gpu.mem_total));
				return floating_humanizer(used, true) + "/" + floating_humanizer(total, true) + "/"
					+ floating_humanizer(total > used ? total - used : 0, true);
			}
			if (valid.mem_used) return floating_humanizer(static_cast<uint64_t>(std::max(0ll, gpu.mem_used)), true) + "/--/--";
			if (valid.mem_total) return "--/" + floating_humanizer(static_cast<uint64_t>(std::max(0ll, gpu.mem_total)), true) + "/--";
			return "--/--/--";
		}

		string device_name(const size_t index, const Gpu::gpu_info& gpu) {
			if (index < Gpu::gpu_names.size() && !Gpu::gpu_names[index].empty()) return Gpu::gpu_names[index];
			if (!gpu.device_id.empty()) return gpu.device_id;
			return fmt::format("GPU {}", index);
		}
#endif

		string host_strip(const Cpu::cpu_info& cpu, const Mem::mem_info& mem, const Net::net_info& net, const int width) {
			const auto cpu_it = cpu.cpu_percent.find("total");
			const bool cpu_valid = cpu_it != cpu.cpu_percent.end() && !cpu_it->second.empty();
			const string cpu_text = cpu_valid ? fmt::format("{}%", cpu_it->second.back()) : "--";
			const auto ram = mem.stats.find("used");
			const uint64_t ram_total = Mem::get_totalMem();
			const string ram_text = ram != mem.stats.end() && ram_total > 0
				? floating_humanizer(ram->second, true) + "/" + floating_humanizer(ram_total, true) : "--/--";
			const auto swap_used = mem.stats.find("swap_used");
			const auto swap_total = mem.stats.find("swap_total");
			const string swap_text = swap_used != mem.stats.end() && swap_total != mem.stats.end()
				? floating_humanizer(swap_used->second, true) + "/" + floating_humanizer(swap_total->second, true) : "--/--";

			const auto disk = std::ranges::find_if(mem.disks_order, [&mem](const auto& name) {
				auto found = mem.disks.find(name);
				return found != mem.disks.end() && (!found->second.io_read.empty() || !found->second.io_write.empty());
			});
			string disk_text = "DISK --";
			if (disk != mem.disks_order.end()) {
				const auto& entry = mem.disks.at(*disk);
				disk_text = fmt::format("DISK {} R:{} W:{}", *disk,
					metric_rate(entry.io_read), metric_rate(entry.io_write));
			}
			if (width < 60) return fmt::format("CPU {} | RAM {} | SWAP {}", cpu_text, ram_text, swap_text);
			if (width < 100) return fmt::format("CPU {} | RAM {} | SWAP {} | UP {} | NET ↓{} ↑{}",
				cpu_text, ram_text, swap_text, uptime_text(), metric_rate(net.bandwidth.at("download")),
				metric_rate(net.bandwidth.at("upload")));
			return fmt::format("CPU {} | RAM {} | SWAP {} | UP {} | NET ↓{} ↑{} | {}",
				cpu_text, ram_text, swap_text, uptime_text(), metric_rate(net.bandwidth.at("download")),
				metric_rate(net.bandwidth.at("upload")), disk_text);
		}

#if defined(GPU_SUPPORT)
		string snapshot_text() {
			string state;
			switch (Gpu::snapshot_state) {
			case Gpu::SnapshotState::starting: state = "starting"; break;
			case Gpu::SnapshotState::healthy: state = "healthy"; break;
			case Gpu::SnapshotState::stale: state = fmt::format("stale {}s", Gpu::snapshot_age_ms / 1000); break;
			case Gpu::SnapshotState::unavailable: state = "unavailable"; break;
			case Gpu::SnapshotState::disabled: state = "disabled"; break;
			}
			if (!Gpu::status_detail.empty()) state += " " + uresize(replace_ascii_control(Gpu::status_detail), 18, true);
			return uresize(std::move(state), 28, true);
		}

		int gpu_util(const Gpu::gpu_info& gpu) {
			const auto found = gpu.gpu_percent.find("gpu-totals");
			return gpu.supported_functions.gpu_utilization && found != gpu.gpu_percent.end() && !found->second.empty()
				? static_cast<int>(std::clamp(found->second.back(), 0ll, 100ll)) : -1;
		}

		string process_gpu_ids(const Gpu::process_info& gpu, const size_t gpu_count) {
			string ids;
			std::unordered_set<unsigned> seen;
			for (const auto index : gpu.device_indices) {
				if (index >= gpu_count || !seen.insert(index).second) continue;
				if (!ids.empty()) ids += ',';
				ids += std::to_string(index);
			}
			return ids.empty() ? "--" : ids;
		}

		string type_text(const Gpu::process_info& gpu) {
			if (gpu.compute && gpu.graphics) return "C/G";
			if (gpu.compute) return "C";
			if (gpu.graphics) return "G";
			return "--";
		}

		string fun_text(const vector<Gpu::gpu_info>& gpus) {
			const auto& mode = Config::getS("ml_fun");
			if (mode == "off") return {};
			int max_util = -1;
			uint64_t used = 0;
			bool have_memory = false;
			for (const auto& gpu : gpus) {
				max_util = std::max(max_util, gpu_util(gpu));
				if (gpu.supported_functions.mem_used) {
					used += static_cast<uint64_t>(std::max(0ll, gpu.mem_used));
					have_memory = true;
				}
			}
			const string label = max_util < 0 ? "--" : fmt::format("{}% max", max_util);
			const string fuel = have_memory ? floating_humanizer(used, true) : "--";
			const string load = max_util < 0 ? "--" : fmt::format("[{}] {}", gpu_bar(max_util, 10), label);
			return mode == "cat" ? fmt::format("=^.^= GPU load {} | VRAM {}", load, fuel)
				: fmt::format(">=> GPU thrust {} | VRAM {}", load, fuel);
		}
#endif

		string process_metric(const double value) {
			return fmt::format("{:.1f}", value);
		}

		void put_line(string& out, const int y, const string& text, const int width, const string& color = {}) {
			const int line_width = std::min(width, Term::width.load()) - 1;
			if (y < 1 || y > Term::height || line_width <= 0) return;
			out += Mv::to(y, 1) + color + fit(text, line_width) + Fx::reset;
		}
	}

#if defined(GPU_SUPPORT)
	string draw(const Cpu::cpu_info& cpu, const Mem::mem_info& mem, const Net::net_info& net,
		const vector<Proc::proc_info>& processes, const vector<Gpu::gpu_info>& gpus,
		const bool force_redraw, const bool data_same) {
#else
	string draw(const Cpu::cpu_info& cpu, const Mem::mem_info& mem, const Net::net_info& net,
		const vector<Proc::proc_info>& processes, const bool force_redraw, const bool data_same) {
#endif
		(void)data_same;
		const int width = Term::width.load();
		const int height = Term::height.load();
#if defined(GPU_SUPPORT)
		const auto frame = layout(width, height, gpus.size());
#else
		const auto frame = layout(width, height, 0);
#endif
		if (width <= 1 || height <= 0) return {};
		static int previous_width = -1, previous_height = -1;
		static int previous_gpu_rows = -1, previous_process_header = -1, previous_process_first = -1, previous_process_rows = -1;
		const bool layout_changed = width != previous_width || height != previous_height
			|| frame.gpu_rows != previous_gpu_rows || frame.process_header != previous_process_header
			|| frame.process_first != previous_process_first || frame.process_rows != previous_process_rows;
		previous_width = width;
		previous_height = height;
		previous_gpu_rows = frame.gpu_rows;
		previous_process_header = frame.process_header;
		previous_process_first = frame.process_first;
		previous_process_rows = frame.process_rows;
		string out = force_redraw || layout_changed ? Term::clear : string{};
		put_line(out, 1, fmt::format("BETTERTOP  ML  {}", Global::Version), width, Theme::c("title") + Fx::b);
		put_line(out, 2, host_strip(cpu, mem, net, width - 1), width, Theme::c("main_fg"));
		if (frame.too_small)
			put_line(out, 3, "Compact terminal: optional columns hidden; resize to at least 80x16 for the full view.", width, Theme::c("inactive_fg"));

#if defined(GPU_SUPPORT)
		const bool wide = width >= 100;
		const int gpu_start = frame.gpu_rows > 0
			? std::clamp(Config::getI("ml_gpu_start"), 0, std::max(0, static_cast<int>(gpus.size()) - frame.gpu_rows)) : 0;
		const int gpu_name_width = width >= 100 ? 23 : width >= 80 ? 18 : std::max(5, std::min(13, width - 34));
		string gpu_title = "GPU fleet";
		if (gpus.size() > static_cast<size_t>(frame.gpu_rows) && frame.gpu_rows > 0)
			gpu_title += fmt::format(" [{}-{} / {}]  [ ] page", gpu_start + 1, gpu_start + frame.gpu_rows, gpus.size());
		put_line(out, 4, gpu_title, width, Theme::c("title") + Fx::b);
		put_line(out, frame.gpu_first - 1,
			wide ? "ID MODEL                   UTIL HISTORY   VRAM used/total/free           TEMP POWER   PROC"
			: width >= 80 ? "ID MODEL              UTIL HISTORY   VRAM used/total/free       PROC"
			: "ID MODEL UTIL VRAM", width, Theme::c("main_fg") + Fx::b);
		for (int row = 0; row < frame.gpu_rows; ++row) {
			const size_t index = static_cast<size_t>(gpu_start + row);
			const auto& gpu = gpus.at(index);
			const int util = gpu_util(gpu);
			const auto process_count = std::ranges::count_if(Gpu::gpu_processes, [index](const auto& process) {
				return std::ranges::find(process.device_indices, static_cast<unsigned>(index)) != process.device_indices.end();
			});
				const bool process_count_valid = Gpu::snapshot_state == Gpu::SnapshotState::healthy || Gpu::snapshot_state == Gpu::SnapshotState::stale;
				const string process_count_text = process_count_valid ? std::to_string(process_count) : "--";
			const string name = uresize(device_name(index, gpu), gpu_name_width, true);
			const string vram = uresize(memory_triplet(gpu), 23, true);
			string row_text;
			const string util_bar = gpu_history(gpu, 8);
			const string compact_bar = gpu_history(gpu, 6);
			if (wide) {
				const string temp = gpu.supported_functions.temp_info && !gpu.temp.empty() ? fmt::format("{}C", gpu.temp.back()) : "--";
				const string power = gpu.supported_functions.pwr_usage
					? (gpu.supported_functions.pwr_limit
						? fmt::format("{:.0f}/{:.0f}W", gpu.pwr_usage / 1000.0, gpu.pwr_max_usage / 1000.0)
						: fmt::format("{:.0f}W", gpu.pwr_usage / 1000.0)) : "--";
				row_text = fmt::format("{:>2} {:<{}} {:>4} [{:<8}] {:<23} {:>4} {:>8} {:>4}", index, name, gpu_name_width,
					percent_or_dash(util >= 0, util), util_bar, vram, temp, power, process_count_text);
			} else if (width >= 80) {
				row_text = fmt::format("{:>2} {:<{}} {:>4} [{:<6}] {:<23} {:>4}", index, name, gpu_name_width,
					percent_or_dash(util >= 0, util), compact_bar, vram, process_count_text);
			} else {
				row_text = width < 36
					? fmt::format("{:>2} {:>4} {}", index, percent_or_dash(util >= 0, util), vram)
					: fmt::format("{:>2} {:<{}} {:>4} {}", index, name, gpu_name_width,
						percent_or_dash(util >= 0, util), vram);
			}
			put_line(out, frame.gpu_first + row, row_text, width, Theme::c("main_fg"));
		}
		if (gpus.empty()) put_line(out, frame.gpu_first, "No GPUs discovered", width, Theme::c("inactive_fg"));
		else if (frame.gpu_rows == 0) put_line(out, frame.gpu_first, "GPU rows hidden; resize to see fleet", width, Theme::c("inactive_fg"));
#else
		put_line(out, 4, "GPU fleet unavailable in this build", width, Theme::c("inactive_fg"));
#endif

		const string sorting = Config::getS("ml_sorting");
	#if defined(GPU_SUPPORT)
		const bool gpu_only = Config::getB("ml_gpu_only");
	#else
		const bool gpu_only = false;
	#endif
		const string filter_text = Config::getS("proc_filter");
		vector<ProcessRow> rows;
		std::unordered_map<size_t, const Proc::proc_info*> host_by_pid;
		for (const auto& process : processes) {
			host_by_pid.insert_or_assign(process.pid, &process);
		}
#if defined(GPU_SUPPORT)
		std::unordered_map<size_t, const Gpu::process_info*> gpu_by_pid;
		for (const auto& process : Gpu::gpu_processes) gpu_by_pid.insert_or_assign(process.pid, &process);
#endif
		for (const auto& process : processes) {
#if defined(GPU_SUPPORT)
			auto gpu = gpu_by_pid.find(process.pid);
			if (process.filtered && !(gpu_only && gpu != gpu_by_pid.end())) continue;
			if (gpu_only && gpu == gpu_by_pid.end()) continue;
			if (!filter_text.empty() && !Proc::matches_filter(process, filter_text)) continue;
			rows.push_back({process.pid, &process, gpu == gpu_by_pid.end() ? nullptr : gpu->second});
#else
			if (process.filtered) continue;
			if (!filter_text.empty() && !Proc::matches_filter(process, filter_text)) continue;
			rows.push_back({process.pid, &process});
#endif
		}
#if defined(GPU_SUPPORT)
		for (const auto& process : Gpu::gpu_processes) {
			if (host_by_pid.contains(process.pid)) continue;
			Proc::proc_info invisible{};
			invisible.pid = process.pid;
			invisible.name = "host process unavailable";
			if (!filter_text.empty() && !Proc::matches_filter(invisible, filter_text)) continue;
			rows.push_back({process.pid, nullptr, &process});
		}
#endif

		const auto known_first = [](const auto& left, const auto& right) {
			return left.has_value() != right.has_value() ? left.has_value() : left > right;
		};
		std::ranges::stable_sort(rows, [&](const ProcessRow& left, const ProcessRow& right) {
			if (sorting == "gpu") {
#if defined(GPU_SUPPORT)
				const auto a = left.gpu ? left.gpu->util_percent : std::optional<unsigned>{};
				const auto b = right.gpu ? right.gpu->util_percent : std::optional<unsigned>{};
				if (a != b) return known_first(a, b);
#endif
			} else if (sorting == "cpu") {
				const auto a = left.host ? std::optional<double>{left.host->cpu_p} : std::optional<double>{};
				const auto b = right.host ? std::optional<double>{right.host->cpu_p} : std::optional<double>{};
				if (a != b) return known_first(a, b);
			} else if (sorting == "ram") {
				const auto a = left.host ? std::optional<uint64_t>{left.host->mem} : std::optional<uint64_t>{};
				const auto b = right.host ? std::optional<uint64_t>{right.host->mem} : std::optional<uint64_t>{};
				if (a != b) return known_first(a, b);
			} else {
#if defined(GPU_SUPPORT)
				const auto a = left.gpu ? left.gpu->mem_used : std::optional<uint64_t>{};
				const auto b = right.gpu ? right.gpu->mem_used : std::optional<uint64_t>{};
				if (a != b) return known_first(a, b);
#endif
			}
			return left.pid < right.pid;
		});

		const int process_header = frame.process_header;
		const int process_first = frame.process_first;
		const int process_rows = frame.process_rows;
		const bool detail = Config::getB("show_detailed");
		Config::set("proc_banner_shown", false);
		Proc::select_max = std::max(1, process_rows) + (detail ? 8 : 0);
		Proc::numpids = static_cast<int>(std::min<size_t>(rows.size(), static_cast<size_t>(std::numeric_limits<int>::max())));
		int start = std::clamp(Config::getI("proc_start"), 0, std::max(0, static_cast<int>(rows.size()) - process_rows));
		const int visible_rows = std::min(process_rows, std::max(0, static_cast<int>(rows.size()) - start));
		int selected = std::clamp(Config::getI("proc_selected"), 0, visible_rows);
		if (rows.empty()) start = selected = 0;
		if (Config::getI("proc_start") != start) Config::set("proc_start", start);
		if (Config::getI("proc_selected") != selected) Config::set("proc_selected", selected);
		Proc::start = start;
		Proc::selected = selected;
		Proc::selected_pid = 0;
		Proc::selected_name.clear();
		Proc::selected_depth = 0;
		Proc::shown = process_rows > 0;
		Proc::x = 0;
		Proc::y = process_header;
		Proc::width = width;
		Proc::height = process_rows + 3;
		Proc::scroll_pos = -1;

		string process_title = fmt::format("Processes [{}] | sort {} | {}", gpu_only ? "GPU only" : "all", sorting, filter_text.empty() ? "f filter" : "f " + filter_text);
		if (detail && Config::getI("detailed_pid") > 0) {
			const auto selected_detail = std::ranges::find(rows, static_cast<size_t>(Config::getI("detailed_pid")), &ProcessRow::pid);
			if (selected_detail == rows.end()) {
				process_title = fmt::format("Process detail PID {} | host process unavailable", Config::getI("detailed_pid"));
			} else {
				const auto& entry = *selected_detail;
				process_title = fmt::format("DETAIL PID {} {} | CPU {}% | RAM {} | GPU {} VRAM {} | {} {}",
					entry.pid, entry.host ? entry.host->name : "host process unavailable",
					entry.host ? process_metric(entry.host->cpu_p) : "--",
					entry.host ? floating_humanizer(entry.host->mem, true) : "--",
#if defined(GPU_SUPPORT)
					entry.gpu ? process_gpu_ids(*entry.gpu, gpus.size()) : "--",
					entry.gpu && entry.gpu->mem_used ? floating_humanizer(*entry.gpu->mem_used, true) : "--",
#else
					"--", "--",
#endif
					entry.host && Proc::detailed.last_pid == entry.pid ? Proc::detailed.status : "",
					entry.host && Proc::detailed.last_pid == entry.pid ? Proc::detailed.elapsed : "");
			}
		}
		put_line(out, process_header, process_title, width, Theme::c("title") + Fx::b);
		const bool wide_process = width >= 100;
		const bool normal_process = width >= 80;
		put_line(out, process_header + 1,
			wide_process ? "PID     USER     PROCESS                  CPU%     RAM     VRAM GPU IDs  GPU max% TYPE"
			: normal_process ? "PID   PROCESS                         CPU%     RAM    VRAM GPU IDs GPUmax%"
			: "PID PROCESS       GPU IDs VRAM",
			width, Theme::c("main_fg") + Fx::b);
		for (int row = 0; row < process_rows; ++row) put_line(out, process_first + row, "", width);

		if (process_rows == 0) {
			put_line(out, process_first, "Resize vertically to show process rows.", width, Theme::c("inactive_fg"));
		} else if (gpu_only && rows.empty()) {
			put_line(out, process_first, "No GPU process rows; press g for all host processes.", width, Theme::c("inactive_fg"));
		} else {
			for (int row = 0; row < process_rows; ++row) {
				const size_t index = static_cast<size_t>(start + row);
				if (index >= rows.size()) break;
				const auto& entry = rows[index];
				const bool is_selected = selected == row + 1;
				if (is_selected) {
					Proc::selected_pid = static_cast<int>(entry.pid);
					Proc::selected_name = entry.host ? entry.host->name : "host process unavailable";
					Proc::selected_depth = entry.host ? static_cast<int>(entry.host->depth) : 0;
				}
				const string pid = std::to_string(entry.pid);
				const string name = entry.host ? (entry.host->prefix + (entry.host->cmd.empty() ? entry.host->name : entry.host->cmd)) : "host process unavailable";
				const string user = entry.host ? entry.host->user : "--";
				const string cpu_text = entry.host ? process_metric(entry.host->cpu_p) : "--";
				const string ram = entry.host ? floating_humanizer(entry.host->mem, true) : "--";
#if defined(GPU_SUPPORT)
				const string gpu_vram = entry.gpu && entry.gpu->mem_used ? floating_humanizer(*entry.gpu->mem_used, true) : "--";
				const string gpu_ids = entry.gpu ? process_gpu_ids(*entry.gpu, gpus.size()) : "--";
				const string gpu_util_text = entry.gpu && entry.gpu->util_percent ? fmt::format("{}%", std::min(100u, *entry.gpu->util_percent)) : "--";
				const string gpu_type = entry.gpu ? type_text(*entry.gpu) : "--";
#else
				const string gpu_vram = "--", gpu_ids = "--", gpu_util_text = "--", gpu_type = "--";
#endif
				string line;
				const string cpu_cell = cpu_text == "--" ? "--" : cpu_text + "%";
				if (wide_process) {
					line = fmt::format("{:<7} {:<8} {:<24} {:>6} {:>8} {:>8} {:<8} {:>7} {:<3}",
						pid, uresize(user, 8), uresize(name, 24, true), cpu_cell,
						ram, gpu_vram, gpu_ids, gpu_util_text, gpu_type);
				} else if (normal_process) {
					line = fmt::format("{:<5} {:<30} {:>6} {:>8} {:>8} {:<7} {:>5}",
						pid, uresize(name, 30, true), cpu_cell, ram, gpu_vram, gpu_ids, gpu_util_text);
				} else {
					line = fmt::format("{:<5} {:<13} {:<7} {:>8}", pid, uresize(name, 13, true), gpu_ids, gpu_vram);
				}
				put_line(out, process_first + row, line, width,
					is_selected ? Theme::c("selected_bg") + Theme::c("selected_fg") + Fx::b : Theme::c("main_fg"));
			}
		}

#if defined(GPU_SUPPORT)
		#if defined(__linux__)
		const string retry_hint = "r retry ";
		const string small_help = "r retry q quit v";
		#else
		const string retry_hint;
		const string small_help = "q quit v";
		#endif
		const string gpu_status = frame.too_small ? uresize(snapshot_text(), 8, true) : snapshot_text();
		put_line(out, frame.fun_row, fun_text(gpus), width, Theme::c("hi_fg"));
		put_line(out, frame.footer, frame.too_small
			? fmt::format("GPU {} | resize 80x16 | {}", gpu_status, small_help)
			: fmt::format("GPU {} | ↑↓sel Enter info f filter g all {}←→sort []GPU v classic x fun q",
				gpu_status, retry_hint), width, Theme::c("inactive_fg"));
#else
		put_line(out, frame.fun_row, {}, width);
		put_line(out, frame.footer, "Host only | ↑↓ select  Enter detail  f filter  v classic  q quit", width, Theme::c("inactive_fg"));
#endif
		return out + Fx::reset;
	}
}
