// Offline/native smoke test. This is not an installed UWP or Xbox test.
#include <vulkan/vulkan.h>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <windows.h>

static void check(VkResult result)
{
    if (result != VK_SUCCESS) { std::printf("VkResult=%d\n", result); throw std::runtime_error("Vulkan probe failed"); }
}
int main()
{
    try
    {
        // Supply the packaged validator/system component to a native process;
        // the real host loads these through its package graph/import table.
        if (!LoadLibraryExW(L"dxil.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) ||
            !LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            throw std::runtime_error("Native probe dependencies unavailable");
        // Exercise a full stop/recreate cycle as well as cached dispatch.
        for (int iteration = 0; iteration < 2; ++iteration)
        {
            VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            app.apiVersion = VK_API_VERSION_1_2;
            VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            info.pApplicationInfo = &app;
            VkInstance instance{};
            check(vkCreateInstance(&info, nullptr, &instance));
            uint32_t count{};
            check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
            if (!count) throw std::runtime_error("No DZN device");
            std::vector<VkPhysicalDevice> gpus(count);
            check(vkEnumeratePhysicalDevices(instance, &count, gpus.data()));
            auto gpu = gpus.front();
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(gpu, &properties);
            std::printf("Device=%s API=%u.%u\n", properties.deviceName,
                VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion));
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
            std::vector<VkQueueFamilyProperties> queues(count);
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, queues.data());
            uint32_t family = 0;
            while (family < count && !(queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
            if (family == count) throw std::runtime_error("No graphics queue");
            float priority = 1;
            VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
            VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
            VkPhysicalDeviceShaderUniformBufferUnsizedArrayFeaturesEXT unsized{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNIFORM_BUFFER_UNSIZED_ARRAY_FEATURES_EXT};
            VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            v12.pNext = &unsized;
            VkPhysicalDeviceFeatures2 available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            available.pNext = &v12;
            vkGetPhysicalDeviceFeatures2(gpu, &available);
            if (!unsized.shaderUniformBufferUnsizedArray || !v12.runtimeDescriptorArray || !v12.uniformBufferStandardLayout)
                throw std::runtime_error("Missing required RPCS3 Vulkan features");
            v12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            v12.runtimeDescriptorArray = VK_TRUE; v12.uniformBufferStandardLayout = VK_TRUE;
            v12.pNext = &unsized;
            deviceInfo.pNext = &v12;
            const char* extension = VK_EXT_SHADER_UNIFORM_BUFFER_UNSIZED_ARRAY_EXTENSION_NAME;
            deviceInfo.enabledExtensionCount = 1; deviceInfo.ppEnabledExtensionNames = &extension;
            VkDevice device{};
            check(vkCreateDevice(gpu, &deviceInfo, nullptr, &device));
            VkQueue queue{};
            vkGetDeviceQueue(device, family, 0, &queue);
            VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bufferInfo.size = 256; bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VkBuffer buffer{};
            check(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer));
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(device, buffer, &requirements);
            VkPhysicalDeviceMemoryProperties memory{};
            vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
            uint32_t type = 0;
            constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            while (type < memory.memoryTypeCount && (!(requirements.memoryTypeBits & (1u << type)) ||
                (memory.memoryTypes[type].propertyFlags & flags) != flags)) ++type;
            if (type == memory.memoryTypeCount) throw std::runtime_error("No coherent readback memory");
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = type;
            VkDeviceMemory allocation{};
            check(vkAllocateMemory(device, &allocate, nullptr, &allocation));
            check(vkBindBufferMemory(device, buffer, allocation, 0));
            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D; imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
            imageInfo.extent = {4, 4, 1}; imageInfo.mipLevels = 1; imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            VkImage image{};
            check(vkCreateImage(device, &imageInfo, nullptr, &image));
            vkGetImageMemoryRequirements(device, image, &requirements);
            type = 0;
            while (type < memory.memoryTypeCount && !(requirements.memoryTypeBits & (1u << type))) ++type;
            allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = type;
            VkDeviceMemory imageMemory{};
            check(vkAllocateMemory(device, &allocate, nullptr, &imageMemory));
            check(vkBindImageMemory(device, image, imageMemory, 0));
            VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolInfo.queueFamilyIndex = family;
            VkCommandPool pool{};
            check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));
            VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            commandInfo.commandPool = pool; commandInfo.commandBufferCount = 1;
            VkCommandBuffer command{};
            check(vkAllocateCommandBuffers(device, &commandInfo, &command));
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            check(vkBeginCommandBuffer(command, &begin));
            // SPIR-V for a no-descriptor compute main(), local_size = 1.
            // Pipeline creation exercises SPIR-V -> NIR -> DXIL -> D3D12 PSO.
            constexpr uint32_t shader[] = {
                0x07230203, 0x00010000, 0, 5, 0,
                0x00020011, 1, 0x0003000e, 0, 1,
                0x0005000f, 5, 3, 0x6e69616d, 0,
                0x00060010, 3, 17, 1, 1, 1,
                0x00020013, 1, 0x00030021, 2, 1,
                0x00050036, 1, 3, 0, 2,
                0x000200f8, 4, 0x000100fd, 0x00010038
            };
            VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            shaderInfo.codeSize = sizeof(shader); shaderInfo.pCode = shader;
            VkShaderModule module{};
            check(vkCreateShaderModule(device, &shaderInfo, nullptr, &module));
            VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            VkPipelineLayout layout{};
            check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout));
            VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipelineInfo.layout = layout;
            pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            pipelineInfo.stage.module = module; pipelineInfo.stage.pName = "main";
            VkPipeline pipeline{};
            check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdDispatch(command, 1, 1, 1);
            vkCmdFillBuffer(command, buffer, 0, 256, 0xa51372fe);
            VkImageMemoryBarrier imageBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            imageBarrier.image = image;
            imageBarrier.srcQueueFamilyIndex = imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            imageBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
            VkClearColorValue color{}; color.float32[1] = .25f; color.float32[2] = .5f; color.float32[3] = 1.f;
            vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &imageBarrier.subresourceRange);
            imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
            VkMemoryBarrier fillBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            fillBarrier.srcAccessMask = fillBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 1, &fillBarrier, 0, nullptr, 0, nullptr);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {4, 4, 1};
            vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                0, 1, &barrier, 0, nullptr, 0, nullptr);
            check(vkEndCommandBuffer(command));
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            VkFence fence{};
            check(vkCreateFence(device, &fenceInfo, nullptr, &fence));
            check(vkQueueSubmit(queue, 1, &submit, fence));
            check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull));
            void* mapped{};
            check(vkMapMemory(device, allocation, 0, 256, 0, &mapped));
            for (int i = 0; i < 64; ++i)
                if (static_cast<uint32_t*>(mapped)[i] != (i < 16 ? 0xff004080u : 0xa51372feu)) throw std::runtime_error("DZN BGRA readback mismatch");
            vkUnmapMemory(device, allocation);
            vkDestroyFence(device, fence, nullptr);
            vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyPipeline(device, pipeline, nullptr);
            vkDestroyPipelineLayout(device, layout, nullptr);
            vkDestroyShaderModule(device, module, nullptr);
            vkDestroyBuffer(device, buffer, nullptr);
            vkDestroyImage(device, image, nullptr);
            vkFreeMemory(device, imageMemory, nullptr);
            vkFreeMemory(device, allocation, nullptr);
            vkDestroyDevice(device, nullptr);
            vkDestroyInstance(instance, nullptr);
        }
        std::puts("PASS: Vulkan 1.2 -> DZN -> D3D12 PSO, BGRA readback and restart");
        return 0;
    }
    catch (const std::exception& error) { std::puts(error.what()); return 1; }
}
