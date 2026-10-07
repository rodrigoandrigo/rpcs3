// Native-PC offscreen extension probe; never linked into RPCS3 or Mesa.
// SPDX-License-Identifier: MIT
static void dozen_seven_smoke(VkInstance instance, VkPhysicalDevice physical,
                             VkDevice device, VkQueue queue, uint32_t family,
                             PFN_vkGetInstanceProcAddr get,
                             PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr,
                             const std::filesystem::path& directory,
                             bool extended1, bool conservative, bool a4b4g4r4)
{
#define SEVEN_FN(name) auto name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name)); if (!name) throw std::runtime_error(#name " missing")
   SEVEN_FN(vkCreateImage); SEVEN_FN(vkGetImageMemoryRequirements); SEVEN_FN(vkBindImageMemory);
   SEVEN_FN(vkCreateImageView); SEVEN_FN(vkCreateBuffer); SEVEN_FN(vkGetBufferMemoryRequirements);
   SEVEN_FN(vkBindBufferMemory); SEVEN_FN(vkAllocateMemory); SEVEN_FN(vkFreeMemory);
   SEVEN_FN(vkMapMemory); SEVEN_FN(vkUnmapMemory); SEVEN_FN(vkFlushMappedMemoryRanges);
   SEVEN_FN(vkInvalidateMappedMemoryRanges); SEVEN_FN(vkCreateShaderModule);
   SEVEN_FN(vkCreatePipelineLayout); SEVEN_FN(vkCreateGraphicsPipelines);
   SEVEN_FN(vkCreateRenderPass); SEVEN_FN(vkCreateFramebuffer); SEVEN_FN(vkCreateCommandPool);
   SEVEN_FN(vkAllocateCommandBuffers); SEVEN_FN(vkBeginCommandBuffer); SEVEN_FN(vkEndCommandBuffer);
   SEVEN_FN(vkCmdBeginRenderPass); SEVEN_FN(vkCmdEndRenderPass); SEVEN_FN(vkCmdBindPipeline);
   SEVEN_FN(vkCmdSetVertexInputEXT); SEVEN_FN(vkCmdBindVertexBuffers);
   SEVEN_FN(vkCmdSetRasterizerDiscardEnableEXT); SEVEN_FN(vkCmdSetDepthBiasEnableEXT);
   SEVEN_FN(vkCmdSetPrimitiveRestartEnableEXT); SEVEN_FN(vkCmdDraw);
   SEVEN_FN(vkCmdCopyImageToBuffer); SEVEN_FN(vkCmdPipelineBarrier);
   SEVEN_FN(vkCmdClearColorImage);
   SEVEN_FN(vkQueueSubmit); SEVEN_FN(vkQueueWaitIdle);
   SEVEN_FN(vkDestroyCommandPool); SEVEN_FN(vkDestroyFramebuffer); SEVEN_FN(vkDestroyRenderPass);
   SEVEN_FN(vkDestroyPipeline); SEVEN_FN(vkDestroyPipelineLayout); SEVEN_FN(vkDestroyShaderModule);
   SEVEN_FN(vkDestroyImageView); SEVEN_FN(vkDestroyImage); SEVEN_FN(vkDestroyBuffer);
   auto memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get(instance, "vkGetPhysicalDeviceMemoryProperties"));
   VkPhysicalDeviceMemoryProperties memory{}; memory_properties(physical, &memory);
   auto allocate = [&](const VkMemoryRequirements& req, VkMemoryPropertyFlags flags) {
      uint32_t type=0;
      while (type<memory.memoryTypeCount && (!(req.memoryTypeBits & (1u<<type)) ||
             (memory.memoryTypes[type].propertyFlags & flags)!=flags)) ++type;
      if (type==memory.memoryTypeCount) throw std::runtime_error("No suitable graphics probe memory");
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=type;
      VkDeviceMemory result{}; check(vkAllocateMemory(device, &ai, nullptr, &result), "seven allocate"); return result;
   };
   auto load_shader = [&](const char* name) {
      std::ifstream file(directory/name, std::ios::binary | std::ios::ate);
      auto size=file.tellg(); if (!file || size<=0 || size%4) throw std::runtime_error("Invalid graphics SPIR-V");
      std::vector<uint32_t> code(static_cast<size_t>(size)/4); file.seekg(0);
      if (!file.read(reinterpret_cast<char*>(code.data()), size)) throw std::runtime_error("Graphics SPIR-V read failed");
      VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize=code.size()*4; info.pCode=code.data();
      VkShaderModule shader{}; check(vkCreateShaderModule(device, &info, nullptr, &shader), "seven shader"); return shader;
   };
   VkShaderModule vs=load_shader("dozen-seven.vert.spv"), fs=load_shader("dozen-seven.frag.spv");
   VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
   image_info.imageType=VK_IMAGE_TYPE_2D; image_info.format=VK_FORMAT_R8G8B8A8_UNORM;
   image_info.extent={32,32,1}; image_info.mipLevels=image_info.arrayLayers=1;
   image_info.samples=VK_SAMPLE_COUNT_1_BIT; image_info.tiling=VK_IMAGE_TILING_OPTIMAL;
   image_info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   VkImage image{}; check(vkCreateImage(device, &image_info, nullptr, &image), "seven image");
   VkMemoryRequirements req{}; vkGetImageMemoryRequirements(device, image, &req);
   VkDeviceMemory image_memory=allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   check(vkBindImageMemory(device, image, image_memory, 0), "seven bind image");
   VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
   view_info.image=image; view_info.viewType=VK_IMAGE_VIEW_TYPE_2D; view_info.format=image_info.format;
   view_info.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
   VkImageView view{}; check(vkCreateImageView(device, &view_info, nullptr, &view), "seven view");
   VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=8192;
   bi.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   VkBuffer buffer{}; check(vkCreateBuffer(device, &bi, nullptr, &buffer), "seven buffer");
   vkGetBufferMemoryRequirements(device, buffer, &req);
   VkDeviceMemory buffer_memory=allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   check(vkBindBufferMemory(device, buffer, buffer_memory, 0), "seven bind buffer");
   void* mapped{}; check(vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped), "seven map");
   float positions[]={-1,-1,3,-1,-1,3}; float outside[]={4,4,6,4,4,6};
   std::memcpy(mapped, positions, sizeof(positions)); std::memcpy(static_cast<char*>(mapped)+64, outside, sizeof(outside));
   int16_t scaled[]={-1,-1,0,1,3,-1,0,1,-1,3,0,1};
   std::memcpy(static_cast<char*>(mapped)+128, scaled, sizeof(scaled));
   float point_positions[]={-.5f,-.5f,.5f,-.5f,0,.5f};
   std::memcpy(static_cast<char*>(mapped)+192, point_positions, sizeof(point_positions));
   VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory=buffer_memory; range.size=VK_WHOLE_SIZE;
   check(vkFlushMappedMemoryRanges(device, 1, &range), "seven flush");
   vkUnmapMemory(device, buffer_memory);
   VkAttachmentDescription attachment{}; attachment.format=image_info.format; attachment.samples=VK_SAMPLE_COUNT_1_BIT;
   attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
   attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription subpass{}; subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
   subpass.colorAttachmentCount=1; subpass.pColorAttachments=&reference;
   VkSubpassDependency dependencies[2]{};
   dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL; dependencies[0].dstSubpass=0;
   dependencies[0].srcStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT; dependencies[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
   dependencies[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; dependencies[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   dependencies[1].srcSubpass=0; dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;
   dependencies[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dependencies[1].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;
   dependencies[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dependencies[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
   VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; ri.attachmentCount=1; ri.pAttachments=&attachment;
   ri.subpassCount=1; ri.pSubpasses=&subpass; ri.dependencyCount=2; ri.pDependencies=dependencies;
   VkRenderPass pass{}; check(vkCreateRenderPass(device, &ri, nullptr, &pass), "seven render pass");
   VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fi.renderPass=pass; fi.attachmentCount=1;
   fi.pAttachments=&view; fi.width=fi.height=32; fi.layers=1;
   VkFramebuffer framebuffer{}; check(vkCreateFramebuffer(device, &fi, nullptr, &framebuffer), "seven framebuffer");
   VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   VkPipelineLayout layout{}; check(vkCreatePipelineLayout(device, &li, nullptr, &layout), "seven layout");
   VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
   stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=vs; stages[0].pName="main";
   stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=fs; stages[1].pName="main";
   VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   VkViewport viewport{0,0,32,32,0,1}; VkRect2D scissor{{0,0},{32,32}};
   VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
   vp.viewportCount=vp.scissorCount=1; vp.pViewports=&viewport; vp.pScissors=&scissor;
   VkPipelineRasterizationStateCreateInfo rast{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
   rast.polygonMode=VK_POLYGON_MODE_FILL; rast.lineWidth=1; rast.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;
   // Intentionally ignored initial discard must not omit the fragment shader.
   rast.rasterizerDiscardEnable=VK_TRUE;
   VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
   VkPipelineColorBlendAttachmentState blend{}; blend.colorWriteMask=15;
   VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount=1; cb.pAttachments=&blend;
   VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
   std::vector<VkDynamicState> dynamics={VK_DYNAMIC_STATE_VERTEX_INPUT_EXT,VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
      VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
   if (extended1) {
      dynamics.insert(dynamics.end(), {VK_DYNAMIC_STATE_CULL_MODE,VK_DYNAMIC_STATE_FRONT_FACE,VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
         VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
         VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
         VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,VK_DYNAMIC_STATE_STENCIL_OP,VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE});
      vp.viewportCount=vp.scissorCount=0;
   }
   VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
   dy.dynamicStateCount=static_cast<uint32_t>(dynamics.size()); dy.pDynamicStates=dynamics.data();
   VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
   pi.stageCount=2; pi.pStages=stages; pi.pInputAssemblyState=&ia; pi.pViewportState=&vp;
   pi.pRasterizationState=&rast; pi.pMultisampleState=&ms; pi.pColorBlendState=&cb; pi.pDepthStencilState=&ds;
   pi.pDynamicState=&dy; pi.layout=layout; pi.renderPass=pass;
   VkPipeline pipeline{}; check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline), "seven dynamic graphics pipeline");
   VkPipeline conservative_pipeline{};
   if (conservative) {
      VkPipelineRasterizationConservativeStateCreateInfoEXT cr{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT};
      cr.conservativeRasterizationMode=VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT; rast.pNext=&cr;
      check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &conservative_pipeline), "seven conservative pipeline");
      rast.pNext=nullptr;
   }
   VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex=family;
   VkPipeline point_pipeline{};
   rast.polygonMode=VK_POLYGON_MODE_POINT;
   check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pi,nullptr,&point_pipeline), "seven implicit point GS pipeline");
   rast.polygonMode=VK_POLYGON_MODE_FILL;
   VkCommandPool pool{}; check(vkCreateCommandPool(device, &pci, nullptr, &pool), "seven pool");
   for (uint32_t test=0; test<12; test++) {
      if (test==5 && !conservative) continue;
      VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool=pool;
      ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1;
      VkCommandBuffer commands{}; check(vkAllocateCommandBuffers(device, &ai, &commands), "seven commands");
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(commands, &begin), "seven begin");
      VkClearValue clear{}; clear.color.float32[3]=1;
      VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; render.renderPass=pass; render.framebuffer=framebuffer;
      render.renderArea=scissor; render.clearValueCount=1; render.pClearValues=&clear;
      vkCmdBeginRenderPass(commands, &render, VK_SUBPASS_CONTENTS_INLINE);
      VkVertexInputBindingDescription2EXT binding{VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT};
      binding.binding=test==3?1:0; binding.stride=8; binding.inputRate=VK_VERTEX_INPUT_RATE_VERTEX; binding.divisor=1;
      VkVertexInputAttributeDescription2EXT attribute{VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT};
      attribute.location=0; attribute.binding=binding.binding; attribute.format=VK_FORMAT_R32G32_SFLOAT;
      attribute.offset=test==2?64:0;
      if(test>=8) attribute.offset=192;
      if (test==6) {
         attribute.format=VK_FORMAT_R16G16B16A16_SSCALED;
         attribute.offset=128;
      }
      // Set before BindPipeline to verify binding does not overwrite dynamic VI.
      vkCmdSetVertexInputEXT(commands, 1, &binding, 1, &attribute);
      VkDeviceSize offset=0, size=256, stride=8;
      if (extended1) {
         SEVEN_FN(vkCmdBindVertexBuffers2EXT);
         vkCmdBindVertexBuffers2EXT(commands, binding.binding, 1, &buffer, &offset, &size, &stride);
      } else {
         vkCmdBindVertexBuffers(commands, binding.binding, 1, &buffer, &offset);
      }
      vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, test>=8?point_pipeline:(test==5?conservative_pipeline:pipeline));
      vkCmdSetRasterizerDiscardEnableEXT(commands, test==1);
      vkCmdSetDepthBiasEnableEXT(commands, test==11); vkCmdSetPrimitiveRestartEnableEXT(commands, VK_FALSE);
      if (extended1) {
         SEVEN_FN(vkCmdSetCullModeEXT); SEVEN_FN(vkCmdSetFrontFaceEXT); SEVEN_FN(vkCmdSetPrimitiveTopologyEXT);
         SEVEN_FN(vkCmdSetViewportWithCountEXT); SEVEN_FN(vkCmdSetScissorWithCountEXT);
         SEVEN_FN(vkCmdSetDepthTestEnableEXT); SEVEN_FN(vkCmdSetDepthWriteEnableEXT); SEVEN_FN(vkCmdSetDepthCompareOpEXT);
         SEVEN_FN(vkCmdSetDepthBoundsTestEnableEXT); SEVEN_FN(vkCmdSetStencilTestEnableEXT); SEVEN_FN(vkCmdSetStencilOpEXT);
         vkCmdSetCullModeEXT(commands, test==4?VK_CULL_MODE_FRONT_AND_BACK:((test==9 || test==10)?VK_CULL_MODE_FRONT_BIT:VK_CULL_MODE_NONE));
         vkCmdSetFrontFaceEXT(commands, test==10?VK_FRONT_FACE_CLOCKWISE:VK_FRONT_FACE_COUNTER_CLOCKWISE);
         vkCmdSetPrimitiveTopologyEXT(commands, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
         vkCmdSetViewportWithCountEXT(commands, 1, &viewport); vkCmdSetScissorWithCountEXT(commands, 1, &scissor);
         vkCmdSetDepthTestEnableEXT(commands,VK_FALSE); vkCmdSetDepthWriteEnableEXT(commands,VK_FALSE);
         vkCmdSetDepthCompareOpEXT(commands,VK_COMPARE_OP_ALWAYS); vkCmdSetDepthBoundsTestEnableEXT(commands,VK_FALSE);
         vkCmdSetStencilTestEnableEXT(commands,VK_FALSE);
         vkCmdSetStencilOpEXT(commands,VK_STENCIL_FACE_FRONT_AND_BACK,VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,VK_COMPARE_OP_ALWAYS);
      }
      vkCmdDraw(commands,3,1,0,0); vkCmdEndRenderPass(commands);
      VkBufferImageCopy copy{}; copy.bufferOffset=4096; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={32,32,1};
      vkCmdCopyImageToBuffer(commands,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
      VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
      host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; host.buffer=buffer; host.offset=4096; host.size=4096;
      vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
      check(vkEndCommandBuffer(commands), "seven end");
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&commands;
      check(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE), "seven submit"); check(vkQueueWaitIdle(queue), "seven wait");
      check(vkMapMemory(device,buffer_memory,0,VK_WHOLE_SIZE,0,&mapped), "seven readback");
      check(vkInvalidateMappedMemoryRanges(device,1,&range), "seven invalidate");
      auto pixels=reinterpret_cast<const uint32_t*>(static_cast<char*>(mapped)+4096);
      bool green=test!=1 && test!=2 && !(test==4 && extended1);
      uint32_t green_count=0;
      for (uint32_t i=0; i<1024; i++) {
         green_count+=pixels[i]==0xff00ff00u;
         if(test<8 && pixels[i]!=(green?0xff00ff00u:0xff000000u))
            throw std::runtime_error("Dynamic graphics mismatch test="+std::to_string(test)+" pixel="+std::to_string(i)+" value="+std::to_string(pixels[i]));
         if(test>=8 && pixels[i]!=0xff00ff00u && pixels[i]!=0xff000000u)
            throw std::runtime_error("Unexpected point-fill pixel");
      }
      if(test>=8 && green_count!=((test==9 && extended1)?0u:3u))
         throw std::runtime_error("Point-fill dynamic culling mismatch test="+std::to_string(test)+" green="+std::to_string(green_count));
      vkUnmapMemory(device,buffer_memory);
      std::cout << "PASS: dynamic graphics readback case " << test << '\n';
   }
   for (uint32_t format_test=0; format_test<(a4b4g4r4?2u:1u); format_test++) {
      VkImageCreateInfo packed_info=image_info;
      packed_info.format=format_test?VK_FORMAT_A4B4G4R4_UNORM_PACK16_EXT:VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT;
      packed_info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
      VkImage packed{}; check(vkCreateImage(device,&packed_info,nullptr,&packed), "4444 image");
      vkGetImageMemoryRequirements(device,packed,&req); VkDeviceMemory packed_memory=allocate(req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      check(vkBindImageMemory(device,packed,packed_memory,0), "4444 bind");
      VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool=pool;
      ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1;
      VkCommandBuffer commands{}; check(vkAllocateCommandBuffers(device,&ai,&commands), "4444 commands");
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(commands,&begin), "4444 begin");
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.image=packed;
      barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
      barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
      vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
      VkClearColorValue red{}; red.float32[0]=red.float32[3]=1;
      vkCmdClearColorImage(commands,packed,VK_IMAGE_LAYOUT_GENERAL,&red,1,&barrier.subresourceRange);
      barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL; barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
      vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
      VkBufferImageCopy copy{}; copy.bufferOffset=4096; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={32,32,1};
      vkCmdCopyImageToBuffer(commands,packed,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&copy);
      VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; host.buffer=buffer; host.offset=4096; host.size=2048;
      host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
      check(vkEndCommandBuffer(commands), "4444 end");
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&commands;
      check(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE), "4444 submit"); check(vkQueueWaitIdle(queue), "4444 wait");
      check(vkMapMemory(device,buffer_memory,0,VK_WHOLE_SIZE,0,&mapped), "4444 readback");
      check(vkInvalidateMappedMemoryRanges(device,1,&range), "4444 invalidate");
      auto packed_pixels=reinterpret_cast<const uint16_t*>(static_cast<char*>(mapped)+4096);
      uint16_t expected=format_test?0xf00f:0xff00;
      for(uint32_t i=0;i<1024;i++) if(packed_pixels[i]!=expected)
         throw std::runtime_error("4444 packed channel mismatch format="+std::to_string(format_test)+" value="+std::to_string(packed_pixels[i]));
      vkUnmapMemory(device,buffer_memory);
      vkDestroyImage(device,packed,nullptr); vkFreeMemory(device,packed_memory,nullptr);
      std::cout << "PASS: 4444 packed clear/readback format " << format_test << '\n';
   }
   if(!a4b4g4r4) std::cout << "SKIPPED: native A4B4G4R4 format unavailable\n";
   vkDestroyCommandPool(device,pool,nullptr); vkDestroyPipeline(device,pipeline,nullptr);
   vkDestroyPipeline(device,point_pipeline,nullptr);
   if(conservative_pipeline) vkDestroyPipeline(device,conservative_pipeline,nullptr);
   vkDestroyPipelineLayout(device,layout,nullptr); vkDestroyShaderModule(device,vs,nullptr); vkDestroyShaderModule(device,fs,nullptr);
   vkDestroyFramebuffer(device,framebuffer,nullptr); vkDestroyRenderPass(device,pass,nullptr);
   vkDestroyImageView(device,view,nullptr); vkDestroyImage(device,image,nullptr); vkDestroyBuffer(device,buffer,nullptr);
   vkFreeMemory(device,image_memory,nullptr); vkFreeMemory(device,buffer_memory,nullptr);
