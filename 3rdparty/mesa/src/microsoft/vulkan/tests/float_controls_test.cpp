/* SPDX-License-Identifier: MIT */
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

static void check(VkResult result, const char *operation)
{
   if (result != VK_SUCCESS) {
      std::fprintf(stderr, "%s: VkResult %d\n", operation, result);
      std::exit(1);
   }
}

int main(int argc, char **argv)
{
   if (argc != 5) return 1;
   HMODULE library = LoadLibraryA(argv[1]);
   if (!library) return 1;
   auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      GetProcAddress(library, "vk_icdGetInstanceProcAddr"));
   if (!get) return 1;
   auto create = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
   VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
   app.apiVersion = VK_API_VERSION_1_2;
   VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
   info.pApplicationInfo = &app;
   VkInstance instance;
   check(create(&info, nullptr, &instance), "create instance");
#define IFN(n) auto n = reinterpret_cast<PFN_##n>(get(instance, #n))
   IFN(vkEnumeratePhysicalDevices);
   IFN(vkGetPhysicalDeviceProperties2);
   IFN(vkGetPhysicalDeviceMemoryProperties);
   IFN(vkGetPhysicalDeviceQueueFamilyProperties);
   IFN(vkEnumerateDeviceExtensionProperties);
   IFN(vkCreateDevice);
   IFN(vkGetDeviceProcAddr);
   IFN(vkDestroyInstance);
   uint32_t count = 0;
   check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "device count");
   if (!count) return 77;
   std::vector<VkPhysicalDevice> physical(count);
   check(vkEnumeratePhysicalDevices(instance, &count, physical.data()), "devices");
   VkPhysicalDeviceFloatControlsProperties controls = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
   VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &controls};
   vkGetPhysicalDeviceProperties2(physical[0], &properties);
   std::printf("%s: preserve32=%u flush32=%u RTE32=%u\n", properties.properties.deviceName,
               controls.shaderDenormPreserveFloat32, controls.shaderDenormFlushToZeroFloat32,
               controls.shaderRoundingModeRTEFloat32);
   check(vkEnumerateDeviceExtensionProperties(physical[0], nullptr, &count, nullptr), "extension count");
   std::vector<VkExtensionProperties> extensions(count);
   check(vkEnumerateDeviceExtensionProperties(physical[0], nullptr, &count, extensions.data()), "extensions");
   bool found = false;
   for (const auto &extension : extensions)
      found |= !std::strcmp(extension.extensionName, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
   if (!found) return 1;
   vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &count, nullptr);
   std::vector<VkQueueFamilyProperties> families(count);
   vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &count, families.data());
   uint32_t family = 0;
   while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) family++;
   if (family == count) return 77;
   float priority = 1;
   VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
   qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
   const char *extension = VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME;
   VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
   di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
   di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = &extension;
   VkDevice device;
   check(vkCreateDevice(physical[0], &di, nullptr, &device), "create device");
