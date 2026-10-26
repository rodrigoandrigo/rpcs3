/* SPDX-License-Identifier: MIT */
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

static void check(VkResult r, const char *op) {
   if (r != VK_SUCCESS) { std::fprintf(stderr, "%s: %d\n", op, r); std::exit(1); }
}
static std::vector<uint32_t> read_spv(const char *path) {
   std::ifstream f(path, std::ios::binary | std::ios::ate);
   if (!f) std::exit(1);
   size_t size = size_t(f.tellg());
   std::vector<uint32_t> code(size / 4); f.seekg(0); f.read((char *)code.data(), size);
   return code;
}
int main(int argc, char **argv) {
   if (argc != 4) return 1;
   HMODULE library = LoadLibraryA(argv[1]);
   auto get = library ? (PFN_vkGetInstanceProcAddr)GetProcAddress(library, "vk_icdGetInstanceProcAddr") : nullptr;
   if (!get) return 1;
   auto create_instance = (PFN_vkCreateInstance)get(nullptr, "vkCreateInstance");
   VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_2;
   VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
   VkInstance instance; check(create_instance(&ici, nullptr, &instance), "instance");
#define IFN(n) auto n = (PFN_##n)get(instance, #n)
   IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceFeatures);
   IFN(vkGetPhysicalDeviceProperties); IFN(vkGetPhysicalDeviceMemoryProperties);
   IFN(vkGetPhysicalDeviceQueueFamilyProperties); IFN(vkCreateDevice);
   IFN(vkGetDeviceProcAddr); IFN(vkDestroyInstance);
   uint32_t count = 0; check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "device count");
   if (!count) return 77;
   std::vector<VkPhysicalDevice> physical(count);
   check(vkEnumeratePhysicalDevices(instance, &count, physical.data()), "devices");
   VkPhysicalDeviceFeatures available; vkGetPhysicalDeviceFeatures(physical[0], &available);
   if (!available.largePoints || !available.geometryShader) return 77;
   VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(physical[0], &props);
   if (props.limits.pointSizeRange[1] < 16.0f) return 1;
   vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &count, nullptr);
   std::vector<VkQueueFamilyProperties> families(count);
   vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &count, families.data());
   uint32_t family = 0; while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family++;
   if (family == count) return 77;
   float priority = 1; VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
   qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
   VkPhysicalDeviceFeatures enabled = {}; enabled.largePoints = enabled.geometryShader = VK_TRUE;
   VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
   dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci; dci.pEnabledFeatures = &enabled;
   VkDevice device; check(vkCreateDevice(physical[0], &dci, nullptr, &device), "device");
