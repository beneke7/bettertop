// SPDX-License-Identifier: Apache-2.0

#include <vector>

#include <gtest/gtest.h>

#include "btop_tools.hpp"
#if defined(__linux__) && defined(GPU_SUPPORT)
#include "btop_shared.hpp"
#endif

TEST(tools, string_split) {
	EXPECT_EQ(Tools::ssplit(""), std::vector<std::string> {});
	EXPECT_EQ(Tools::ssplit("foo"), std::vector<std::string> { "foo" });
	{
		auto actual = Tools::ssplit("foo       bar         baz    ");
		auto expected = std::vector<std::string> { "foo", "bar", "baz" };
		EXPECT_EQ(actual, expected);
	}

	{
		auto actual = Tools::ssplit("foobo  oho  barbo  bo  bazbo", 'o');
		auto expected = std::vector<std::string> { "f", "b", "  ", "h", "  barb", "  b", "  bazb" };
		EXPECT_EQ(actual, expected);
	}
}

#if defined(__linux__) && defined(GPU_SUPPORT)
TEST(proc, gpu_sorting) {
	std::vector<Proc::proc_info> processes(3);
	processes[0].pid = 1;
	processes[0].gpu_percent = 20;
	processes[1].pid = 2;
	processes[2].pid = 3;
	processes[2].gpu_percent = 90;

	Proc::proc_sorter(processes, "gpu", false);
	EXPECT_EQ(processes[0].pid, 3);
	EXPECT_EQ(processes[1].pid, 1);
	EXPECT_EQ(processes[2].pid, 2);

	Proc::proc_sorter(processes, "gpu", true);
	EXPECT_EQ(processes[0].pid, 2);
	EXPECT_EQ(processes[1].pid, 1);
	EXPECT_EQ(processes[2].pid, 3);
}
#endif