#define DFN(n) auto n = reinterpret_cast<PFN_##n>(vkGetDeviceProcAddr(device, #n))
   DFN(vkGetDeviceQueue); DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements);
   DFN(vkAllocateMemory); DFN(vkBindBufferMemory); DFN(vkMapMemory); DFN(vkUnmapMemory);
   DFN(vkCreateDescriptorSetLayout); DFN(vkCreateDescriptorPool); DFN(vkAllocateDescriptorSets);
   DFN(vkUpdateDescriptorSets); DFN(vkCreatePipelineLayout); DFN(vkCreateShaderModule);
   DFN(vkCreateComputePipelines); DFN(vkCreateCommandPool); DFN(vkAllocateCommandBuffers);
   DFN(vkBeginCommandBuffer); DFN(vkEndCommandBuffer); DFN(vkCmdBindPipeline);
   DFN(vkCmdBindDescriptorSets); DFN(vkCmdDispatch); DFN(vkCmdPipelineBarrier);
   DFN(vkQueueSubmit); DFN(vkQueueWaitIdle); DFN(vkResetCommandBuffer);
   DFN(vkDestroyPipeline); DFN(vkDestroyShaderModule); DFN(vkDestroyPipelineLayout);
   DFN(vkDestroyDescriptorPool); DFN(vkDestroyDescriptorSetLayout); DFN(vkDestroyCommandPool);
   DFN(vkDestroyBuffer); DFN(vkFreeMemory); DFN(vkDestroyDevice);
   VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
   VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
   bi.size = 16; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
   VkBuffer buffer; check(vkCreateBuffer(device, &bi, nullptr, &buffer), "create buffer");
   VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device, buffer, &requirements);
   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physical[0], &mp);
   uint32_t type = 0;
   const VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   while (type < mp.memoryTypeCount &&
          (!(requirements.memoryTypeBits & (1u << type)) || (mp.memoryTypes[type].propertyFlags & flags) != flags)) type++;
   if (type == mp.memoryTypeCount) return 77;
   VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
   ai.allocationSize = requirements.size; ai.memoryTypeIndex = type;
   VkDeviceMemory memory; check(vkAllocateMemory(device, &ai, nullptr, &memory), "allocate memory");
   check(vkBindBufferMemory(device, buffer, memory, 0), "bind buffer");
   uint32_t *values; check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void **>(&values)), "map");
   VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
   VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
   li.bindingCount = 1; li.pBindings = &binding;
   VkDescriptorSetLayout set_layout; check(vkCreateDescriptorSetLayout(device, &li, nullptr, &set_layout), "set layout");
   VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   pli.setLayoutCount = 1; pli.pSetLayouts = &set_layout;
   VkPipelineLayout layout; check(vkCreatePipelineLayout(device, &pli, nullptr, &layout), "pipeline layout");
   VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
   VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
   dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &pool_size;
   VkDescriptorPool pool; check(vkCreateDescriptorPool(device, &dpi, nullptr, &pool), "descriptor pool");
   VkDescriptorSetAllocateInfo sai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
   sai.descriptorPool = pool; sai.descriptorSetCount = 1; sai.pSetLayouts = &set_layout;
   VkDescriptorSet set; check(vkAllocateDescriptorSets(device, &sai, &set), "descriptor set");
   VkDescriptorBufferInfo dbi = {buffer, 0, 16};
   VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
   write.dstSet = set; write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   write.pBufferInfo = &dbi; vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
   VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   cpi.queueFamilyIndex = family; cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   VkCommandPool command_pool; check(vkCreateCommandPool(device, &cpi, nullptr, &command_pool), "command pool");
   VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
   cai.commandPool = command_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
   VkCommandBuffer command; check(vkAllocateCommandBuffers(device, &cai, &command), "command buffer");
   const bool supported[] = {bool(controls.shaderDenormPreserveFloat32), bool(controls.shaderDenormFlushToZeroFloat32), bool(controls.shaderRoundingModeRTEFloat32)};
   unsigned tested = 0;
   for (unsigned mode = 0; mode < 3; mode++) {
      if (!supported[mode]) continue;
      std::ifstream file(argv[mode + 2], std::ios::binary | std::ios::ate);
      if (!file) return 1;
      size_t length = static_cast<size_t>(file.tellg());
      if (!length || length % 4) return 1;
      std::vector<uint32_t> code(length / 4); file.seekg(0); file.read(reinterpret_cast<char *>(code.data()), length);
      VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      smi.codeSize = length; smi.pCode = code.data();
      VkShaderModule shader; check(vkCreateShaderModule(device, &smi, nullptr, &shader), "shader");
      VkComputePipelineCreateInfo pci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      pci.layout = layout; pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pci.stage.module = shader; pci.stage.pName = "main";
      VkPipeline pipeline; check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline), "compute pipeline");
      values[0] = mode < 2 ? 0x00800000u : 0x3f800000u;
      values[1] = mode < 2 ? 0x3f000000u : 0x33800000u;
      values[2] = values[3] = 0xdeadbeefu;
      VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      check(vkBeginCommandBuffer(command, &begin), "begin");
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
      vkCmdDispatch(command, 1, 1, 1);
      VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
      check(vkEndCommandBuffer(command), "end");
      VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
      check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "submit"); check(vkQueueWaitIdle(queue), "wait");
      uint32_t expected = mode == 0 ? 0x00400000u : mode == 1 ? 0 : 0x3f800000u;
      uint32_t actual = values[mode == 2 ? 3 : 2];
      std::printf("mode %u: actual=%08x expected=%08x\n", mode, actual, expected);
      if (actual != expected) return 1;
      check(vkResetCommandBuffer(command, 0), "reset");
      vkDestroyPipeline(device, pipeline, nullptr); vkDestroyShaderModule(device, shader, nullptr); tested++;
   }
   vkDestroyCommandPool(device, command_pool, nullptr); vkDestroyDescriptorPool(device, pool, nullptr);
   vkDestroyPipelineLayout(device, layout, nullptr); vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
   vkUnmapMemory(device, memory); vkDestroyBuffer(device, buffer, nullptr); vkFreeMemory(device, memory, nullptr);
   vkDestroyDevice(device, nullptr); vkDestroyInstance(instance, nullptr); FreeLibrary(library);
   return tested ? 0 : 77;
}
