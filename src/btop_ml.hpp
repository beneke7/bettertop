/* Copyright 2026 BetterTop contributors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0
*/

#pragma once

#include "btop_shared.hpp"

namespace Ml {
	struct Layout {
		int gpu_first{};
		int gpu_rows{};
		int process_header{};
		int process_first{};
		int process_rows{};
		int fun_row{};
		int footer{};
		bool too_small{};
	};

	Layout layout(int width, int height, size_t gpu_count);
	bool self_test();

#if defined(GPU_SUPPORT)
	string draw(const Cpu::cpu_info& cpu, const Mem::mem_info& mem, const Net::net_info& net,
		const vector<Proc::proc_info>& processes, const vector<Gpu::gpu_info>& gpus,
		bool force_redraw = false, bool data_same = false);
#else
	string draw(const Cpu::cpu_info& cpu, const Mem::mem_info& mem, const Net::net_info& net,
		const vector<Proc::proc_info>& processes, bool force_redraw = false, bool data_same = false);
#endif
}
