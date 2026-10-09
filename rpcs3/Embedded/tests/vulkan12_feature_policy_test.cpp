#include "../../Emu/RSX/VK/vkutils/Vulkan12FeaturePolicy.h"
#include <cstdio>

int main()
{
	constexpr auto none = vk::select_vulkan12_features(false, false);
	constexpr auto runtime_only = vk::select_vulkan12_features(true, false);
	constexpr auto both = vk::select_vulkan12_features(true, true);
	static_assert(!none.runtime_descriptor_array && !none.uniform_buffer_standard_layout);
	static_assert(runtime_only.runtime_descriptor_array && !runtime_only.uniform_buffer_standard_layout);
	static_assert(both.runtime_descriptor_array && both.uniform_buffer_standard_layout);
	std::puts("PASS: Vulkan 1.2 device requests never exceed reported feature support");
}
