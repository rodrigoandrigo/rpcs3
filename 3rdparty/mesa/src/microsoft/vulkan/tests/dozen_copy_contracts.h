// SPDX-License-Identifier: MIT
// Internal backend contract probe, not extension conformance/advertising proof.
static void dozen_copy_contracts(VkPhysicalDevice physical, VkDevice device,
   VkQueue queue, uint32_t family, PFN_vkGetInstanceProcAddr get,
   VkInstance instance, PFN_vkGetDeviceProcAddr get_device)
{
#define COPY_FN(name) auto name=reinterpret_cast<PFN_##name>(get_device(device,#name)); if (!name) throw std::runtime_error(#name)
   COPY_FN(vkCreateImage); COPY_FN(vkDestroyImage); COPY_FN(vkGetImageMemoryRequirements);
   COPY_FN(vkAllocateMemory); COPY_FN(vkFreeMemory); COPY_FN(vkBindImageMemory);
   COPY_FN(vkCreateBuffer); COPY_FN(vkDestroyBuffer); COPY_FN(vkGetBufferMemoryRequirements); COPY_FN(vkBindBufferMemory);
   COPY_FN(vkCreateCommandPool); COPY_FN(vkDestroyCommandPool); COPY_FN(vkAllocateCommandBuffers);
   COPY_FN(vkBeginCommandBuffer); COPY_FN(vkEndCommandBuffer); COPY_FN(vkCmdPipelineBarrier);
   COPY_FN(vkCmdUpdateBuffer); COPY_FN(vkCmdCopyBufferToImage); COPY_FN(vkCmdCopyImage);
   COPY_FN(vkCmdCopyImageToBuffer); COPY_FN(vkQueueSubmit); COPY_FN(vkQueueWaitIdle);
   COPY_FN(vkCmdClearColorImage); COPY_FN(vkCmdResolveImage);
   COPY_FN(vkMapMemory); COPY_FN(vkInvalidateMappedMemoryRanges); COPY_FN(vkUnmapMemory);
   auto get_memory=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get(instance,"vkGetPhysicalDeviceMemoryProperties"));
   VkPhysicalDeviceMemoryProperties memory{}; get_memory(physical,&memory);
   auto allocate=[&](VkMemoryRequirements req, VkMemoryPropertyFlags flags) {
      uint32_t index=0;
      while(index<memory.memoryTypeCount && (!(req.memoryTypeBits&(1u<<index)) ||
            (memory.memoryTypes[index].propertyFlags&flags)!=flags)) ++index;
      if(index==memory.memoryTypeCount) throw std::runtime_error("copy memory type");
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=index;
      VkDeviceMemory result{}; check(vkAllocateMemory(device,&ai,nullptr,&result),"copy allocate"); return result;
   };
   struct Case { VkFormat depth, color; VkImageAspectFlags aspect; uint32_t bytes; };
   const Case cases[]{
      {VK_FORMAT_D32_SFLOAT,VK_FORMAT_R32_SFLOAT,VK_IMAGE_ASPECT_DEPTH_BIT,4},
      {VK_FORMAT_D32_SFLOAT,VK_FORMAT_R32_UINT,VK_IMAGE_ASPECT_DEPTH_BIT,4},
      {VK_FORMAT_D32_SFLOAT,VK_FORMAT_R32_SINT,VK_IMAGE_ASPECT_DEPTH_BIT,4},
      {VK_FORMAT_D16_UNORM,VK_FORMAT_R16_UINT,VK_IMAGE_ASPECT_DEPTH_BIT,2},
      {VK_FORMAT_D16_UNORM,VK_FORMAT_R16_SFLOAT,VK_IMAGE_ASPECT_DEPTH_BIT,2},
      {VK_FORMAT_D16_UNORM,VK_FORMAT_R16_UNORM,VK_IMAGE_ASPECT_DEPTH_BIT,2},
      {VK_FORMAT_D16_UNORM,VK_FORMAT_R16_SNORM,VK_IMAGE_ASPECT_DEPTH_BIT,2},
      {VK_FORMAT_D16_UNORM,VK_FORMAT_R16_SINT,VK_IMAGE_ASPECT_DEPTH_BIT,2},
      {VK_FORMAT_D24_UNORM_S8_UINT,VK_FORMAT_R32_UINT,VK_IMAGE_ASPECT_DEPTH_BIT,4},
      {VK_FORMAT_D24_UNORM_S8_UINT,VK_FORMAT_R8_UINT,VK_IMAGE_ASPECT_STENCIL_BIT,1},
      {VK_FORMAT_D32_SFLOAT_S8_UINT,VK_FORMAT_R32_UINT,VK_IMAGE_ASPECT_DEPTH_BIT,4},
      {VK_FORMAT_D32_SFLOAT_S8_UINT,VK_FORMAT_R8_UINT,VK_IMAGE_ASPECT_STENCIL_BIT,1},
   };
   const uint32_t case_count=sizeof(cases)/sizeof(cases[0]);
   const uint32_t ms_cases[]{1,3,8,9,10,11};
   /* Set DZN_TEST_SPECIAL_DEPTH=1 to reproduce the remaining raw D32 depth
    * store limitation.  This diagnostic is expected to fail and must not be
    * hidden by extension advertising or a numeric-comparison tolerance. */
   const bool special_depth=GetEnvironmentVariableW(L"DZN_TEST_SPECIAL_DEPTH",nullptr,0)!=0;
   const bool full_depth=GetEnvironmentVariableW(L"DZN_TEST_FULL_DEPTH",nullptr,0)!=0;
   const bool integer_control=GetEnvironmentVariableW(L"DZN_TEST_INTEGER_CONTROL",nullptr,0)!=0;
   const bool float_control=GetEnvironmentVariableW(L"DZN_TEST_FLOAT_CONTROL",nullptr,0)!=0;
   const bool attachment_depth=GetEnvironmentVariableW(L"DZN_TEST_DEPTH_ATTACHMENT",nullptr,0)!=0;
   if(integer_control && float_control)
      throw std::runtime_error("Choose one D32 control format, not both");
   if(float_control && (!special_depth || !full_depth))
      throw std::runtime_error("FLOAT_CONTROL requires SPECIAL_DEPTH and FULL_DEPTH: partial color blits are numerical");
   if(integer_control || float_control || attachment_depth)
      std::cout << "D32 diagnostic middle format=" << (integer_control?"R32_UINT":float_control?"R32_SFLOAT":"D32_SFLOAT")
                << " depth attachment=" << attachment_depth << " full=" << full_depth << std::endl;
   for(uint32_t test=0; test<case_count+(special_depth?16:12); ++test) {
      const bool ms=test>=case_count;
      const bool partial=test>=case_count+6 && !(full_depth && test>=case_count+12);
      auto c=cases[test>=case_count+12 ? 1 : ms ? ms_cases[(test-case_count)%6] : test];
      if(integer_control && test>=case_count+12) {
         c.depth=VK_FORMAT_R32_UINT;
         c.aspect=VK_IMAGE_ASPECT_COLOR_BIT;
      }
      if(float_control && test>=case_count+12) {
         c.depth=VK_FORMAT_R32_SFLOAT;
         c.aspect=VK_IMAGE_ASPECT_COLOR_BIT;
      }
      VkImage images[4]{}; VkDeviceMemory allocations[4]{};
      for(uint32_t i=0;i<4;++i) {
         VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType=VK_IMAGE_TYPE_2D;
         ci.format=i==1?c.depth:c.color; ci.extent={16,16,1}; ci.mipLevels=ci.arrayLayers=1;
         ci.samples=ms && i<3?VK_SAMPLE_COUNT_4_BIT:VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL;
         ci.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
         if(attachment_depth && i==1 && c.aspect!=VK_IMAGE_ASPECT_COLOR_BIT)
            ci.usage|=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
         check(vkCreateImage(device,&ci,nullptr,&images[i]),"copy image");
         VkMemoryRequirements req{}; vkGetImageMemoryRequirements(device,images[i],&req);
         allocations[i]=allocate(req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
         check(vkBindImageMemory(device,images[i],allocations[i],0),"copy bind image");
      }
      VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=8192;
      bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      VkBuffer buffer{}; check(vkCreateBuffer(device,&bi,nullptr,&buffer),"copy buffer");
      VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device,buffer,&req);
      VkDeviceMemory buffer_memory=allocate(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
      check(vkBindBufferMemory(device,buffer,buffer_memory,0),"copy bind buffer");
      std::vector<uint8_t> pattern(256*c.bytes);
      for(uint32_t pixel=0;pixel<256;++pixel) {
         uint32_t value=c.bytes==4 ? (c.depth==VK_FORMAT_D24_UNORM_S8_UINT ?
            (pixel*65537u)&0xffffffu : 0x3e800000u+(pixel<<13)) : pixel*257u;
         if(ms) value=c.bytes==4?(c.depth==VK_FORMAT_D24_UNORM_S8_UINT?0x800000u:0x3f000000u):c.bytes==2?0x8000u:0x5au;
         if(test>=case_count+12) {
            const uint32_t special[]{0x7fc0abcdu,0xbf000000u,0x00000100u,0x7f800000u};
            value=special[test-case_count-12];
         }
         if(partial && (pixel%16<4 || pixel%16>=12 || pixel/16<4 || pixel/16>=12)) value=0;
         std::memcpy(pattern.data()+pixel*c.bytes,&value,c.bytes);
      }
      VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex=family;
      VkCommandPool pool{}; check(vkCreateCommandPool(device,&pci,nullptr,&pool),"copy pool");
      VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      cai.commandPool=pool; cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount=1;
      VkCommandBuffer cb{}; check(vkAllocateCommandBuffers(device,&cai,&cb),"copy commands");
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(cb,&begin),"copy begin");
      vkCmdUpdateBuffer(cb,buffer,0,pattern.size(),pattern.data());
      auto barrier=[&](uint32_t i,VkImageLayout old_layout,VkImageLayout new_layout) {
         VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
         b.srcAccessMask=old_layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
         b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
         b.oldLayout=old_layout; b.newLayout=new_layout;
         b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=images[i];
         VkImageAspectFlags aspects=i==1?c.aspect:VK_IMAGE_ASPECT_COLOR_BIT;
         if(i==1 && (c.depth==VK_FORMAT_D24_UNORM_S8_UINT || c.depth==VK_FORMAT_D32_SFLOAT_S8_UINT))
            aspects=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
         b.subresourceRange={aspects,0,1,0,1};
         vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
      };
      VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; bb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
      bb.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT; bb.srcQueueFamilyIndex=bb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      bb.buffer=buffer; bb.size=VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&bb,0,nullptr);
      for(uint32_t i=0;i<4;++i) barrier(i,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkBufferImageCopy upload{}; upload.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; upload.imageExtent={16,16,1};
      if(ms) {
         VkClearColorValue clear{}; std::memcpy(&clear.uint32[0],pattern.data()+68*c.bytes,c.bytes);
         VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
         vkCmdClearColorImage(cb,images[0],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
         clear.uint32[0]=0; vkCmdClearColorImage(cb,images[2],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
      } else vkCmdCopyBufferToImage(cb,buffer,images[0],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&upload);
      barrier(0,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      if(test>=case_count+12) {
         VkImageResolve source_resolve{}; source_resolve.srcSubresource=source_resolve.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
         source_resolve.extent={16,16,1};
         vkCmdResolveImage(cb,images[0],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,images[3],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&source_resolve);
         barrier(3,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
         VkBufferImageCopy source_copy=upload; source_copy.bufferOffset=2048;
         vkCmdCopyImageToBuffer(cb,images[3],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&source_copy);
         barrier(3,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      }
      VkImageCopy copy{}; copy.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
      copy.dstSubresource={c.aspect,0,0,1}; copy.extent={16,16,1};
      if(partial) {copy.srcOffset=copy.dstOffset={4,4,0};copy.extent={8,8,1};}
      vkCmdCopyImage(cb,images[0],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,images[1],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
      barrier(1,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      copy.srcSubresource.aspectMask=c.aspect; copy.dstSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
      vkCmdCopyImage(cb,images[1],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,images[2],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
      barrier(2,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      if(ms) {
         VkImageResolve resolve{}; resolve.srcSubresource=resolve.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; resolve.extent={16,16,1};
         vkCmdResolveImage(cb,images[2],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,images[3],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&resolve);
         barrier(3,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      }
      upload.bufferOffset=4096; vkCmdCopyImageToBuffer(cb,images[ms?3:2],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&upload);
      bb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; bb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&bb,0,nullptr);
      check(vkEndCommandBuffer(cb),"copy end"); VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cb;
      check(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE),"copy submit"); check(vkQueueWaitIdle(queue),"copy wait");
      void* mapped{}; check(vkMapMemory(device,buffer_memory,0,VK_WHOLE_SIZE,0,&mapped),"copy map");
      VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory=buffer_memory; range.size=VK_WHOLE_SIZE;
      check(vkInvalidateMappedMemoryRanges(device,1,&range),"copy invalidate");
      for(uint32_t pixel=0;pixel<256;++pixel) {
         uint32_t actual=0,expected=0; std::memcpy(&actual,static_cast<const uint8_t*>(mapped)+4096+pixel*c.bytes,c.bytes);
         std::memcpy(&expected,pattern.data()+pixel*c.bytes,c.bytes);
         if(c.aspect==VK_IMAGE_ASPECT_DEPTH_BIT && c.depth==VK_FORMAT_D24_UNORM_S8_UINT) {actual&=0xffffffu;expected&=0xffffffu;}
         if(actual!=expected) {
            uint32_t source=0; if(test>=case_count+12) std::memcpy(&source,static_cast<const uint8_t*>(mapped)+2048+pixel*c.bytes,c.bytes);
            throw std::runtime_error("copy bit mismatch case="+std::to_string(test)+" pixel="+std::to_string(pixel)+" actual="+std::to_string(actual)+" expected="+std::to_string(expected)+" source="+std::to_string(source));
         }
      }
      vkUnmapMemory(device,buffer_memory); vkDestroyCommandPool(device,pool,nullptr);
      vkDestroyBuffer(device,buffer,nullptr); vkFreeMemory(device,buffer_memory,nullptr);
      for(uint32_t i=0;i<4;++i) {vkDestroyImage(device,images[i],nullptr);vkFreeMemory(device,allocations[i],nullptr);}
      std::cout<<"PASS: internal matching-aspect bit copy case "<<test<<'\n';
   }
#undef COPY_FN
}
