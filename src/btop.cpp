/* Copyright 2021 Aristocratos (jakob@qvantnet.com)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

indent = tab
tab-size = 4
*/

#include "btop.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <clocale>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <span>
#include <string_view>
#ifdef __FreeBSD__
	#include <pthread_np.h>
#endif
#include <thread>
#include <numeric>
#include <ranges>
#include <unistd.h>
#include <cmath>
#include <iostream>
#include <exception>
#include <tuple>
#include <regex>
#include <chrono>
#include <utility>
#include <semaphore>
#include <signal.h>
#include <time.h>
#if defined(__linux__)
	#include <sys/utsname.h>
#endif

#ifdef __APPLE__
	#include <CoreFoundation/CoreFoundation.h>
	#include <mach-o/dyld.h>
	#include <limits.h>
#endif

#ifdef __NetBSD__
	#include <sys/param.h>
	#include <sys/sysctl.h>
	#include <unistd.h>
#endif

#include <fmt/core.h>
#include <fmt/ostream.h>

#include "btop_cli.hpp"
#include "btop_config.hpp"
#include "btop_draw.hpp"
#include "btop_input.hpp"
#include "btop_log.hpp"
#include "btop_menu.hpp"
#include "btop_ml.hpp"
#include "btop_shared.hpp"
#include "btop_theme.hpp"
#include "btop_tools.hpp"
#if defined(__linux__) && defined(GPU_SUPPORT)
	#include "linux/btop_gpu_bridge.hpp"
#endif

using std::atomic;
using std::min;
using std::string;
using std::string_view;
using std::to_string;
using std::vector;

namespace fs = std::filesystem;

using namespace Tools;
using namespace std::chrono_literals;
using namespace std::literals;

namespace {
	volatile sig_atomic_t pending_quit_signal{};
	volatile sig_atomic_t pending_quit_status{};
	volatile sig_atomic_t pending_sleep_signal{};
	volatile sig_atomic_t pending_resize_signal{};
	volatile sig_atomic_t pending_reload_signal{};
#if defined(GPU_SUPPORT) && defined(__linux__)
	bool gpu_bridge_started{};
#endif

	constexpr array fatal_signals{SIGSEGV, SIGABRT, SIGTRAP, SIGBUS, SIGILL, SIGFPE};
	array<struct sigaction, fatal_signals.size()> previous_fatal_actions{};
}

namespace Global {
	const vector<array<string, 2>> Banner_src = {
		{"#E62525", "██████╗ ████████╗ ██████╗ ██████╗"},
		{"#CD2121", "██╔══██╗╚══██╔══╝██╔═══██╗██╔══██╗   ██╗    ██╗"},
		{"#B31D1D", "██████╔╝   ██║   ██║   ██║██████╔╝ ██████╗██████╗"},
		{"#9A1919", "██╔══██╗   ██║   ██║   ██║██╔═══╝  ╚═██╔═╝╚═██╔═╝"},
		{"#801414", "██████╔╝   ██║   ╚██████╔╝██║        ╚═╝    ╚═╝"},
		{"#000000", "╚═════╝    ╚═╝    ╚═════╝ ╚═╝"},
	};
	const string Version = "0.1.11-debug";

	int coreCount;
	string overlay;
	string clock;

	string fg_white = "\x1b[1;97m";
	string fg_green = "\x1b[1;92m";
	string fg_red = "\x1b[1;91m";

	uid_t real_uid, set_uid;

	fs::path self_path;

	string exit_error_msg;
	atomic<bool> thread_exception (false);

	bool debug{};

	uint64_t start_time;

	atomic<bool> resized (false);
	atomic<bool> quitting (false);
	atomic<bool> should_quit (false);
	atomic<bool> should_sleep (false);
	atomic<bool> _runner_started (false);
	atomic<bool> init_conf (false);
	atomic<bool> reload_conf (false);
}

namespace Runner {
	static pthread_t runner_id;
	void request_stop();
	bool join_for(std::chrono::seconds timeout);
} // namespace Runner