#undef SEVEN_FN
}

static void dozen_seven_wsi_smoke(VkInstance instance, VkPhysicalDevice physical,
                                 VkDevice device, VkQueue queue, uint32_t family,
                                 PFN_vkGetInstanceProcAddr get,
                                 PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr, HMODULE module)
{
#define WSI_I(name) auto name=reinterpret_cast<PFN_##name>(get(instance,#name)); if(!name) throw std::runtime_error(#name " missing")
#define WSI_D(name) auto name=reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device,#name)); if(!name) throw std::runtime_error(#name " missing")
   WSI_I(vkCreateWin32SurfaceKHR); WSI_I(vkDestroySurfaceKHR); WSI_I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
   WSI_I(vkGetPhysicalDeviceSurfaceSupportKHR);
   WSI_D(vkCreateSwapchainKHR); WSI_D(vkDestroySwapchainKHR); WSI_D(vkGetSwapchainImagesKHR);
   WSI_D(vkCreateImageView); WSI_D(vkDestroyImageView); WSI_D(vkAcquireNextImageKHR);
   WSI_D(vkCreateSemaphore); WSI_D(vkDestroySemaphore); WSI_D(vkQueuePresentKHR);
   WSI_D(vkCreateCommandPool); WSI_D(vkDestroyCommandPool); WSI_D(vkAllocateCommandBuffers);
   WSI_D(vkBeginCommandBuffer); WSI_D(vkEndCommandBuffer); WSI_D(vkCmdPipelineBarrier);
   WSI_D(vkCmdClearColorImage); WSI_D(vkQueueSubmit); WSI_D(vkQueueWaitIdle); WSI_D(vkDeviceWaitIdle);
   auto attach=reinterpret_cast<void(*)(long(__cdecl*)(void*,void*),void*)>(GetProcAddress(module,"mesa_uwp_set_swapchain_attach_callback"));
   auto window_size=reinterpret_cast<void(*)(void*,int,int)>(GetProcAddress(module,"uwp_set_window_reference"));
   if(!attach || !window_size) throw std::runtime_error("Isolated UWP WSI callback exports missing");
   // Accept the composition swapchain without connecting a frontend/XAML panel.
   // This verifies submission, NOT visible presentation or installed UWP use.
   attach([](void*,void*) -> long {return S_OK;},nullptr); window_size(nullptr,64,64);
   HWND window=CreateWindowExW(0,L"STATIC",L"Dozen isolated WSI probe",WS_POPUP,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
   if(!window) throw std::runtime_error("Hidden WSI window Win32="+std::to_string(GetLastError()));
   VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
   surface_info.hinstance=GetModuleHandleW(nullptr); surface_info.hwnd=window;
   VkSurfaceKHR surface{}; check(vkCreateWin32SurfaceKHR(instance,&surface_info,nullptr,&surface), "seven WSI surface");
   VkBool32 supported{}; check(vkGetPhysicalDeviceSurfaceSupportKHR(physical,family,surface,&supported), "seven WSI support");
   if(!supported) throw std::runtime_error("No WSI support for graphics queue");
   VkSurfaceCapabilitiesKHR caps{}; check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps), "seven WSI caps");
   VkFormat formats[]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB};
   VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO}; list.viewFormatCount=2; list.pViewFormats=formats;
   VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; ci.pNext=&list;
   ci.flags=VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR; ci.surface=surface;
   ci.minImageCount=caps.minImageCount<2?2:caps.minImageCount; ci.imageFormat=formats[0]; ci.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   ci.imageExtent=caps.currentExtent; ci.imageArrayLayers=1; ci.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   ci.preTransform=caps.currentTransform; ci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;
   VkSwapchainKHR swapchain{}; check(vkCreateSwapchainKHR(device,&ci,nullptr,&swapchain), "seven mutable swapchain");
   uint32_t count{}; check(vkGetSwapchainImagesKHR(device,swapchain,&count,nullptr), "seven swapchain count");
   std::vector<VkImage> images(count); check(vkGetSwapchainImagesKHR(device,swapchain,&count,images.data()), "seven swapchain images");
   std::vector<VkImageView> views;
   for(auto image:images) for(auto format:formats) {
      VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view_info.image=image;
      view_info.viewType=VK_IMAGE_VIEW_TYPE_2D; view_info.format=format; view_info.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
      VkImageView view{}; check(vkCreateImageView(device,&view_info,nullptr,&view), "seven mutable view"); views.push_back(view);
   }
   VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex=family;
   VkCommandPool pool{}; check(vkCreateCommandPool(device,&pci,nullptr,&pool), "seven WSI pool");
   for(uint64_t id: {1ull,0ull,2ull}) {
      VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VkSemaphore acquired{},rendered{};
      check(vkCreateSemaphore(device,&si,nullptr,&acquired), "seven acquire semaphore");
      check(vkCreateSemaphore(device,&si,nullptr,&rendered), "seven present semaphore");
      uint32_t index{}; check(vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,acquired,VK_NULL_HANDLE,&index), "seven acquire");
      VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool=pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1;
      VkCommandBuffer commands{}; check(vkAllocateCommandBuffers(device,&ai,&commands), "seven WSI commands");
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(commands,&begin), "seven WSI begin");
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.image=images[index];
      barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL; barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
      vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
      VkClearColorValue clear{}; clear.float32[1]=clear.float32[3]=1;
      vkCmdClearColorImage(commands,images[index],VK_IMAGE_LAYOUT_GENERAL,&clear,1,&barrier.subresourceRange);
      barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL; barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=0;
      vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);
      check(vkEndCommandBuffer(commands), "seven WSI end");
      VkPipelineStageFlags wait_stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&commands;
      submit.waitSemaphoreCount=1; submit.pWaitSemaphores=&acquired; submit.pWaitDstStageMask=&wait_stage;
      submit.signalSemaphoreCount=1; submit.pSignalSemaphores=&rendered;
      check(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE), "seven WSI submit");
      VkPresentIdKHR present_id{VK_STRUCTURE_TYPE_PRESENT_ID_KHR}; present_id.swapchainCount=1; present_id.pPresentIds=&id;
      VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; present.pNext=&present_id;
      present.swapchainCount=1; present.pSwapchains=&swapchain; present.pImageIndices=&index;
      present.waitSemaphoreCount=1; present.pWaitSemaphores=&rendered;
      check(vkQueuePresentKHR(queue,&present), "seven identified present"); check(vkDeviceWaitIdle(device), "seven WSI idle");
      vkDestroySemaphore(device,acquired,nullptr); vkDestroySemaphore(device,rendered,nullptr);
      std::cout << "PASS: isolated mutable-format DXGI present id " << id << '\n';
   }
   vkDestroyCommandPool(device,pool,nullptr); for(auto view:views) vkDestroyImageView(device,view,nullptr);
   vkDestroySwapchainKHR(device,swapchain,nullptr); vkDestroySurfaceKHR(instance,surface,nullptr);
   DestroyWindow(window); attach(nullptr,nullptr); window_size(nullptr,0,0);
#undef WSI_I
#undef WSI_D
}
