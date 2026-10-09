#pragma once

namespace vk
{
	struct vulkan12_feature_policy
	{
		bool runtime_descriptor_array = false;
		bool uniform_buffer_standard_layout = false;
	};

	constexpr vulkan12_feature_policy select_vulkan12_features(bool runtime_descriptor_array,
		bool uniform_buffer_standard_layout) noexcept
	{
		return {runtime_descriptor_array, uniform_buffer_standard_layout};
	}
}