//* Handler for SIGWINCH and general resizing events, does nothing if terminal hasn't been resized unless force=true
void term_resize(bool force) {
	static atomic<bool> resizing (false);
	if (Input::polling) {
		Global::resized = true;
		Input::interrupt();
		return;
	}
	atomic_lock lck(resizing, true);
	if (auto refreshed = Term::refresh(true); refreshed or force) {
		if (force and refreshed) force = false;
	}
	else return;
#ifdef GPU_SUPPORT
	static const array<string, 10> all_boxes = {"gpu5", "cpu", "mem", "net", "proc", "gpu0", "gpu1", "gpu2", "gpu3", "gpu4"};
#else
	static const array<string, 5> all_boxes = {"", "cpu", "mem", "net", "proc"};
#endif
	Global::resized = true;
	if (Runner::active) Runner::stop();
	Term::refresh();
	Config::unlock();

	auto boxes = Config::getS("shown_boxes");
	auto min_size = Config::getB("ml_view") ? array<int, 2>{0, 0} : Term::get_min_size(boxes);
	auto minWidth = min_size.at(0), minHeight = min_size.at(1);

	while (not force or (Term::width < minWidth or Term::height < minHeight)) {
		sleep_ms(100);
		if (Term::width < minWidth or Term::height < minHeight) {
			int width = Term::width, height = Term::height;
			Term::output(fmt::format("{clear}{fg_white}"
					"{mv1}Terminal size too small:"
					"{mv2} Width = {fg_width}{width} {fg_white}Height = {fg_height}{height}"
					"{mv3}{fg_white}Needed for current config:"
					"{mv4}Width = {minWidth} Height = {minHeight}",
					"clear"_a = Term::clear, "fg_white"_a = Global::fg_white,
					"mv1"_a = Mv::to((height / 2) - 2, (width / 2) - 11),
					"mv2"_a = Mv::to((height / 2) - 1, (width / 2) - 10),
						"fg_width"_a = (width < minWidth ? Global::fg_red : Global::fg_green),
						"width"_a = width,
						"fg_height"_a = (height < minHeight ? Global::fg_red : Global::fg_green),
						"height"_a = height,
					"mv3"_a = Mv::to((height / 2) + 1, (width / 2) - 12),
					"mv4"_a = Mv::to((height / 2) + 2, (width / 2) - 10),
						"minWidth"_a = minWidth,
						"minHeight"_a = minHeight
			));

			bool got_key = false;
			for (; not Term::refresh() and not got_key; got_key = Input::poll(10));
			if (got_key) {
				auto key = Input::get();
				if (key == "q")
					clean_quit(0);
				else if (key.size() == 1 and isint(key)) {
					auto intKey = stoi(key);
				#ifdef GPU_SUPPORT
					if ((intKey == 0 and Gpu::count >= 5) or (intKey >= 5 and intKey - 4 <= Gpu::count)) {
				#else
					if (intKey > 0 and intKey < 5) {
				#endif
						const auto& box = all_boxes.at(intKey);
						Config::current_preset.reset();
						Config::toggle_box(box);
						boxes = Config::getS("shown_boxes");
					}
				}
			}
			min_size = Term::get_min_size(boxes);
			minWidth = min_size.at(0);
			minHeight = min_size.at(1);
		}
		else if (not Term::refresh()) break;
	}

	Input::interrupt();
}

//* Exit handler; stops threads, restores terminal and saves config changes
void clean_quit(int sig) {
	if (Global::quitting) return;
	Global::quitting = true;
	Runner::request_stop();
	const bool restored = Term::restore();
	if (Global::_runner_started) {
		const auto joined = Runner::join_for(5s);
		if (not joined) _Exit(sig != -1 ? sig : 0);
	}
	if (not restored) Term::restore();

#if defined(GPU_SUPPORT) && defined(__linux__)
	if (gpu_bridge_started) Gpu::Bridge::shutdown();
#endif

#ifdef GPU_SUPPORT
	Gpu::Asysfs::shutdown();
	#ifdef __APPLE__
	Gpu::AppleSilicon::shutdown();
	#endif
#endif


	if (Config::getB("save_config_on_exit")) {
		Config::write();
	}

	Input::clear();

	if (not Global::exit_error_msg.empty()) {
		sig = 1;
		Logger::error("{}", Global::exit_error_msg);
		fmt::println(std::cerr, "ERROR: {}", Global::exit_error_msg);
	}
	Logger::info("Quitting! Runtime: {}", sec_to_dhms(time_s() - Global::start_time));

	const auto excode = (sig != -1 ? sig : 0);

#if defined __APPLE__ || defined __OpenBSD__ || defined __NetBSD__
	_Exit(excode);
#else
	quick_exit(excode);
#endif
}

//* Handler for SIGTSTP; stops threads, restores terminal and sends SIGSTOP
static void _sleep() {
	Runner::stop();
	if (not Term::restore()) clean_quit(1);
	std::raise(SIGSTOP);
	if (not Term::init()) {
		Global::exit_error_msg = "Failed to restore terminal after resume.";
		clean_quit(1);
	}
	term_resize(true);
}

static void _exit_handler() {
	clean_quit(-1);
}

static void _crash_handler(const int sig) {
	Term::emergency_restore();
	for (size_t i = 0; i < fatal_signals.size(); ++i) {
		if (fatal_signals[i] == sig) {
			::sigaction(sig, &previous_fatal_actions[i], nullptr);
			break;
		}
	}
	::raise(sig);
}

static void _signal_handler(const int sig) {
	switch (sig) {
		case SIGINT:
		case SIGTERM:
		case SIGHUP:
		case SIGQUIT:
			pending_quit_signal = sig;
			break;
		case SIGTSTP:
			pending_sleep_signal = 1;
			break;
		case SIGCONT:
			break;
		case SIGWINCH:
			pending_resize_signal = 1;
			break;
		case SIGUSR1:
			// Input::poll interrupt
			break;
		case SIGUSR2:
			pending_reload_signal = 1;
			break;
	}
	if (sig != SIGUSR1) ::kill(::getpid(), SIGUSR1);
}

static void _install_signal_handlers() {
	std::atexit(_exit_handler);
	auto install = [](const int sig, void (*handler)(int), const int flags = 0, struct sigaction* previous = nullptr) {
		struct sigaction action{};
		action.sa_handler = handler;
		action.sa_flags = flags;
		sigemptyset(&action.sa_mask);
		::sigaction(sig, &action, previous);
	};
	for (const auto sig : {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP, SIGCONT, SIGWINCH, SIGUSR1, SIGUSR2})
		install(sig, _signal_handler);
	for (size_t i = 0; i < fatal_signals.size(); ++i)
		install(fatal_signals[i], _crash_handler, 0, &previous_fatal_actions[i]);
}

static void _dispatch_signal_flags() {
	sigset_t handled, previous;
	sigemptyset(&handled);
	for (const auto sig : {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP, SIGCONT, SIGWINCH, SIGUSR2})
		sigaddset(&handled, sig);
	pthread_sigmask(SIG_BLOCK, &handled, &previous);
	const sig_atomic_t quit = pending_quit_signal;
	const bool sleep = pending_sleep_signal;
	const bool resize = pending_resize_signal;
	const bool reload = pending_reload_signal;
	pending_quit_signal = 0;
	pending_sleep_signal = 0;
	pending_resize_signal = 0;
	pending_reload_signal = 0;
	pthread_sigmask(SIG_SETMASK, &previous, nullptr);
	if (quit) {
		pending_quit_status = 128 + quit;
		Global::should_quit = true;
	}
	if (sleep) Global::should_sleep = true;
	if (resize) Global::resized = true;
	if (reload) Global::reload_conf = true;
}

//* Config init
void init_config(bool low_color, std::optional<std::string>& filter) {
	atomic_lock lck(Global::init_conf);
	vector<string> load_warnings;
	Config::load(Config::conf_file, load_warnings);
	Config::set("lowcolor", (low_color ? true : not Config::getB("truecolor")));

	static bool first_init = true;

	if (Global::debug and first_init) {
		Logger::set_log_level(Logger::Level::DEBUG);
		Logger::debug("Running in DEBUG mode!");
	}
	else Logger::set_log_level(Config::getS("log_level"));

	if (filter.has_value()) {
		Config::set("proc_filter", filter.value());
	}

	static string log_level;
	if (const string current_level = Config::getS("log_level"); log_level != current_level) {
		log_level = current_level;
		Logger::info("Logger set to {}", (Global::debug ? "DEBUG" : log_level));
	}

	for (const auto& err_str : load_warnings) Logger::warning("{}", err_str);
	first_init = false;
}

//* Manages secondary thread for collection and drawing of boxes
namespace Runner {
	atomic_waiting_lock active;
	atomic<bool> stopping (false);
	atomic<bool> waiting (false);
	atomic<bool> redraw (false);
	atomic<bool> coreNum_reset (false);
	atomic<const char*> phase{"waiting"};

	static void set_phase(const char* value) noexcept {
		phase.store(value, std::memory_order_relaxed);
	}

	static inline auto set_active(bool value) noexcept {
		active.store(value);
	}

	//* Setup semaphore for triggering thread to do work
	// TODO: This can be made a local without too much effort.
	std::counting_semaphore<2> do_work { 0 };
	inline void thread_wait() { do_work.acquire(); }
	inline void thread_trigger() { do_work.release(); }
	std::binary_semaphore runner_exited { 0 };

	//* Wrapper for raising privileges when using SUID bit
	class gain_priv {
		int status = -1;
	public:
		gain_priv() {
			if (Global::real_uid != Global::set_uid)
				this->status = seteuid(Global::set_uid);
		}
		~gain_priv() noexcept {
			if (status == 0)
				status = seteuid(Global::real_uid);
		}
		gain_priv(const gain_priv& other) = delete;
		gain_priv& operator=(const gain_priv& other) = delete;
		gain_priv(gain_priv&& other) = delete;
		gain_priv& operator=(gain_priv&& other) = delete;
	};

	string output;
	string empty_bg;
	bool pause_output{};
	sigset_t mask;
	std::mutex mtx;

	enum debug_actions {
		collect_begin,
		collect_done,
		draw_begin,
		draw_begin_only,
		draw_done
	};

	enum debug_array {
		collect,
		draw
	};

	string debug_bg;
	std::unordered_map<string, array<uint64_t, 2>> debug_times;

	class MyNumPunct : public std::numpunct<char>
	{
	protected:
		virtual char do_thousands_sep() const override { return '\''; }
		virtual std::string do_grouping() const override { return "\03"; }
	};


	struct runner_conf {
		vector<string> boxes;
		bool no_update;
		bool force_redraw;
		bool background_update;
		string overlay;
		string clock;
	};

	struct runner_conf current_conf;

	static void debug_timer(const char* name, const int action) {
		switch (action) {
			case collect_begin:
				debug_times[name].at(collect) = time_micros();
				return;
			case collect_done:
				debug_times[name].at(collect) = time_micros() - debug_times[name].at(collect);
				debug_times["total"].at(collect) += debug_times[name].at(collect);
				return;
			case draw_begin_only:
				debug_times[name].at(draw) = time_micros();
				return;
			case draw_begin:
				debug_times[name].at(draw) = time_micros();
				debug_times[name].at(collect) = debug_times[name].at(draw) - debug_times[name].at(collect);
				debug_times["total"].at(collect) += debug_times[name].at(collect);
				return;
			case draw_done:
				debug_times[name].at(draw) = time_micros() - debug_times[name].at(draw);
				debug_times["total"].at(draw) += debug_times[name].at(draw);
				return;
		}
	}

	//? ------------------------------- Secondary thread: async launcher and drawing ----------------------------------
	static void * _runner(void *) {
		//? Block some signals in this thread to avoid deadlock from any signal handlers trying to stop this thread
		sigemptyset(&mask);
		sigaddset(&mask, SIGINT);
		sigaddset(&mask, SIGTERM);
		sigaddset(&mask, SIGHUP);
		sigaddset(&mask, SIGQUIT);
		sigaddset(&mask, SIGTSTP);
		sigaddset(&mask, SIGCONT);
		sigaddset(&mask, SIGWINCH);
		sigaddset(&mask, SIGUSR1);
		sigaddset(&mask, SIGUSR2);
		pthread_sigmask(SIG_BLOCK, &mask, nullptr);

		// TODO: On first glance it looks redudant with `Runner::active`.
		std::lock_guard lock {mtx};

		//* ----------------------------------------------- THREAD LOOP -----------------------------------------------
		while (not Global::quitting) {
			thread_wait();
			atomic_wait_for(active, true, 30'000);
			if (active) {
				Global::exit_error_msg = "Runner thread failed to get active lock (30s)!";
				Global::thread_exception = true;
				Input::interrupt();
				stopping = true;
			}
			if (stopping or Global::resized) {
				sleep_ms(1);
				continue;
			}

			//? Atomic lock used for blocking non thread-safe actions in main thread
			auto lck = active.lock();
			set_phase("starting");

			//? Set effective user if SUID bit is set
			gain_priv powers{};

			auto& conf = current_conf;
			const bool ml_view = Config::getB("ml_view");

			//! DEBUG stats
			if (Global::debug and not ml_view) {
                if (debug_bg.empty() or redraw)
                    Runner::debug_bg = Draw::createBox(2, 2, 33,
					#ifdef GPU_SUPPORT
						9,
					#else
						8,
					#endif
					"", true, "μs");

				debug_times.clear();
				debug_times["total"] = {0, 0};
			}

			output.clear();

			//* Run collection and draw functions for all boxes
			try {
				if (ml_view) {
					set_phase("CPU collect");
					auto& cpu = Cpu::collect(conf.no_update);
					if (coreNum_reset) {
						coreNum_reset = false;
						Cpu::core_mapping = Cpu::get_core_mapping();
						Global::resized = true;
						Input::interrupt();
						continue;
					}
					set_phase("memory collect");
					auto& mem = Mem::collect(conf.no_update);
					set_phase("network collect");
					auto& net = Net::collect(conf.no_update);
					set_phase("process collect");
					auto& processes = Proc::collect(conf.no_update);
#if defined(GPU_SUPPORT)
					set_phase("GPU collect");
					auto& gpus = Gpu::collect(conf.no_update);
					if (not pause_output) {
						set_phase("ML draw");
						output += Ml::draw(cpu, mem, net, processes, gpus, conf.force_redraw, conf.no_update);
					}
#else
					if (not pause_output) {
						set_phase("ML draw");
						output += Ml::draw(cpu, mem, net, processes, conf.force_redraw, conf.no_update);
					}
#endif
				} else {
#if defined(GPU_SUPPORT)
				//? GPU data collection
				const bool gpu_in_cpu_panel = Gpu::gpu_names.size() > 0 and (
					Config::getS("cpu_graph_lower").starts_with("gpu-")
					or (Config::getS("cpu_graph_lower") == "Auto")
					or Config::getS("cpu_graph_upper").starts_with("gpu-")
					or (Gpu::shown == 0 and Config::getS("show_gpu_info") != "Off")
				);

				vector<unsigned int> gpu_panels = {};
				for (auto& box : conf.boxes)
					if (box.starts_with("gpu"))
						gpu_panels.push_back(box.back()-'0');

				vector<Gpu::gpu_info> gpus;
#if defined(__linux__)
				constexpr bool collect_gpu = true;
#else
				const bool collect_gpu = gpu_in_cpu_panel or not gpu_panels.empty();
#endif
				if (collect_gpu) {
					set_phase("GPU collect");
					if (Global::debug) debug_timer("gpu", collect_begin);
					gpus = Gpu::collect(conf.no_update);
					if (Global::debug) debug_timer("gpu", collect_done);
					if (Global::resized) {
						Input::interrupt();
						continue;
					}
				}
				auto& gpus_ref = gpus;
#endif // GPU_SUPPORT

				//? CPU
				if (v_contains(conf.boxes, "cpu")) {
					try {
						if (Global::debug) debug_timer("cpu", collect_begin);

						//? Start collect
						set_phase("CPU collect");
						auto cpu = Cpu::collect(conf.no_update);

						if (coreNum_reset) {
							coreNum_reset = false;
							Cpu::core_mapping = Cpu::get_core_mapping();
							Global::resized = true;
							Input::interrupt();
							continue;
						}

						if (Global::debug) debug_timer("cpu", draw_begin);

						//? Draw box
						if (not pause_output) {
							set_phase("CPU draw");
							output += Cpu::draw(
								cpu,
#if defined(GPU_SUPPORT)
								gpus_ref,
#endif // GPU_SUPPORT
								conf.force_redraw,
								conf.no_update
							);
						}

						if (Global::debug) debug_timer("cpu", draw_done);
					}
					catch (const std::exception& e) {
						throw std::runtime_error("Cpu:: -> " + string{e.what()});
					}
				}
			#ifdef GPU_SUPPORT
				//? GPU
				if (not gpu_panels.empty() and not gpus_ref.empty()) {
					try {
						if (Global::debug) debug_timer("gpu", draw_begin_only);

						//? Draw box
						if (not pause_output) {
							set_phase("GPU draw");
							for (unsigned long i = 0; i < gpu_panels.size(); ++i)
								output += Gpu::draw(gpus_ref[gpu_panels[i]], i, conf.force_redraw, conf.no_update);
						}

						if (Global::debug) debug_timer("gpu", draw_done);
					}
					catch (const std::exception& e) {
                        throw std::runtime_error("Gpu:: -> " + string{e.what()});
					}
				}
			#endif
				//? MEM
				if (v_contains(conf.boxes, "mem")) {
					try {
						if (Global::debug) debug_timer("mem", collect_begin);

						//? Start collect
						set_phase("memory collect");
						auto mem = Mem::collect(conf.no_update);

						if (Global::debug) debug_timer("mem", draw_begin);

						//? Draw box
						if (not pause_output) {
							set_phase("memory draw");
							output += Mem::draw(mem, conf.force_redraw, conf.no_update);
						}

						if (Global::debug) debug_timer("mem", draw_done);
					}
					catch (const std::exception& e) {
						throw std::runtime_error("Mem:: -> " + string{e.what()});
					}
				}

				//? NET
				if (v_contains(conf.boxes, "net")) {
					try {
						if (Global::debug) debug_timer("net", collect_begin);

						//? Start collect
						set_phase("network collect");
						auto net = Net::collect(conf.no_update);

						if (Global::debug) debug_timer("net", draw_begin);

						//? Draw box
						if (not pause_output) {
							set_phase("network draw");
							output += Net::draw(net, conf.force_redraw, conf.no_update);
						}

						if (Global::debug) debug_timer("net", draw_done);
					}
					catch (const std::exception& e) {
						throw std::runtime_error("Net:: -> " + string{e.what()});
					}
				}

				//? PROC
				if (v_contains(conf.boxes, "proc")) {
					try {
						if (Global::debug) debug_timer("proc", collect_begin);

						//? Start collect
						set_phase("process collect");
						auto proc = Proc::collect(conf.no_update);

						if (Global::debug) debug_timer("proc", draw_begin);

						//? Draw box
						if (not pause_output) {
							set_phase("process draw");
							output += Proc::draw(proc, conf.force_redraw, conf.no_update);
						}

						if (Global::debug) debug_timer("proc", draw_done);
					}
					catch (const std::exception& e) {
						throw std::runtime_error("Proc:: -> " + string{e.what()});
					}
				}
				}

			}
			catch (const std::exception& e) {
				Global::exit_error_msg = fmt::format("Exception in runner thread -> {}", e.what());
				Global::thread_exception = true;
				Input::interrupt();
				stopping = true;
			}

			if (stopping) {
				continue;
			}

			if (redraw or conf.force_redraw) {
				empty_bg.clear();
				redraw = false;
			}

			if (not ml_view and not pause_output) output += conf.clock;
			if (not conf.overlay.empty() and not conf.background_update) pause_output = true;
			if (not ml_view and output.empty() and not pause_output) {
				if (empty_bg.empty()) {
					const int x = Term::width / 2 - 10, y = Term::height / 2 - 10;
					output += Term::clear;
					empty_bg = fmt::format(
						"{banner}"
						"{mv1}{titleFg}{b}No boxes shown!"
						"{mv2}{hiFg}1 {mainFg}| Show CPU box"
						"{mv3}{hiFg}2 {mainFg}| Show MEM box"
						"{mv4}{hiFg}3 {mainFg}| Show NET box"
						"{mv5}{hiFg}4 {mainFg}| Show PROC box"
						"{mv6}{hiFg}5-0 {mainFg}| Show GPU boxes"
						"{mv7}{hiFg}esc {mainFg}| Show menu"
						"{mv8}{hiFg}q {mainFg}| Quit",
						"banner"_a = Draw::banner_gen(y, 0, true),
						"titleFg"_a = Theme::c("title"), "b"_a = Fx::b, "hiFg"_a = Theme::c("hi_fg"), "mainFg"_a = Theme::c("main_fg"),
						"mv1"_a = Mv::to(y+6, x),
						"mv2"_a = Mv::to(y+8, x),
						"mv3"_a = Mv::to(y+9, x),
						"mv4"_a = Mv::to(y+10, x),
						"mv5"_a = Mv::to(y+11, x),
						"mv6"_a = Mv::to(y+12, x-2),
						"mv7"_a = Mv::to(y+13, x-2),
						"mv8"_a = Mv::to(y+14, x)
					);
				}
				output += empty_bg;
			}

			//! DEBUG stats -->
			if (Global::debug and not ml_view and not Menu::active) {
				output += fmt::format("{pre}{box:5.5} {collect:>12.12} {draw:>12.12}{post}",
					"pre"_a = debug_bg + Theme::c("title") + Fx::b,
					"box"_a = "box", "collect"_a = "collect", "draw"_a = "draw",
					"post"_a = Theme::c("main_fg") + Fx::ub
				);
				static auto loc = std::locale(std::locale::classic(), new MyNumPunct);
			#ifdef GPU_SUPPORT
				for (const string name : {"cpu", "mem", "net", "proc", "gpu", "total"}) {
			#else
				for (const string name : {"cpu", "mem", "net", "proc", "total"}) {
			#endif
					if (not debug_times.contains(name)) debug_times[name] = {0,0};
					const auto& [time_collect, time_draw] = debug_times.at(name);
					if (name == "total") output += Fx::b;
					output += fmt::format(loc, "{mvLD}{name:5.5} {collect:12L} {draw:12L}",
						"mvLD"_a = Mv::l(31) + Mv::d(1),
						"name"_a = name,
						"collect"_a = time_collect,
						"draw"_a = time_draw
					);
				}
			}

			//? If overlay isn't empty, print output without color and then print overlay on top
			const bool term_sync = Config::getB("terminal_sync");
			set_phase("terminal output");
			Term::output((term_sync ? Term::sync_start : "") + (conf.overlay.empty()
					? output
					: (output.empty() ? "" : Fx::ub + Theme::c("inactive_fg") + Fx::uncolor(output)) + conf.overlay)
				+ (term_sync ? Term::sync_end : ""));
			set_phase("idle");
		}
		//* ----------------------------------------------- THREAD LOOP -----------------------------------------------
		runner_exited.release();
		return {};
	}
	//? ------------------------------------------ Secondary thread end -----------------------------------------------

	void request_stop() {
		stopping = true;
		thread_trigger();
	}

	bool join_for(const std::chrono::seconds timeout) {
#if defined(__linux__)
		timespec deadline{};
		if (::clock_gettime(CLOCK_REALTIME, &deadline) != 0) return false;
		deadline.tv_sec += timeout.count();
		return pthread_timedjoin_np(runner_id, nullptr, &deadline) == 0;
#else
		if (not runner_exited.try_acquire_for(timeout)) return false;
		return pthread_join(runner_id, nullptr) == 0;
#endif
	}

	//* Runs collect and draw in a secondary thread, unlocks and locks config to update cached values
	void run(const string& box, bool no_update, bool force_redraw) {
		atomic_wait_for(active, true, 10'000);
		if (active) {
			Logger::warning("Runner thread slow (>10s) during {}, waiting up to 30s...", phase.load(std::memory_order_relaxed));
			atomic_wait_for(active, true, 20'000);
		}
		if (active) {
			Global::exit_error_msg = fmt::format("Runner thread stalled for 30s during {}, exiting.", phase.load(std::memory_order_relaxed));
			clean_quit(1);
		}
		if (stopping or Global::resized) return;

		if (box == "overlay") {
			const bool term_sync = Config::getB("terminal_sync");
			Term::output((term_sync ? Term::sync_start : "") + Global::overlay + (term_sync ? Term::sync_end : ""));
		}
		else if (box == "clock") {
			const bool term_sync = Config::getB("terminal_sync");
			Term::output((term_sync ? Term::sync_start : "") + Global::clock + (term_sync ? Term::sync_end : ""));
		}
		else {
			Config::unlock();
			Config::lock();

			current_conf = {
				(box == "all" ? Config::current_boxes : vector{box}),
				no_update, force_redraw,
				(not Config::getB("tty_mode") and Config::getB("background_update")),
				Global::overlay,
				Global::clock
			};

			if (Menu::active and not current_conf.background_update) Global::overlay.clear();

			thread_trigger();
			atomic_wait_for(active, false, 10);
		}


	}

	//* Stops any work being done in runner thread and checks for thread errors
	void stop() {
		stopping = true;
		auto lock = std::unique_lock {mtx, std::defer_lock};
		const auto is_runner_busy = !lock.try_lock();
		if (!is_runner_busy and not Global::quitting) {
			if (active) {
				set_active(false);
			}
			Global::exit_error_msg = "Runner thread died unexpectedly!";
			clean_quit(1);
		} else if (is_runner_busy) {
			atomic_wait_for(active, true, 30'000);
			if (active) {
				set_active(false);
				if (Global::quitting) {
					return;
				}
				else {
					Global::exit_error_msg = "No response from Runner thread (30s), quitting!";
					clean_quit(1);
				}
			}
			thread_trigger();
			atomic_wait_for(active, false, 100);
			atomic_wait_for(active, true, 100);
		}
		stopping = false;
	}

}

static auto configure_tty_mode(std::optional<bool> force_tty) {
	if (force_tty.has_value()) {
		Config::set("tty_mode", force_tty.value());
		Logger::debug("TTY mode set via command line");
	}
	else if (Config::getB("force_tty")) {
		Config::set("tty_mode", true);
		Logger::debug("TTY mode set via config");
	}

#if !defined(__APPLE__) && !defined(__OpenBSD__) && !defined(__NetBSD__)
	else if (Term::current_tty.starts_with("/dev/tty")) {
		Config::set("tty_mode", true);
		Logger::debug("Auto detect real TTY");
	}
#endif

	Logger::debug("TTY mode enabled: {}", Config::getB("tty_mode"));
}

#if defined(__linux__) && defined(GPU_SUPPORT)
static auto diagnose_gpu() -> int {
	utsname os{};
	if (uname(&os) == 0) fmt::println("OS: {} {} ({})", os.sysname, os.release, os.machine);
	else fmt::println("OS: Linux");
	fmt::println("BetterTop: {}", Global::Version);

	Config::set("shown_gpus", string{"nvidia"});
	Gpu::Bridge::start();
	const auto deadline = std::chrono::steady_clock::now() + 6s;
	while (std::chrono::steady_clock::now() < deadline) {
		const auto& current = Gpu::Bridge::poll();
		if (current.has_sample || current.state == Gpu::Bridge::State::unavailable) break;
		std::this_thread::sleep_for(50ms);
	}
	const auto snapshot = Gpu::Bridge::poll();
	Gpu::Bridge::shutdown();

	const auto state_name = [&] {
		switch (snapshot.state) {
			case Gpu::Bridge::State::starting: return "starting";
			case Gpu::Bridge::State::healthy: return "healthy";
			case Gpu::Bridge::State::stale: return "stale";
			case Gpu::Bridge::State::unavailable: return "unavailable";
			case Gpu::Bridge::State::disabled: return "disabled";
		}
		return "unknown";
	}();
	const auto helper_status = [&] {
		switch (snapshot.helper_status) {
			case BETTERTOP_GPU_STATUS_OK: return "ok";
			case BETTERTOP_GPU_STATUS_UNAVAILABLE: return "unavailable";
			case BETTERTOP_GPU_STATUS_ERROR: return "error";
		}
		return "unknown";
	}();
	fmt::println("Worker: {} (status={}, detail={}), sample age: {} ms", state_name, helper_status,
				 snapshot.status_detail, snapshot.age_ms);
	if (snapshot.has_sample) {
		fmt::println("Inventory: {} NVIDIA GPU(s)", snapshot.devices.size());
		for (const auto& device : snapshot.devices) {
			fmt::println("  [{}] {} | {} | {}", device.display_index, replace_ascii_control(device.name),
						 replace_ascii_control(device.uuid), replace_ascii_control(device.pci_bus_id));
			if (device.valid_fields & BETTERTOP_GPU_DEVICE_MEMORY_TOTAL)
				fmt::println("      memory: {} / {} MiB",
					device.memory_used_bytes / (1024 * 1024), device.memory_total_bytes / (1024 * 1024));
			if (device.valid_fields & BETTERTOP_GPU_DEVICE_GPU_UTIL)
				fmt::println("      GPU utilization: {}%", device.gpu_util_pct);
		}
		if (snapshot.flags & BETTERTOP_GPU_TRUNCATED_DEVICES) fmt::println("  Inventory was truncated.");
	} else {
		fmt::println("No NVIDIA sample received.");
	}
	return snapshot.state == Gpu::Bridge::State::healthy && !snapshot.devices.empty() ? 0 : 1;
}
#endif

//* --------------------------------------------- Main starts here! ---------------------------------------------------
[[nodiscard]] auto btop_main(const std::span<const std::string_view> args) -> int {

	//? ------------------------------------------------ INIT ---------------------------------------------------------

	Global::start_time = time_s();

	//? Save real and effective userid's and drop privileges until needed if running with SUID bit set
	Global::real_uid = getuid();
	Global::set_uid = geteuid();
	if (Global::real_uid != Global::set_uid) {
		if (seteuid(Global::real_uid) != 0) {
			Global::real_uid = Global::set_uid;
			Global::exit_error_msg = "Failed to change effective user ID. Unset btop SUID bit to ensure security on this system. Quitting!";
			clean_quit(1);
		}
	}

	Cli::Cli cli;
	{
		// Get the cli options or return with an exit code
		auto result = Cli::parse(args);
		if (result.has_value()) {
			cli = result.value();
		} else {
			auto error = result.error();
			if (error != 0) {
				Cli::usage();
				Cli::help_hint();
			}
			return error;
		}
	}

	Global::debug = cli.debug;

	{
		const auto config_dir = Config::get_config_dir();

		if (cli.config_file.has_value()) {
			Config::conf_file = cli.config_file.value();
		} else if (config_dir.has_value()) {
			Config::conf_dir = config_dir.value();
			Config::conf_file = Config::conf_dir / "bettertop.conf";

			auto log_file = Config::get_log_file();
			if (log_file.has_value()) {
				Logger::init(log_file.value());
			}

			Theme::user_theme_dir = Config::conf_dir / "themes";

			// If necessary create the user theme directory
			std::error_code error;
			if (not fs::exists(Theme::user_theme_dir, error) and not fs::create_directories(Theme::user_theme_dir, error)) {
				Theme::user_theme_dir.clear();
				Logger::warning("Failed to create user theme directory: {}", error.message());
			}
		}
	}

	//? Try to find global btop theme path relative to binary path
#ifdef __linux__
	{ 	std::error_code ec;
		Global::self_path = fs::read_symlink("/proc/self/exe", ec).remove_filename();
	}
#elif __APPLE__
	{
		char buf [PATH_MAX];
		uint32_t bufsize = PATH_MAX;
		if(!_NSGetExecutablePath(buf, &bufsize))
			Global::self_path = fs::path(buf).remove_filename();
	}
#elif __NetBSD__
	{
		int mib[4];
		char buf[PATH_MAX];
		size_t bufsize = sizeof buf;

		mib[0] = CTL_KERN;
		mib[1] = KERN_PROC_ARGS;
		mib[2] = getpid();
		mib[3] = KERN_PROC_PATHNAME;
		if (sysctl(mib, 4, buf, &bufsize, NULL, 0) == 0)
			Global::self_path = fs::path(buf).remove_filename();
	}
#endif
	if (std::error_code ec; not Global::self_path.empty()) {
		Theme::theme_dir = fs::canonical(Global::self_path / "../share/btop/themes", ec);
		if (ec or not fs::is_directory(Theme::theme_dir) or access(Theme::theme_dir.c_str(), R_OK) == -1) Theme::theme_dir.clear();
	}
	//? If relative path failed, check two most common absolute paths
	if (Theme::theme_dir.empty()) {
		for (auto theme_path : {"/usr/local/share/btop/themes", "/usr/share/btop/themes"}) {
			if (fs::is_directory(fs::path(theme_path)) and access(theme_path, R_OK) != -1) {
				Theme::theme_dir = fs::path(theme_path);
				break;
			}
		}
	}

	//? Set custom themes directory from command line if provided
	if (cli.themes_dir.has_value()) {
		Theme::custom_theme_dir = cli.themes_dir.value();
		Logger::info("Using custom themes directory: {}", Theme::custom_theme_dir.string());
	}

	//? Config init
	init_config(cli.low_color, cli.filter);
	if (cli.classic) Config::set("ml_view", false);
	if (cli.fun_mode.has_value()) Config::set("ml_fun", cli.fun_mode.value());
	if (cli.diagnose_gpu) {
#if defined(__linux__) && defined(GPU_SUPPORT)
		return diagnose_gpu();
#else
		fmt::println("GPU diagnostic is available on Linux NVIDIA builds only.");
		return 2;
#endif
	}

	//? Try to find and set a UTF-8 locale
	if (std::setlocale(LC_ALL, "") != nullptr and not std::string_view { std::setlocale(LC_ALL, "") }.contains(";")
	and str_to_upper(s_replace((string)std::setlocale(LC_ALL, ""), "-", "")).ends_with("UTF8")) {
		Logger::debug("Using locale {}", std::locale().name());
	}
	else {
		string found;
		bool set_failure{};
		for (const auto loc_env : array{"LANG", "LC_ALL", "LC_CTYPE"}) {
			if (std::getenv(loc_env) != nullptr and str_to_upper(s_replace((string)std::getenv(loc_env), "-", "")).ends_with("UTF8")) {
				found = std::getenv(loc_env);
				if (std::setlocale(LC_ALL, found.c_str()) == nullptr) {
					set_failure = true;
					Logger::warning("Failed to set locale {} continuing anyway.", found);
				}
			}
		}
		if (found.empty()) {
			if (setenv("LC_ALL", "", 1) == 0 and setenv("LANG", "", 1) == 0) {
				try {
					if (const auto loc = std::locale("").name(); not loc.empty() and loc != "*") {
						for (auto& l : ssplit(loc, ';')) {
							if (str_to_upper(s_replace(l, "-", "")).ends_with("UTF8")) {
								found = l.substr(l.find('=') + 1);
								if (std::setlocale(LC_ALL, found.c_str()) != nullptr) {
									break;
								}
							}
						}
					}
				}
				catch (...) { found.clear(); }
			}
		}
	//
	#ifdef __APPLE__
		if (found.empty()) {
			CFLocaleRef cflocale = CFLocaleCopyCurrent();
			CFStringRef id_value = (CFStringRef)CFLocaleGetValue(cflocale, kCFLocaleIdentifier);
			auto loc_id = CFStringGetCStringPtr(id_value, kCFStringEncodingUTF8);
			CFRelease(cflocale);
			std::string cur_locale = (loc_id != nullptr ? loc_id : "");
			if (cur_locale.empty()) {
				Logger::warning("No UTF-8 locale detected! Some symbols might not display correctly.");
			}
			else if (std::setlocale(LC_ALL, string(cur_locale + ".UTF-8").c_str()) != nullptr) {
				Logger::debug("Setting LC_ALL={}.UTF-8", cur_locale);
			}
			else if(std::setlocale(LC_ALL, "en_US.UTF-8") != nullptr) {
				Logger::debug("Setting LC_ALL=en_US.UTF-8");
			}
			else {
				Logger::warning("Failed to set macos locale, continuing anyway.");
			}
		}
	#else
		if (found.empty() and cli.force_utf) {
			Logger::warning("No UTF-8 locale detected! Forcing start with --force-utf argument.");
		} else if (found.empty()) {
			Global::exit_error_msg = "No UTF-8 locale detected!\nUse --force-utf argument to force start if you're sure your terminal can handle it.";
			clean_quit(1);
		}
	#endif
		else if (not set_failure) {
			Logger::debug("Setting LC_ALL={}", found);
		}
	}

	_install_signal_handlers();

	//? Initialize terminal and set options
	if (not Term::init()) {
		Global::exit_error_msg = "No tty detected!\nbtop++ needs an interactive shell to run.";
		clean_quit(1);
	}

#if defined(GPU_SUPPORT) && defined(__linux__)
	gpu_bridge_started = true;
	Gpu::Bridge::start();
#endif

	if (Term::current_tty != "unknown") {
		Logger::info("Running on {}", Term::current_tty);
	}

	configure_tty_mode(cli.force_tty);

	//? Check for valid terminal dimensions
	{
		int t_count = 0;
		while (Term::width <= 0 or Term::width > 10000 or Term::height <= 0 or Term::height > 10000) {
			sleep_ms(10);
			Term::refresh();
			if (++t_count == 100) {
				Global::exit_error_msg = "Failed to get size of terminal!";
				clean_quit(1);
			}
		}
	}

	//? Platform dependent init and error check
	try {
		Shared::init();
	}
	catch (const std::exception& e) {
		Global::exit_error_msg = fmt::format("Exception in Shared::init() -> {}", e.what());
		clean_quit(1);
	}

	if (not Config::set_boxes(Config::getS("shown_boxes"))) {
		Config::set_boxes("cpu mem net proc");
		Config::set("shown_boxes", "cpu mem net proc"s);
	}

	//? Update list of available themes and generate the selected theme
	Theme::updateThemes();
	Theme::setTheme();

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	pthread_sigmask(SIG_BLOCK, &mask, &Input::signal_mask);

	if (pthread_create(&Runner::runner_id, nullptr, &Runner::_runner, nullptr) != 0) {
		Global::exit_error_msg = "Failed to create _runner thread!";
		clean_quit(1);
	}
	else {
		Global::_runner_started = true;
	}

	//? Calculate sizes of all boxes
	Config::presetsValid(Config::getS("presets"));
	if (cli.preset.has_value()) {
		Config::current_preset = min(static_cast<std::int32_t>(cli.preset.value()), static_cast<std::int32_t>(Config::preset_list.size() - 1));
		Config::apply_preset(Config::preset_list.at(Config::current_preset.value()));
	}

	if (Config::getB("ml_view")) {
		Global::resized = true;
	} else {
		const auto [x, y] = Term::get_min_size(Config::getS("shown_boxes"));
		if (Term::height < y or Term::width < x) {
			pthread_sigmask(SIG_SETMASK, &Input::signal_mask, &mask);
			term_resize(true);
			pthread_sigmask(SIG_SETMASK, &mask, nullptr);
			Global::resized = false;
		}
		Draw::calcSizes();

		//? Print out box outlines
		const bool term_sync = Config::getB("terminal_sync");
		Term::output((term_sync ? Term::sync_start : "") + Cpu::box + Mem::box + Net::box + Proc::box + (term_sync ? Term::sync_end : ""));
	}


	//? ------------------------------------------------ MAIN LOOP ----------------------------------------------------

	if (cli.updates.has_value()) {
		Config::set("update_ms", static_cast<int>(cli.updates.value()));
	}
	uint64_t update_ms = Config::getI("update_ms");
	auto future_time = time_ms();

	try {
		while (not true not_eq not false) {
			_dispatch_signal_flags();
			//? Check for exceptions in secondary thread and exit with fail signal if true
			if (Global::thread_exception) {
				clean_quit(1);
			}
			else if (Global::should_quit) {
				clean_quit(pending_quit_status);
			}
			else if (Global::should_sleep) {
				Global::should_sleep = false;
				_sleep();
			}
			//? Hot reload config from CTRL + R or SIGUSR2
			else if (Global::reload_conf) {
				Global::reload_conf = false;
				if (Runner::active) Runner::stop();
				Config::unlock();
				init_config(cli.low_color, cli.filter);
				if (cli.classic) Config::set("ml_view", false);
				if (cli.fun_mode.has_value()) Config::set("ml_fun", cli.fun_mode.value());
				Theme::updateThemes();
				Theme::setTheme();
				Draw::banner_gen(0, 0, false, true);
				Global::resized = true;
			}

			//? Make sure terminal size hasn't changed (in case of SIGWINCH not working properly)
			term_resize(Global::resized);

			//? Trigger secondary thread to redraw if terminal has been resized
			if (Global::resized) {
				if (not Config::getB("ml_view")) {
					Draw::calcSizes();
					Draw::update_clock(true);
				}
				Global::resized = false;
				if (Menu::active) Menu::process();
				else Runner::run("all", true, true);
				atomic_wait_for(Runner::active, true, 1000);
			}

			//? Update clock if needed
			if (not Config::getB("ml_view") and Draw::update_clock() and not Menu::active) {
				Runner::run("clock");
			}

			//? Start secondary collect & draw thread at the interval set by <update_ms> config value
			if (time_ms() >= future_time and not Global::resized) {
				Runner::run("all");
				update_ms = Config::getI("update_ms");
				future_time = time_ms() + update_ms;
			}

			//? Loop over input polling and input action processing
			for (auto current_time = time_ms(); current_time < future_time; current_time = time_ms()) {

				//? Check for external clock changes and for changes to the update timer
				if (std::cmp_not_equal(update_ms, Config::getI("update_ms"))) {
					update_ms = Config::getI("update_ms");
					future_time = time_ms() + update_ms;
				}
				else if (future_time - current_time > update_ms) {
					future_time = current_time;
				}
				//? Poll for input and process any input detected
				else if (Input::poll(min((uint64_t)1000, future_time - current_time))) {
					if (not Runner::active) Config::unlock();

					if (Menu::active) Menu::process(Input::get());
					else Input::process(Input::get());
				}

				//? Break the loop at 1000ms intervals or if input polling was interrupted
				else break;

			}

		}
	}
	catch (const std::exception& e) {
		Global::exit_error_msg = fmt::format("Exception in main loop -> {}", e.what());
		clean_quit(1);
	}
	return 0;
}
