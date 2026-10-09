// Offline/native smoke test. This is not an installed UWP or Xbox test.
#include <vulkan/vulkan.h>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <fstream>
#include <windows.h>

static void check(VkResult result)
{
    if (result != VK_SUCCESS) { std::printf("VkResult=%d\n", result); throw std::runtime_error("Vulkan probe failed"); }
}
#include "dozen_draw_probe.h"
static void test_sample(VkDevice device, VkQueue queue, VkCommandPool pool,
    VkImage image, VkBuffer buffer, VkDeviceMemory memory, const char* shaderPath, unsigned setIndex, bool unsized, bool late)
{
    std::ifstream input(shaderPath, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Missing sampling SPIR-V");
    const auto bytes = static_cast<size_t>(input.tellg());
    if (!bytes || bytes % 4) throw std::runtime_error("Invalid sampling SPIR-V");
    std::vector<uint32_t> code(bytes / 4);
    input.seekg(0); input.read(reinterpret_cast<char*>(code.data()), bytes);
    if (!input) throw std::runtime_error("Cannot read sampling SPIR-V");
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = bytes; sm.pCode = code.data();
    VkShaderModule module{}; check(vkCreateShaderModule(device, &sm, nullptr, &module));
    VkDescriptorSetLayoutBinding bindings[3] = {
        {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ds.bindingCount = 3; ds.pBindings = bindings;
    VkDescriptorBindingFlags bindingFlags[3] = {
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags.bindingCount = 3; flags.pBindingFlags = bindingFlags;
    if (late) { ds.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT; ds.pNext = &flags; }
    VkDescriptorSetLayout setLayout{}; check(vkCreateDescriptorSetLayout(device, &ds, nullptr, &setLayout));
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayoutCreateInfo emptyInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout empty{}; check(vkCreateDescriptorSetLayout(device, &emptyInfo, nullptr, &empty));
    VkDescriptorSetLayout setLayouts[] = {setIndex ? empty : setLayout, setLayout};
    pl.setLayoutCount = setIndex + 1; pl.pSetLayouts = setLayouts;
    VkPipelineLayout layout{}; check(vkCreatePipelineLayout(device, &pl, nullptr, &layout));
    VkComputePipelineCreateInfo pc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pc.layout = layout;
    pc.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pc.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pc.stage.module = module; pc.stage.pName = "main";
    VkPipeline pipeline{}; check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pc, nullptr, &pipeline));
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1; dp.poolSizeCount = 3; dp.pPoolSizes = sizes;
    if (late) dp.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    VkDescriptorPool descriptors{}; check(vkCreateDescriptorPool(device, &dp, nullptr, &descriptors));
    VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    sa.descriptorPool = descriptors; sa.descriptorSetCount = 1; sa.pSetLayouts = &setLayout;
    VkDescriptorSet set{}; check(vkAllocateDescriptorSets(device, &sa, &set));
    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler{}; check(vkCreateSampler(device, &sc, nullptr, &sampler));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_B8G8R8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // Includes the non-identity remap used by RSX textures, not just copies.
    const VkComponentMapping maps[] = {
        {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A},
        {VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B}};
    const uint32_t expected[] = {0xff804000u, 0x804000ffu};
    for (unsigned i = 0; i < 2; ++i)
    {
        vi.components = maps[i];
        VkImageView view{}; check(vkCreateImageView(device, &vi, nullptr, &view));
        VkDescriptorImageInfo ii{sampler, view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo bi{buffer, 128, 4};
        VkDescriptorBufferInfo uniformInfo{buffer, 0, 64};
        VkWriteDescriptorSet writes[3] = {};
        for (unsigned b = 0; b < 3; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = set; writes[b].dstBinding = bindings[b].binding; writes[b].descriptorCount = 1;
            writes[b].descriptorType = bindings[b].descriptorType;
        }
        writes[0].pImageInfo = &ii; writes[1].pBufferInfo = &bi;
        writes[2].pBufferInfo = &uniformInfo;
        if (unsized) {
            void* data{}; check(vkMapMemory(device, memory, 0, 256, 0, &data));
            std::memset(data, 0, 64);
            const float values[] = {.2f, .4f, .6f, .8f};
            std::memcpy(static_cast<char*>(data) + 48, values, sizeof(values));
            vkUnmapMemory(device, memory);
        }
        if (!late) vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool; ca.commandBufferCount = 1;
        VkCommandBuffer cmd{}; check(vkAllocateCommandBuffers(device, &ca, &cmd));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(cmd, &begin));
        VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib.image = image; ib.subresourceRange = vi.subresourceRange;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.oldLayout = i ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &ib);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, setIndex, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, 1, 1, 1);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &mb, 0, nullptr, 0, nullptr);
        check(vkEndCommandBuffer(cmd));
        if (late) vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
        check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); check(vkQueueWaitIdle(queue));
        void* mapped{}; check(vkMapMemory(device, memory, 0, 256, 0, &mapped));
        const auto actual = static_cast<uint32_t*>(mapped)[32];
        vkUnmapMemory(device, memory);
        const uint32_t wanted = unsized ? 0xcc996633u : expected[i];
        std::printf("Sample remap %u: actual=%08x expected=%08x\n", i, actual, wanted);
        if (actual != wanted) throw std::runtime_error("DZN sampled channel/constant mismatch");
        vkFreeCommandBuffers(device, pool, 1, &cmd); vkDestroyImageView(device, view, nullptr);
    }
    vkDestroySampler(device, sampler, nullptr); vkDestroyDescriptorPool(device, descriptors, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr); vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyDescriptorSetLayout(device, setLayout, nullptr); vkDestroyShaderModule(device, module, nullptr);
    vkDestroyDescriptorSetLayout(device, empty, nullptr);
}