#define DFN(n) auto n = (PFN_##n)vkGetDeviceProcAddr(device, #n)
   DFN(vkGetDeviceQueue); DFN(vkCreateShaderModule); DFN(vkCreatePipelineLayout);
   DFN(vkCreateRenderPass); DFN(vkCreateGraphicsPipelines); DFN(vkCreateImage);
   DFN(vkGetImageMemoryRequirements); DFN(vkAllocateMemory); DFN(vkBindImageMemory);
   DFN(vkCreateImageView); DFN(vkCreateFramebuffer); DFN(vkCreateBuffer);
   DFN(vkGetBufferMemoryRequirements); DFN(vkBindBufferMemory); DFN(vkCreateCommandPool);
   DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkCmdBeginRenderPass);
   DFN(vkCmdBindPipeline); DFN(vkCmdDraw); DFN(vkCmdEndRenderPass);
   DFN(vkCmdCopyImageToBuffer); DFN(vkCmdPipelineBarrier); DFN(vkEndCommandBuffer);
   DFN(vkQueueSubmit); DFN(vkQueueWaitIdle); DFN(vkMapMemory); DFN(vkUnmapMemory);
   DFN(vkDestroyCommandPool); DFN(vkDestroyFramebuffer); DFN(vkDestroyImageView);
   DFN(vkDestroyImage); DFN(vkDestroyBuffer); DFN(vkFreeMemory); DFN(vkDestroyPipeline);
   DFN(vkDestroyRenderPass); DFN(vkDestroyPipelineLayout); DFN(vkDestroyShaderModule);
   DFN(vkDestroyDevice);
   VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
   auto vs_code = read_spv(argv[2]), fs_code = read_spv(argv[3]);
   auto make_module = [&](const std::vector<uint32_t> &code) {
      VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      ci.codeSize = code.size() * 4; ci.pCode = code.data(); VkShaderModule m;
      check(vkCreateShaderModule(device, &ci, nullptr, &m), "shader"); return m;
   };
   VkShaderModule vs = make_module(vs_code), fs = make_module(fs_code);
   VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   VkPipelineLayout layout; check(vkCreatePipelineLayout(device, &plci, nullptr, &layout), "layout");
   VkAttachmentDescription attachment = {};
   attachment.format = VK_FORMAT_R8G8B8A8_UNORM; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
   attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription subpass = {}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &ref;
   VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
   rpci.attachmentCount = 1; rpci.pAttachments = &attachment; rpci.subpassCount = 1; rpci.pSubpasses = &subpass;
   VkRenderPass pass; check(vkCreateRenderPass(device, &rpci, nullptr, &pass), "render pass");
   VkPhysicalDeviceMemoryProperties memory_props; vkGetPhysicalDeviceMemoryProperties(physical[0], &memory_props);
   auto memory_type = [&](uint32_t bits, VkMemoryPropertyFlags required) {
      for (uint32_t i = 0; i < memory_props.memoryTypeCount; i++)
         if ((bits & (1u << i)) && (memory_props.memoryTypes[i].propertyFlags & required) == required) return i;
      return UINT32_MAX;
   };
   VkImageCreateInfo image_ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image_ci.imageType = VK_IMAGE_TYPE_2D;
   image_ci.format = VK_FORMAT_R8G8B8A8_UNORM; image_ci.extent = {64, 64, 1}; image_ci.mipLevels = 1;
   image_ci.arrayLayers = 1; image_ci.samples = VK_SAMPLE_COUNT_1_BIT; image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
   image_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   VkImage image; check(vkCreateImage(device, &image_ci, nullptr, &image), "image");
   VkMemoryRequirements image_req; vkGetImageMemoryRequirements(device, image, &image_req);
   VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = image_req.size;
   mai.memoryTypeIndex = memory_type(image_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   VkDeviceMemory image_mem; check(vkAllocateMemory(device, &mai, nullptr, &image_mem), "image memory");
   check(vkBindImageMemory(device, image, image_mem, 0), "bind image");
   VkImageViewCreateInfo ivci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; ivci.image = image;
   ivci.viewType = VK_IMAGE_VIEW_TYPE_2D; ivci.format = image_ci.format;
   ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   VkImageView view; check(vkCreateImageView(device, &ivci, nullptr, &view), "view");
   VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fci.renderPass = pass;
   fci.attachmentCount = 1; fci.pAttachments = &view; fci.width = fci.height = 64; fci.layers = 1;
   VkFramebuffer framebuffer; check(vkCreateFramebuffer(device, &fci, nullptr, &framebuffer), "framebuffer");
   VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = 64 * 64 * 4; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   VkBuffer buffer; check(vkCreateBuffer(device, &bci, nullptr, &buffer), "buffer");
   VkMemoryRequirements buffer_req; vkGetBufferMemoryRequirements(device, buffer, &buffer_req);
   mai.allocationSize = buffer_req.size; mai.memoryTypeIndex = memory_type(buffer_req.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   VkDeviceMemory buffer_mem; check(vkAllocateMemory(device, &mai, nullptr, &buffer_mem), "buffer memory");
   check(vkBindBufferMemory(device, buffer, buffer_mem, 0), "bind buffer");
   VkPipelineShaderStageCreateInfo stages[2] = {};
   stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
   stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};
   VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
   VkViewport viewport = {0, 0, 64, 64, 0, 1}; VkRect2D scissor = {{0, 0}, {64, 64}};
   VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = vp.scissorCount = 1; vp.pViewports = &viewport; vp.pScissors = &scissor;
   VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
   rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_FRONT_AND_BACK;
   rs.depthBiasEnable = VK_TRUE; rs.depthBiasConstantFactor = 1024.0f; rs.depthBiasSlopeFactor = 32.0f;
   rs.lineWidth = 1;
   VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
   VkPipelineColorBlendAttachmentState cba = {}; cba.colorWriteMask = 15;
   VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &cba;
   VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gp.stageCount = 2; gp.pStages = stages;
   gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp; gp.pRasterizationState = &rs;
   gp.pMultisampleState = &ms; gp.pColorBlendState = &cb; gp.layout = layout; gp.renderPass = pass;
   VkPipeline pipeline; check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline), "pipeline");
   VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpci.queueFamilyIndex = family;
   VkCommandPool pool; check(vkCreateCommandPool(device, &cpci, nullptr, &pool), "command pool");
   VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
   VkCommandBuffer cmd; check(vkAllocateCommandBuffers(device, &cbai, &cmd), "command buffer");
   VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(cmd, &begin), "begin");
   VkClearValue clear = {}; VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rbi.renderPass = pass;
   rbi.framebuffer = framebuffer; rbi.renderArea.extent = {64, 64}; rbi.clearValueCount = 1; rbi.pClearValues = &clear;
   vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE); vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdDraw(cmd, 1, 1, 0, 0); vkCmdEndRenderPass(cmd);
   VkBufferImageCopy copy = {}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {64, 64, 1};
   vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
   VkMemoryBarrier host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER}; host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
   check(vkEndCommandBuffer(cmd), "end"); VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
   check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "submit"); check(vkQueueWaitIdle(queue), "wait");
   uint8_t *pixels; check(vkMapMemory(device, buffer_mem, 0, VK_WHOLE_SIZE, 0, (void **)&pixels), "map");
   unsigned covered = 0; uint8_t min_r = 255, max_r = 0, min_g = 255, max_g = 0;
   for (unsigned i = 0; i < 64 * 64; i++) if (pixels[i * 4 + 3]) { covered++; min_r = min_r < pixels[i*4] ? min_r : pixels[i*4]; max_r = max_r > pixels[i*4] ? max_r : pixels[i*4]; min_g = min_g < pixels[i*4+1] ? min_g : pixels[i*4+1]; max_g = max_g > pixels[i*4+1] ? max_g : pixels[i*4+1]; }
   std::printf("%s: covered=%u PointCoord R=%u..%u G=%u..%u\n", props.deviceName, covered, min_r, max_r, min_g, max_g);
   if (covered != 256 || min_r > 16 || min_g > 16 || max_r < 239 || max_g < 239) return 1;
   vkUnmapMemory(device, buffer_mem); vkDestroyCommandPool(device, pool, nullptr); vkDestroyPipeline(device, pipeline, nullptr);
   vkDestroyFramebuffer(device, framebuffer, nullptr); vkDestroyImageView(device, view, nullptr); vkDestroyImage(device, image, nullptr);
   vkDestroyBuffer(device, buffer, nullptr); vkFreeMemory(device, buffer_mem, nullptr); vkFreeMemory(device, image_mem, nullptr);
   vkDestroyRenderPass(device, pass, nullptr); vkDestroyPipelineLayout(device, layout, nullptr);
   vkDestroyShaderModule(device, fs, nullptr); vkDestroyShaderModule(device, vs, nullptr); vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr); FreeLibrary(library); return 0;
}
