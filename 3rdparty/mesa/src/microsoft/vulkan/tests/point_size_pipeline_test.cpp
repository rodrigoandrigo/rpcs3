/* SPDX-License-Identifier: MIT */
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
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
   /* DLL, VS, TCS, GS, FS, then the TES variants. This uses the newly built
    * ICD directly; installed loader/registry configuration is irrelevant. */
   if (argc < 7)
      return 1;
   HMODULE library = LoadLibraryA(argv[1]);
   if (!library) {
      std::fprintf(stderr, "LoadLibrary: Win32 %lu\n", GetLastError());
      return 1;
   }
   auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      GetProcAddress(library, "vk_icdGetInstanceProcAddr"));
   if (!get)
      return 1;
   auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
   VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
   app.pApplicationName = "Dozen PointSize pipeline regression";
   app.apiVersion = VK_API_VERSION_1_2;
   VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
   instance_info.pApplicationInfo = &app;
   VkInstance instance;
   check(create_instance(&instance_info, nullptr, &instance), "vkCreateInstance");
#define INSTANCE_FN(name) auto name = reinterpret_cast<PFN_##name>(get(instance, #name))
   INSTANCE_FN(vkEnumeratePhysicalDevices);
   INSTANCE_FN(vkGetPhysicalDeviceFeatures);
   INSTANCE_FN(vkGetPhysicalDeviceProperties);
   INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties);
   INSTANCE_FN(vkCreateDevice);
   INSTANCE_FN(vkDestroyInstance);
   INSTANCE_FN(vkGetDeviceProcAddr);
   uint32_t count = 0;
   check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate device count");
   if (!count) {
      vkDestroyInstance(instance, nullptr);
      FreeLibrary(library);
      return 77;
   }
   std::vector<VkPhysicalDevice> physical(count);
   check(vkEnumeratePhysicalDevices(instance, &count, physical.data()), "enumerate devices");
   VkPhysicalDeviceFeatures available;
   vkGetPhysicalDeviceFeatures(physical[0], &available);
   if (!available.geometryShader || !available.tessellationShader ||
       !available.shaderTessellationAndGeometryPointSize || !available.largePoints)
      return 77;
   VkPhysicalDeviceProperties properties;
   vkGetPhysicalDeviceProperties(physical[0], &properties);
   std::printf("%s: geometry=%u tessellation=%u PointSize=%u largePoints=%u\n",
      properties.deviceName, available.geometryShader, available.tessellationShader,
      available.shaderTessellationAndGeometryPointSize, available.largePoints);
   vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &count, nullptr);
   std::vector<VkQueueFamilyProperties> queues(count);
   vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &count, queues.data());
   uint32_t family = 0;
   while (family < count && !(queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
      family++;
   if (family == count)
      return 77;
   float priority = 1;
   VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
   queue_info.queueFamilyIndex = family;
   queue_info.queueCount = 1;
   queue_info.pQueuePriorities = &priority;
   VkPhysicalDeviceFeatures features = {};
   features.geometryShader = features.tessellationShader =
      features.shaderTessellationAndGeometryPointSize = VK_TRUE;
   features.largePoints = VK_TRUE;
   VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
   device_info.queueCreateInfoCount = 1;
   device_info.pQueueCreateInfos = &queue_info;
   device_info.pEnabledFeatures = &features;
   VkDevice device;
   check(vkCreateDevice(physical[0], &device_info, nullptr, &device), "vkCreateDevice");
#define DEVICE_FN(name) auto name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name))
   DEVICE_FN(vkCreateShaderModule);
   DEVICE_FN(vkDestroyShaderModule);
   DEVICE_FN(vkCreatePipelineLayout);
   DEVICE_FN(vkDestroyPipelineLayout);
   DEVICE_FN(vkCreateRenderPass);
   DEVICE_FN(vkDestroyRenderPass);
   DEVICE_FN(vkCreateGraphicsPipelines);
   DEVICE_FN(vkDestroyPipeline);
   DEVICE_FN(vkDestroyDevice);
   std::vector<VkShaderModule> modules;
   for (int i = 2; i < argc; i++) {
      std::ifstream file(argv[i], std::ios::binary | std::ios::ate);
      if (!file)
         return 1;
      size_t length = static_cast<size_t>(file.tellg());
      if (!length || length % 4)
         return 1;
      std::vector<uint32_t> code(length / 4);
      file.seekg(0);
      file.read(reinterpret_cast<char *>(code.data()), length);
      VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      info.codeSize = length;
      info.pCode = code.data();
      VkShaderModule module;
      check(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
      modules.push_back(module);
   }
   VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   VkPipelineLayout layout;
   check(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout), "vkCreatePipelineLayout");
   VkAttachmentDescription attachment = {};
   attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
   attachment.samples = VK_SAMPLE_COUNT_1_BIT;
   attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription subpass = {};
   subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   subpass.colorAttachmentCount = 1;
   subpass.pColorAttachments = &reference;
   VkRenderPassCreateInfo pass_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
   pass_info.attachmentCount = 1;
   pass_info.pAttachments = &attachment;
   pass_info.subpassCount = 1;
   pass_info.pSubpasses = &subpass;
   VkRenderPass pass;
   check(vkCreateRenderPass(device, &pass_info, nullptr, &pass), "vkCreateRenderPass");
   VkPipelineVertexInputStateCreateInfo vertex = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
   VkPipelineTessellationStateCreateInfo tess = {VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
   tess.patchControlPoints = 3;
   VkViewport viewport = {0, 0, 64, 64, 0, 1};
   VkRect2D scissor = {{0, 0}, {64, 64}};
   VkPipelineViewportStateCreateInfo view = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
   view.viewportCount = view.scissorCount = 1;
   view.pViewports = &viewport;
   view.pScissors = &scissor;
   VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
   raster.polygonMode = VK_POLYGON_MODE_FILL;
   raster.lineWidth = 1;
   VkPipelineMultisampleStateCreateInfo sample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
   sample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
   VkPipelineColorBlendAttachmentState blend_attachment = {};
   blend_attachment.colorWriteMask = 15;
   VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
   blend.attachmentCount = 1;
   blend.pAttachments = &blend_attachment;
   unsigned cases = 0;
   for (size_t variant = 0; variant <= modules.size() - 4; variant++) {
      bool tessellated = variant != 0;
      std::vector<VkPipelineShaderStageCreateInfo> stages;
      auto add = [&](VkShaderStageFlagBits stage, size_t module) {
         VkPipelineShaderStageCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
         info.stage = stage;
         info.module = modules[module];
         info.pName = "main";
         stages.push_back(info);
      };
      add(VK_SHADER_STAGE_VERTEX_BIT, 0);
      if (tessellated) {
         add(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, 1);
         add(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, variant + 3);
      } else {
         add(VK_SHADER_STAGE_GEOMETRY_BIT, 2);
      }
      add(VK_SHADER_STAGE_FRAGMENT_BIT, 3);
      assembly.topology = tessellated ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      /* TES variants alternate ordinary and PointMode. Also cover the full
       * VS -> TCS -> TES -> GS -> FS chain for every PointMode variant. */
      unsigned geometry_cases = tessellated && variant % 2 == 0 ? 2 : 1;
      for (unsigned geometry = 0; geometry < geometry_cases; geometry++) {
      if (geometry) {
         VkPipelineShaderStageCreateInfo gs = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
         gs.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
         gs.module = modules[2];
         gs.pName = "main";
         stages.insert(stages.end() - 1, gs);
      }
      for (unsigned origin = 0; origin < 2; origin++) {
         VkPipelineTessellationDomainOriginStateCreateInfo domain = {
            VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO};
         domain.domainOrigin = static_cast<VkTessellationDomainOrigin>(origin);
         tess.pNext = &domain;
         VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
         info.stageCount = static_cast<uint32_t>(stages.size());
         info.pStages = stages.data();
         info.pVertexInputState = &vertex;
         info.pInputAssemblyState = &assembly;
         info.pTessellationState = tessellated ? &tess : nullptr;
         info.pViewportState = &view;
         info.pRasterizationState = &raster;
         info.pMultisampleState = &sample;
         info.pColorBlendState = &blend;
         info.layout = layout;
         info.renderPass = pass;
         VkPipeline pipeline;
         std::printf("pipeline variant=%zu origin=%u geometry=%u\n", variant, origin, geometry);
         check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr,
                                        &pipeline), "vkCreateGraphicsPipelines");
         vkDestroyPipeline(device, pipeline, nullptr);
         cases++;
      }
      }
   }
   vkDestroyRenderPass(device, pass, nullptr);
   vkDestroyPipelineLayout(device, layout, nullptr);
   for (auto module : modules)
      vkDestroyShaderModule(device, module, nullptr);
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   FreeLibrary(library);
   std::printf("%u native PointSize/tessellation pipelines passed\n", cases);
   return 0;
}