int main(int argc, char** argv)
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
            const bool late = argc > 3 && (std::strcmp(argv[3], "late") == 0 || std::strcmp(argv[3], "late-sample") == 0);
            if (late) {
                v12.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
                v12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
                v12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
            }
            v12.pNext = &unsized;
            deviceInfo.pNext = &v12;
            const char* extension = VK_EXT_SHADER_UNIFORM_BUFFER_UNSIZED_ARRAY_EXTENSION_NAME;
            deviceInfo.enabledExtensionCount = 1; deviceInfo.ppEnabledExtensionNames = &extension;
            VkDevice device{};
            check(vkCreateDevice(gpu, &deviceInfo, nullptr, &device));
            VkQueue queue{};
            vkGetDeviceQueue(device, family, 0, &queue);
            VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bufferInfo.size = 320; bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
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
            VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
            VkPhysicalDeviceMemoryProperties2 budgetProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
            budgetProperties.pNext = &budget;
            vkGetPhysicalDeviceMemoryProperties2(gpu, &budgetProperties);
            const auto uploadHeap = memory.memoryTypes[type].heapIndex;
            if (!budget.heapBudget[uploadHeap] || budget.heapUsage[uploadHeap] < requirements.size)
                throw std::runtime_error("DZN memory budget does not account for the live upload allocation");
            std::printf("PASS: live heap budget usage=%llu budget=%llu\n",
                static_cast<unsigned long long>(budget.heapUsage[uploadHeap]),
                static_cast<unsigned long long>(budget.heapBudget[uploadHeap]));
            void* upload{};
            check(vkMapMemory(device, allocation, 0, 320, 0, &upload));
            for (unsigned i = 64; i < 80; ++i) static_cast<uint32_t*>(upload)[i] = 0xff004080u;
            vkUnmapMemory(device, allocation);
            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D; imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
            imageInfo.extent = {4, 4, 1}; imageInfo.mipLevels = 1; imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            VkImage image{};
            check(vkCreateImage(device, &imageInfo, nullptr, &image));
            vkGetImageMemoryRequirements(device, image, &requirements);
            type = 0;
            while (type < memory.memoryTypeCount && !(requirements.memoryTypeBits & (1u << type))) ++type;
            allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = type;
            VkDeviceMemory imageMemory{};
            check(vkAllocateMemory(device, &allocate, nullptr, &imageMemory));
            check(vkBindImageMemory(device, image, imageMemory, 0));
            VkImage blitImage{};
            check(vkCreateImage(device, &imageInfo, nullptr, &blitImage));
            vkGetImageMemoryRequirements(device, blitImage, &requirements);
            allocate.allocationSize = requirements.size;
            VkDeviceMemory blitMemory{};
            check(vkAllocateMemory(device, &allocate, nullptr, &blitMemory));
            check(vkBindImageMemory(device, blitImage, blitMemory, 0));
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
            VkMemoryBarrier uploadBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            uploadBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            uploadBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 1, &uploadBarrier, 0, nullptr, 0, nullptr);
            VkBufferImageCopy uploadRegion{};
            uploadRegion.bufferOffset = 256;
            uploadRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            uploadRegion.imageExtent = {4, 4, 1};
            vkCmdCopyBufferToImage(command, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &uploadRegion);
            imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
            VkImageMemoryBarrier blitBarrier = imageBarrier;
            blitBarrier.image = blitImage; blitBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            blitBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            blitBarrier.srcAccessMask = 0; blitBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &blitBarrier);
            VkImageBlit blit{};
            blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = blit.dstOffsets[1] = {4, 4, 1};
            vkCmdBlitImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                blitImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            blitBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            blitBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            blitBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; blitBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &blitBarrier);
            VkMemoryBarrier fillBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            fillBarrier.srcAccessMask = fillBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 1, &fillBarrier, 0, nullptr, 0, nullptr);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {4, 4, 1};
            vkCmdCopyImageToBuffer(command, blitImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
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
            if (argc > 1) test_sample(device, queue, pool, image, buffer, allocation, argv[1], argc > 2 ? 1 : 0,
                argc > 3 && std::strcmp(argv[3], "late-sample") != 0, late);
            if (argc > 5) test_draw(device, queue, pool, blitImage, buffer, allocation, argv[4], argv[5]);
            vkDestroyFence(device, fence, nullptr);
            vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyPipeline(device, pipeline, nullptr);
            vkDestroyPipelineLayout(device, layout, nullptr);
            vkDestroyShaderModule(device, module, nullptr);
            vkDestroyBuffer(device, buffer, nullptr);
            vkDestroyImage(device, image, nullptr);
            vkDestroyImage(device, blitImage, nullptr);
            vkFreeMemory(device, blitMemory, nullptr);
            vkFreeMemory(device, imageMemory, nullptr);
            vkFreeMemory(device, allocation, nullptr);
            vkDestroyDevice(device, nullptr);
            vkDestroyInstance(instance, nullptr);
        }
        std::puts("PASS: Vulkan 1.2 -> DZN -> D3D12 PSO, staged BGRA upload/readback and restart");
        return 0;
    }
    catch (const std::exception& error) { std::puts(error.what()); return 1; }
}
