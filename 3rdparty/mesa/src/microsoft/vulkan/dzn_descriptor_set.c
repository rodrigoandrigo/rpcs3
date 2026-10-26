/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "dzn_private.h"

#include "vk_alloc.h"
#include "vk_descriptors.h"
#include "vk_util.h"

#include "dxil_spirv_nir.h"

#include "util/mesa-blake3.h"
#include "util/format/u_format.h"
#include "vk_format.h"

static uint32_t
translate_desc_stages(VkShaderStageFlags in)
{
   if (in == VK_SHADER_STAGE_ALL)
      in = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;

   uint32_t out = 0;

   u_foreach_bit(s, in)
      out |= BITFIELD_BIT(vk_to_mesa_shader_stage(BITFIELD_BIT(s)));

   return out;
}

static D3D12_SHADER_VISIBILITY
translate_desc_visibility(VkShaderStageFlags in)
{
   switch (in) {
   case VK_SHADER_STAGE_VERTEX_BIT: return D3D12_SHADER_VISIBILITY_VERTEX;
   case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return D3D12_SHADER_VISIBILITY_HULL;
   case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return D3D12_SHADER_VISIBILITY_DOMAIN;
   case VK_SHADER_STAGE_GEOMETRY_BIT: return D3D12_SHADER_VISIBILITY_GEOMETRY;
   case VK_SHADER_STAGE_FRAGMENT_BIT: return D3D12_SHADER_VISIBILITY_PIXEL;
   default: return D3D12_SHADER_VISIBILITY_ALL;
   }
}

static D3D12_DESCRIPTOR_RANGE_TYPE
desc_type_to_range_type(VkDescriptorType in, bool writeable)
{
   switch (in) {
   case VK_DESCRIPTOR_TYPE_SAMPLER:
      return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;

   case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
   case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
   case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
      return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;

   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
   case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
      return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;

   case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
      return writeable ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
   default:
      UNREACHABLE("Unsupported desc type");
   }
}

static bool
is_buffer_desc_type_without_view(VkDescriptorType desc_type)
{
   return (desc_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
           desc_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
}

static bool
dzn_descriptor_type_depends_on_shader_usage(VkDescriptorType type, bool bindless)
{
   if (bindless)
      return false;
   return type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER ||
          type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
          type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
          type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

static inline bool
dzn_desc_type_has_sampler(VkDescriptorType type)
{
   return type == VK_DESCRIPTOR_TYPE_SAMPLER ||
          type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

static uint32_t
dzn_binding_desc_count(const VkDescriptorSetLayoutBinding *binding)
{
   return binding->descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK ?
          1 : binding->descriptorCount;
}

static uint32_t
num_descs_for_type(VkDescriptorType type, bool static_sampler, bool bindless)
{
   if (bindless)
      return 1;

   unsigned num_descs = 1;

   /* Some type map to an SRV or UAV depending on how the shaders is using the
    * resource (NONWRITEABLE flag set or not), in that case we need to reserve
    * slots for both the UAV and SRV descs.
    */
   if (dzn_descriptor_type_depends_on_shader_usage(type, false))
      num_descs++;

   /* There's no combined SRV+SAMPLER type in d3d12, we need an descriptor
    * for the sampler.
    */
   if (type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
      num_descs++;

   /* Don't count immutable samplers, they have their own descriptor. */
   if (static_sampler && dzn_desc_type_has_sampler(type))
      num_descs--;

   return num_descs;
}

static void
dzn_descriptor_set_layout_hash(struct dzn_descriptor_set_layout *layout,
                               const VkDescriptorSetLayoutCreateInfo *create_info,
                               const VkDescriptorSetLayoutBinding *sorted_bindings,
                               bool image_lod)
{
   const VkDescriptorSetLayoutBindingFlagsCreateInfo *binding_flags =
      vk_find_struct_const(create_info->pNext,
                           DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
   struct mesa_blake3 ctx;
   _mesa_blake3_init(&ctx);

#define DZN_HASH_VALUE(value) \
   _mesa_blake3_update(&ctx, &(value), sizeof(value))

   VkDescriptorSetLayoutCreateFlags layout_flags = create_info->flags;
   uint32_t binding_count = create_info->bindingCount;
   DZN_HASH_VALUE(image_lod);
   DZN_HASH_VALUE(layout_flags);
   DZN_HASH_VALUE(binding_count);

   for (uint32_t i = 0; i < create_info->bindingCount; i++) {
      const VkDescriptorSetLayoutBinding *binding = &sorted_bindings[i];
      uint32_t binding_number = binding->binding;
      VkDescriptorType descriptor_type = binding->descriptorType;
      uint32_t descriptor_count = binding->descriptorCount;
      VkShaderStageFlags stage_flags = binding->stageFlags;
      VkDescriptorBindingFlags flags = 0;
      for (uint32_t j = 0; j < create_info->bindingCount; j++) {
         if (create_info->pBindings[j].binding == binding_number) {
            if (binding_flags && j < binding_flags->bindingCount)
               flags = binding_flags->pBindingFlags[j];
            break;
         }
      }
      bool has_immutable_samplers = dzn_desc_type_has_sampler(descriptor_type) &&
                                    binding->pImmutableSamplers != NULL;
      DZN_HASH_VALUE(binding_number);
      DZN_HASH_VALUE(descriptor_type);
      DZN_HASH_VALUE(descriptor_count);
      DZN_HASH_VALUE(stage_flags);
      DZN_HASH_VALUE(flags);
      DZN_HASH_VALUE(has_immutable_samplers);

      if (has_immutable_samplers) {
         for (uint32_t s = 0; s < descriptor_count; s++) {
            VK_FROM_HANDLE(dzn_sampler, sampler,
                           binding->pImmutableSamplers[s]);
            bool has_sampler = sampler != NULL;
            DZN_HASH_VALUE(has_sampler);
            if (has_sampler) {
               _mesa_blake3_update(&ctx, &sampler->desc,
                                   sizeof(sampler->desc));
               int static_border_color = sampler->static_border_color;
               DZN_HASH_VALUE(static_border_color);
            }
         }
      }
   }

   _mesa_blake3_final(&ctx, layout->vk.blake3);
#undef DZN_HASH_VALUE
}

static VkResult
dzn_descriptor_set_layout_create(struct dzn_device *device,
                                 const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                                 const VkAllocationCallbacks *pAllocator,
                                 VkDescriptorSetLayout *out)
{
   const VkDescriptorSetLayoutBinding *bindings = pCreateInfo->pBindings;
   uint32_t binding_count = 0, static_sampler_count = 0, total_ranges = 0;
   uint32_t dynamic_ranges_offset = 0, immutable_sampler_count = 0;
   uint32_t dynamic_buffer_count = 0;
   uint32_t range_count[MAX_SHADER_VISIBILITIES][NUM_POOL_TYPES] = { 0 };

   const VkDescriptorSetLayoutBindingFlagsCreateInfo *binding_flags =
      vk_find_struct_const(pCreateInfo->pNext, DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
   bool has_variable_size = false;

   for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
      binding_count = MAX2(binding_count, bindings[i].binding + 1);

      if (!bindings[i].descriptorCount)
         continue;

      D3D12_SHADER_VISIBILITY visibility = device->bindless ?
         D3D12_SHADER_VISIBILITY_ALL :
         translate_desc_visibility(bindings[i].stageFlags);
      VkDescriptorType desc_type = bindings[i].descriptorType;
      bool has_sampler = dzn_desc_type_has_sampler(desc_type);

      /* From the Vulkan 1.1.97 spec for VkDescriptorSetLayoutBinding:
       *
       *    "If descriptorType specifies a VK_DESCRIPTOR_TYPE_SAMPLER or
       *    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER type descriptor, then
       *    pImmutableSamplers can be used to initialize a set of immutable
       *    samplers. [...]  If descriptorType is not one of these descriptor
       *    types, then pImmutableSamplers is ignored.
       *
       * We need to be careful here and only parse pImmutableSamplers if we
       * have one of the right descriptor types.
       */
      bool immutable_samplers =
         has_sampler &&
         bindings[i].pImmutableSamplers != NULL;
      bool static_sampler = false;

      if (device->support_static_samplers &&
          immutable_samplers && bindings[i].descriptorCount == 1) {
         VK_FROM_HANDLE(dzn_sampler, sampler, bindings[i].pImmutableSamplers[0]);

         if (sampler->static_border_color != -1)
            static_sampler = true;
      }

      if (static_sampler) {
         static_sampler_count += bindings[i].descriptorCount;
      } else if (has_sampler) {
         unsigned type = device->bindless ? D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV : D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
         range_count[visibility][type]++;
         total_ranges++;

         if (immutable_samplers)
            immutable_sampler_count += bindings[i].descriptorCount;
      }

      if (desc_type != VK_DESCRIPTOR_TYPE_SAMPLER) {
         range_count[visibility][D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV]++;
         total_ranges++;

         if (dzn_descriptor_type_depends_on_shader_usage(desc_type, device->bindless)) {
            range_count[visibility][D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV]++;
            total_ranges++;
         }

         uint32_t factor =
            dzn_descriptor_type_depends_on_shader_usage(desc_type, device->bindless) ? 2 : 1;
         if (vk_descriptor_type_is_dynamic(desc_type))
            dynamic_buffer_count += dzn_binding_desc_count(&bindings[i]) * factor;
         else
            dynamic_ranges_offset += dzn_binding_desc_count(&bindings[i]) * factor;
      }

      if (binding_flags && binding_flags->bindingCount &&
          (binding_flags->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT))
         has_variable_size = true;
   }



   /* We need to allocate decriptor set layouts off the device allocator
    * with DEVICE scope because they are reference counted and may not be
    * destroyed when vkDestroyDescriptorSetLayout is called.
    */
   VK_MULTIALLOC(ma);
   VK_MULTIALLOC_DECL(&ma, struct dzn_descriptor_set_layout, set_layout, 1);
   VK_MULTIALLOC_DECL(&ma, D3D12_DESCRIPTOR_RANGE1,
                      ranges, total_ranges);
   VK_MULTIALLOC_DECL(&ma, D3D12_STATIC_SAMPLER_DESC1, static_samplers,
                      static_sampler_count);
   VK_MULTIALLOC_DECL(&ma, const struct dzn_sampler *, immutable_samplers,
                      immutable_sampler_count);
   VK_MULTIALLOC_DECL(&ma, struct dzn_descriptor_set_layout_binding, binfos,
                      binding_count);

   if (!vk_descriptor_set_layout_multizalloc(&device->vk, &ma, pCreateInfo))
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   set_layout->static_samplers = static_samplers;
   set_layout->static_sampler_count = static_sampler_count;
   set_layout->immutable_samplers = immutable_samplers;
   set_layout->immutable_sampler_count = immutable_sampler_count;
   set_layout->bindings = binfos;
   set_layout->binding_count = binding_count;
   set_layout->dynamic_buffers.range_offset = dynamic_ranges_offset;
   set_layout->buffer_count = dynamic_buffer_count;

   for (uint32_t i = 0; i < MAX_SHADER_VISIBILITIES; i++) {
      dzn_foreach_pool_type (type) {
         if (range_count[i][type]) {
            set_layout->ranges[i][type] = ranges;
            set_layout->range_count[i][type] = range_count[i][type];
            ranges += range_count[i][type];
         }
      }
   }

   VkDescriptorSetLayoutBinding *ordered_bindings;
   VkResult ret =
      vk_create_sorted_bindings(pCreateInfo->pBindings,
                                pCreateInfo->bindingCount,
                                &ordered_bindings, NULL, NULL);
   if (ret != VK_SUCCESS) {
      vk_descriptor_set_layout_destroy(&device->vk, &set_layout->vk);
      return ret;
   }

   dzn_descriptor_set_layout_hash(set_layout, pCreateInfo, ordered_bindings,
                                  device->vk.enabled_extensions.AMD_shader_image_load_store_lod);

   assert(binding_count ==
          (pCreateInfo->bindingCount ?
           (ordered_bindings[pCreateInfo->bindingCount - 1].binding + 1) : 0));

   uint32_t range_idx[MAX_SHADER_VISIBILITIES][NUM_POOL_TYPES] = { 0 };
   uint32_t static_sampler_idx = 0, immutable_sampler_idx = 0;
   uint32_t dynamic_buffer_idx = 0;
   uint32_t base_register = 0;

   for (uint32_t i = 0; i < binding_count; i++) {
      binfos[i].immutable_sampler_idx = ~0;
      binfos[i].buffer_idx = ~0;
      dzn_foreach_pool_type (type)
         binfos[i].range_idx[type] = ~0;
   }

   for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
      VkDescriptorType desc_type = ordered_bindings[i].descriptorType;
      uint32_t binding = ordered_bindings[i].binding;
      uint32_t desc_count = dzn_binding_desc_count(&ordered_bindings[i]);
      bool has_sampler = dzn_desc_type_has_sampler(desc_type);
      bool has_immutable_samplers =
         has_sampler &&
         ordered_bindings[i].pImmutableSamplers != NULL;
      bool has_static_sampler = device->support_static_samplers &&
         has_immutable_samplers && desc_count == 1;
      bool is_dynamic = vk_descriptor_type_is_dynamic(desc_type);

      D3D12_SHADER_VISIBILITY visibility = device->bindless ?
         D3D12_SHADER_VISIBILITY_ALL :
         translate_desc_visibility(ordered_bindings[i].stageFlags);
      binfos[binding].type = desc_type;
      binfos[binding].descriptor_count = desc_count;
      binfos[binding].flags = 0;
      if (binding_flags && binding_flags->bindingCount) {
         for (uint32_t j = 0; j < pCreateInfo->bindingCount; j++) {
            if (bindings[j].binding == binding) {
               binfos[binding].flags = binding_flags->pBindingFlags[j];
               break;
            }
         }
      }
      binfos[binding].descriptor_stride =
         desc_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
         device->vk.enabled_extensions.AMD_shader_image_load_store_lod ?
         DZN_IMAGE_MIP_DESCRIPTORS : 1;
      binfos[binding].stages =
         translate_desc_stages(ordered_bindings[i].stageFlags);
      set_layout->stages |= binfos[binding].stages;
      binfos[binding].visibility = visibility;
      /* Only the last binding can have variable size */
      if (binding == binding_count - 1)
         binfos[binding].variable_size = has_variable_size;
      if (is_dynamic && device->bindless) {
         /* Assign these into a separate 0->N space */
         binfos[binding].base_shader_register = dynamic_buffer_idx;
      } else {
         binfos[binding].base_shader_register = base_register;
         assert(base_register + desc_count >= base_register);
         base_register += desc_count * binfos[binding].descriptor_stride;
      }

      if (has_static_sampler) {
         VK_FROM_HANDLE(dzn_sampler, sampler, ordered_bindings[i].pImmutableSamplers[0]);

         /* Not all border colors are supported. */
         if (sampler->static_border_color != -1) {
            D3D12_STATIC_SAMPLER_DESC1 *desc = (D3D12_STATIC_SAMPLER_DESC1 *)
               &static_samplers[static_sampler_idx];

            desc->Filter = sampler->desc.Filter;
            desc->AddressU = sampler->desc.AddressU;
            desc->AddressV = sampler->desc.AddressV;
            desc->AddressW = sampler->desc.AddressW;
            desc->MipLODBias = sampler->desc.MipLODBias;
            desc->MaxAnisotropy = sampler->desc.MaxAnisotropy;
            desc->ComparisonFunc = sampler->desc.ComparisonFunc;
            desc->BorderColor = sampler->static_border_color;
            desc->MinLOD = sampler->desc.MinLOD;
            desc->MaxLOD = sampler->desc.MaxLOD;
            desc->ShaderRegister = binfos[binding].base_shader_register;
            desc->ShaderVisibility = translate_desc_visibility(ordered_bindings[i].stageFlags);
            desc->Flags = sampler->desc.Flags;
            if (device->bindless && desc_type == VK_DESCRIPTOR_TYPE_SAMPLER) {
               /* Avoid using space in the descriptor set buffer for pure static samplers. The meaning
                * of base register index is different for them - it's just an ID to map to the root
                * signature, as opposed to all other bindings where it's an offset into the buffer. */
               binfos[binding].base_shader_register = desc->ShaderRegister = static_sampler_idx;
               base_register--;
            }
            static_sampler_idx++;
         } else {
            has_static_sampler = false;
         }
      }

      if (desc_type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
         binfos[binding].inline_offset = set_layout->inline_data_size;
         binfos[binding].inline_size = ordered_bindings[i].descriptorCount;
         set_layout->inline_data_size +=
            ALIGN_POT(ordered_bindings[i].descriptorCount,
                      D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
      }

      if (has_static_sampler) {
         binfos[binding].immutable_sampler_idx = STATIC_SAMPLER_TAG;
      } else if (has_immutable_samplers) {
         binfos[binding].immutable_sampler_idx = immutable_sampler_idx;
         for (uint32_t s = 0; s < desc_count; s++) {
            VK_FROM_HANDLE(dzn_sampler, sampler, ordered_bindings[i].pImmutableSamplers[s]);

            immutable_samplers[immutable_sampler_idx++] = sampler;
         }
      }

      if (is_dynamic) {
         binfos[binding].buffer_idx = dynamic_buffer_idx;
         for (uint32_t d = 0; d < desc_count; d++)
            set_layout->dynamic_buffers.bindings[dynamic_buffer_idx + d] = binding;
         dynamic_buffer_idx += desc_count;
         assert(dynamic_buffer_idx <= MAX_DYNAMIC_BUFFERS);
      } else if (is_buffer_desc_type_without_view(desc_type)) {
         binfos[binding].buffer_idx = set_layout->buffer_count;
         set_layout->buffer_count += desc_count;
      }

      if (!ordered_bindings[i].descriptorCount)
         continue;

      unsigned num_descs =
         num_descs_for_type(desc_type, has_static_sampler, device->bindless);
      if (!num_descs) continue;

      assert(visibility < ARRAY_SIZE(set_layout->ranges));

      bool has_range[NUM_POOL_TYPES] = { 0 };
      has_range[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER] =
         has_sampler && !has_static_sampler && !device->bindless;
      has_range[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV] =
         desc_type != VK_DESCRIPTOR_TYPE_SAMPLER || (device->bindless && !has_static_sampler);

      dzn_foreach_pool_type (type) {
         if (!has_range[type]) continue;

         uint32_t idx = range_idx[visibility][type]++;
         assert(idx < range_count[visibility][type]);

         binfos[binding].range_idx[type] = idx;
         D3D12_DESCRIPTOR_RANGE1 *range = (D3D12_DESCRIPTOR_RANGE1 *)
            &set_layout->ranges[visibility][type][idx];
         VkDescriptorType range_type = desc_type;
         if (desc_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            range_type = type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ?
                         VK_DESCRIPTOR_TYPE_SAMPLER :
                         VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
         }
         range->RangeType = desc_type_to_range_type(range_type, true);
         range->NumDescriptors = desc_count * binfos[binding].descriptor_stride;
         range->BaseShaderRegister = binfos[binding].base_shader_register;
         range->Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
            (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 0 : D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);
         if (is_dynamic) {
            range->OffsetInDescriptorsFromTableStart =
               set_layout->dynamic_buffers.range_offset +
               set_layout->dynamic_buffers.desc_count;
            set_layout->dynamic_buffers.count += range->NumDescriptors;
            set_layout->dynamic_buffers.desc_count += range->NumDescriptors;
         } else {
            range->OffsetInDescriptorsFromTableStart = set_layout->range_desc_count[type];
            if (!binfos[binding].variable_size || !device->bindless)
               set_layout->range_desc_count[type] += range->NumDescriptors;
         }

         if (!dzn_descriptor_type_depends_on_shader_usage(desc_type, device->bindless))
            continue;

         assert(idx + 1 < range_count[visibility][type]);
         range_idx[visibility][type]++;
         range[1] = range[0];
         range++;
         range->RangeType = desc_type_to_range_type(range_type, false);
         if (is_dynamic) {
            range->OffsetInDescriptorsFromTableStart =
               set_layout->dynamic_buffers.range_offset +
               set_layout->dynamic_buffers.desc_count;
            set_layout->dynamic_buffers.desc_count += range->NumDescriptors;
         } else {
            range->OffsetInDescriptorsFromTableStart = set_layout->range_desc_count[type];
            set_layout->range_desc_count[type] += range->NumDescriptors;
         }
      }
   }

   free(ordered_bindings);

   *out = dzn_descriptor_set_layout_to_handle(set_layout);
   return VK_SUCCESS;
}

static uint32_t
dzn_descriptor_set_layout_get_heap_offset(const struct dzn_descriptor_set_layout *layout,
                                          uint32_t b,
                                          D3D12_DESCRIPTOR_HEAP_TYPE type,
                                          bool alt,
                                          bool bindless)
{
   assert(b < layout->binding_count);
   D3D12_SHADER_VISIBILITY visibility = layout->bindings[b].visibility;
   assert(visibility < ARRAY_SIZE(layout->ranges));
   assert((unsigned)type < NUM_POOL_TYPES);

   if (bindless)
      type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

   uint32_t range_idx = layout->bindings[b].range_idx[(unsigned)type];

   if (range_idx == ~0)
      return ~0;

   if (alt &&
       !dzn_descriptor_type_depends_on_shader_usage(layout->bindings[b].type, bindless))
      return ~0;

   if (alt)
      range_idx++;

   assert(range_idx < layout->range_count[visibility][type]);
   return layout->ranges[visibility][type][range_idx].OffsetInDescriptorsFromTableStart;
}

static uint32_t
dzn_descriptor_set_layout_get_desc_count(const struct dzn_descriptor_set_layout *layout,
                                         uint32_t b)
{
   ASSERTED D3D12_SHADER_VISIBILITY visibility = layout->bindings[b].visibility;
   assert(visibility < ARRAY_SIZE(layout->ranges));

   dzn_foreach_pool_type (type) {
      uint32_t range_idx = layout->bindings[b].range_idx[type];
      assert(range_idx == ~0 || range_idx < layout->range_count[visibility][type]);

      if (range_idx != ~0)
         return layout->bindings[b].descriptor_count;
   }

   return 0;
}

VKAPI_ATTR VkResult VKAPI_CALL
dzn_CreateDescriptorSetLayout(VkDevice device,
                              const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                              const VkAllocationCallbacks *pAllocator,
                              VkDescriptorSetLayout *pSetLayout)
{
   return dzn_descriptor_set_layout_create(dzn_device_from_handle(device),
                                           pCreateInfo, pAllocator, pSetLayout);
}

VKAPI_ATTR void VKAPI_CALL
dzn_GetDescriptorSetLayoutSupport(VkDevice _device,
                                  const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                                  VkDescriptorSetLayoutSupport *pSupport)
{
   VK_FROM_HANDLE(dzn_device, device, _device);
   const VkDescriptorSetLayoutBinding *bindings = pCreateInfo->pBindings;
   uint64_t sampler_count = 0, other_desc_count = 0;
   uint64_t push_desc_count = 0;
   bool invalid_push_descriptor_type = false;
   bool is_push_descriptor_layout =
      pCreateInfo->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
   bool inline_size_supported = true;

   const VkDescriptorSetLayoutBindingFlagsCreateInfo *binding_flags =
      vk_find_struct_const(pCreateInfo->pNext, DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
   VkDescriptorSetVariableDescriptorCountLayoutSupport *variable_count =
      vk_find_struct(pSupport->pNext, DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT);
   if (variable_count)
      variable_count->maxVariableDescriptorCount = 0;

   for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
      VkDescriptorType desc_type = bindings[i].descriptorType;
      if (is_push_descriptor_layout) {
         push_desc_count += bindings[i].descriptorCount;
         invalid_push_descriptor_type |=
            vk_descriptor_type_is_dynamic(desc_type) ||
            desc_type > VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
      }
      bool has_sampler = dzn_desc_type_has_sampler(desc_type);
      bool is_sampler = desc_type == VK_DESCRIPTOR_TYPE_SAMPLER;

      unsigned stride = desc_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
         device->vk.enabled_extensions.AMD_shader_image_load_store_lod ? DZN_IMAGE_MIP_DESCRIPTORS : 1;
      if (desc_type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK &&
          bindings[i].descriptorCount > DZN_INLINE_UNIFORM_BLOCK_MAX_SIZE)
         inline_size_supported = false;

      if (has_sampler)
         sampler_count += bindings[i].descriptorCount;
      if (!is_sampler)
         other_desc_count += (uint64_t)dzn_binding_desc_count(&bindings[i]) * stride;
      if (dzn_descriptor_type_depends_on_shader_usage(desc_type, device->bindless))
         other_desc_count += (uint64_t)bindings[i].descriptorCount * stride;
   }

   if (variable_count && binding_flags) {
      for (uint32_t i = 0; i < binding_flags->bindingCount; i++) {
         if (!(binding_flags->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT))
            continue;
         VkDescriptorType type = bindings[i].descriptorType;
         unsigned stride = type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
            device->vk.enabled_extensions.AMD_shader_image_load_store_lod ? DZN_IMAGE_MIP_DESCRIPTORS : 1;
         if (dzn_descriptor_type_depends_on_shader_usage(type, device->bindless))
            stride *= 2;
         uint64_t other = other_desc_count;
         if (type != VK_DESCRIPTOR_TYPE_SAMPLER)
            other -= (uint64_t)dzn_binding_desc_count(&bindings[i]) * stride;
         uint64_t upper_bound = other < MAX_DESCS_PER_CBV_SRV_UAV_HEAP ?
            (MAX_DESCS_PER_CBV_SRV_UAV_HEAP - other) / stride : 0;
         if (dzn_desc_type_has_sampler(type)) {
            uint64_t samplers = sampler_count - bindings[i].descriptorCount;
            uint64_t sampler_upper_bound = samplers < MAX_DESCS_PER_SAMPLER_HEAP ?
               MAX_DESCS_PER_SAMPLER_HEAP - samplers : 0;
            upper_bound = type == VK_DESCRIPTOR_TYPE_SAMPLER ? sampler_upper_bound :
               MIN2(sampler_upper_bound, upper_bound);
         }
         variable_count->maxVariableDescriptorCount =
            type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK ? DZN_INLINE_UNIFORM_BLOCK_MAX_SIZE :
            device->bindless && stride == 1 ? INT32_MAX : upper_bound;
      }
   }

   pSupport->supported = inline_size_supported && (device->bindless ||
      (sampler_count <= MAX_DESCS_PER_SAMPLER_HEAP &&
       other_desc_count <= MAX_DESCS_PER_CBV_SRV_UAV_HEAP)) &&
      (!is_push_descriptor_layout ||
       (push_desc_count <= DZN_MAX_PUSH_DESCRIPTORS &&
        !invalid_push_descriptor_type));
}

static void
dzn_pipeline_layout_destroy(struct vk_device *vk_device,
                            struct vk_pipeline_layout *vk_layout)
{
   struct dzn_pipeline_layout *layout =
      container_of(vk_layout, struct dzn_pipeline_layout, vk);

   if (layout->root.sig)
      ID3D12RootSignature_Release(layout->root.sig);

   vk_pipeline_layout_destroy(vk_device, &layout->vk);
}

#define MAX_INTERNAL_ROOT_PARAMS 3

// One root parameter for samplers and the other one for views, multiplied by
// the number of visibility combinations, plus the internal root parameters.
#define MAX_ROOT_PARAMS ((MAX_SHADER_VISIBILITIES * 2) + MAX_INTERNAL_ROOT_PARAMS)

// Maximum number of DWORDS (32-bit words) that can be used for a root signature
#define MAX_ROOT_DWORDS 64

static void
dzn_pipeline_layout_hash_stages(struct dzn_pipeline_layout *layout,
                                const VkPipelineLayoutCreateInfo *info)
{
   uint32_t stages = 0;
   for (uint32_t stage = 0; stage < ARRAY_SIZE(layout->stages); stage++) {
      for (uint32_t set = 0; set < info->setLayoutCount; set++) {
         VK_FROM_HANDLE(dzn_descriptor_set_layout, set_layout, info->pSetLayouts[set]);

         stages |= set_layout->stages;
      }
   }

   for (uint32_t stage = 0; stage < ARRAY_SIZE(layout->stages); stage++) {
      if (!(stages & BITFIELD_BIT(stage)))
         continue;

      blake3_hasher ctx;

      _mesa_blake3_init(&ctx);
      for (uint32_t set = 0; set < info->setLayoutCount; set++) {
         VK_FROM_HANDLE(dzn_descriptor_set_layout, set_layout, info->pSetLayouts[set]);
         if (!(BITFIELD_BIT(stage) & set_layout->stages))
            continue;

         for (uint32_t b = 0; b < set_layout->binding_count; b++) {
            if (!(BITFIELD_BIT(stage) & set_layout->bindings[b].stages))
               continue;

            _mesa_blake3_update(&ctx, &b, sizeof(b));
            _mesa_blake3_update(&ctx, &set, sizeof(set));
            _mesa_blake3_update(&ctx, &layout->binding_translation[set].image_offsets[b],
                                sizeof(uint32_t));
            _mesa_blake3_update(&ctx, &set_layout->bindings[b].base_shader_register,
                              sizeof(set_layout->bindings[b].base_shader_register));
         }
      }
      _mesa_blake3_final(&ctx, layout->stages[stage].hash);
   }
}

static VkResult
dzn_pipeline_layout_create(struct dzn_device *device,
                           const VkPipelineLayoutCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkPipelineLayout *out)
{
   struct dzn_physical_device *pdev = container_of(device->vk.physical, struct dzn_physical_device, vk);
   uint32_t binding_count = 0;

   for (uint32_t s = 0; s < pCreateInfo->setLayoutCount; s++) {
      VK_FROM_HANDLE(dzn_descriptor_set_layout, set_layout, pCreateInfo->pSetLayouts[s]);

      if (!set_layout)
         continue;

      binding_count += set_layout->binding_count;
   }

   VK_MULTIALLOC(ma);
   VK_MULTIALLOC_DECL(&ma, struct dzn_pipeline_layout, layout, 1);
   VK_MULTIALLOC_DECL(&ma, uint32_t, binding_translation, binding_count);
   VK_MULTIALLOC_DECL(&ma, uint32_t, image_offsets, binding_count);
   VK_MULTIALLOC_DECL(&ma, uint8_t, binding_class, binding_count);

   if (!vk_pipeline_layout_multizalloc(&device->vk, &ma, pCreateInfo))
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   layout->vk.destroy = dzn_pipeline_layout_destroy;

   for (uint32_t s = 0; s < pCreateInfo->setLayoutCount; s++) {
      VK_FROM_HANDLE(dzn_descriptor_set_layout, set_layout, pCreateInfo->pSetLayouts[s]);

      if (!set_layout || !set_layout->binding_count)
         continue;

      layout->binding_translation[s].base_reg = binding_translation;
      layout->binding_translation[s].image_offsets = image_offsets;
      image_offsets += set_layout->binding_count;
      layout->binding_translation[s].binding_class = binding_class;
      binding_translation += set_layout->binding_count;
      binding_class += set_layout->binding_count;
   }

   uint32_t range_count = 0, static_sampler_count = 0;
   uint32_t dynamic_buffer_base = 0;

   layout->root.param_count = 0;
   dzn_foreach_pool_type (type)
      layout->desc_count[type] = 0;

   layout->set_count = pCreateInfo->setLayoutCount;
   for (uint32_t j = 0; j < layout->set_count; j++) {
      VK_FROM_HANDLE(dzn_descriptor_set_layout, set_layout, pCreateInfo->pSetLayouts[j]);
      uint32_t *binding_trans = layout->binding_translation[j].base_reg;

      layout->sets[j].dynamic_buffer_count = set_layout->dynamic_buffers.count;
      memcpy(layout->sets[j].range_desc_count, set_layout->range_desc_count,
             sizeof(layout->sets[j].range_desc_count));
      layout->binding_translation[j].binding_count = set_layout->binding_count;

      for (uint32_t b = 0; b < set_layout->binding_count; b++) {
         binding_trans[b] = set_layout->bindings[b].base_shader_register;
         if (vk_descriptor_type_is_dynamic(set_layout->bindings[b].type)) {
            layout->binding_translation[j].binding_class[b] = DZN_PIPELINE_BINDING_DYNAMIC_BUFFER;
            if (device->bindless)
               binding_trans[b] += dynamic_buffer_base;
         } else if (dzn_desc_type_has_sampler(set_layout->bindings[b].type) &&
                  set_layout->bindings[b].immutable_sampler_idx == STATIC_SAMPLER_TAG)
            layout->binding_translation[j].binding_class[b] = DZN_PIPELINE_BINDING_STATIC_SAMPLER;
         else
            layout->binding_translation[j].binding_class[b] = DZN_PIPELINE_BINDING_NORMAL;
      }

      static_sampler_count += set_layout->static_sampler_count;
      dzn_foreach_pool_type (type) {
         layout->sets[j].heap_offsets[type] = layout->desc_count[type];
         layout->desc_count[type] += set_layout->range_desc_count[type];
         for (uint32_t i = 0; i < MAX_SHADER_VISIBILITIES; i++)
            range_count += set_layout->range_count[i][type];
      }

      for (uint32_t b = 0; b < set_layout->binding_count; b++) {
         uint32_t image_offset = ~0u;
         if (!device->bindless && set_layout->bindings[b].descriptor_count) {
            image_offset = dzn_descriptor_set_layout_get_heap_offset(
                set_layout, b, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, false, false);
            if (image_offset != ~0u)
               image_offset += layout->sets[j].heap_offsets[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV];
         }
         layout->binding_translation[j].image_offsets[b] = image_offset;
      }

      if (!device->bindless)
         layout->desc_count[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV] += set_layout->dynamic_buffers.desc_count;

      dynamic_buffer_base += set_layout->dynamic_buffers.count;
      for (uint32_t o = 0, elem = 0; o < set_layout->dynamic_buffers.count; o++, elem++) {
         if (device->bindless) {
            layout->sets[j].dynamic_buffer_heap_offsets[o].primary = layout->dynamic_buffer_count++;
            layout->sets[j].dynamic_buffer_heap_offsets[o].alt = ~0;
         } else {
            uint32_t b = set_layout->dynamic_buffers.bindings[o];

            if (o > 0 && set_layout->dynamic_buffers.bindings[o - 1] != b)
               elem = 0;

            uint32_t heap_offset =
               dzn_descriptor_set_layout_get_heap_offset(set_layout, b, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                         false, false);
            uint32_t alt_heap_offset =
               dzn_descriptor_set_layout_get_heap_offset(set_layout, b, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                         true, false);

            layout->sets[j].dynamic_buffer_heap_offsets[o].primary = heap_offset != ~0 ? heap_offset + elem : ~0;
            layout->sets[j].dynamic_buffer_heap_offsets[o].alt = alt_heap_offset != ~0 ? alt_heap_offset + elem : ~0;
         }
      }
   }

   layout->image_metadata_offset = ~0u;
   if (!device->bindless) {
      layout->image_metadata_offset = layout->desc_count[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV]++;
      range_count++;
   }

   D3D12_DESCRIPTOR_RANGE1 *ranges =
      vk_alloc2(&device->vk.alloc, pAllocator, sizeof(*ranges) * range_count, 8,
                VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (range_count && !ranges) {
      vk_pipeline_layout_unref(&device->vk, &layout->vk);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   static_assert(sizeof(D3D12_STATIC_SAMPLER_DESC1) > sizeof(D3D12_STATIC_SAMPLER_DESC),
                 "Allocating larger array and re-using for smaller struct");
   D3D12_STATIC_SAMPLER_DESC1 *static_sampler_descs =
      vk_alloc2(&device->vk.alloc, pAllocator,
                sizeof(*static_sampler_descs) * static_sampler_count, 8,
                VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (static_sampler_count && !static_sampler_descs) {
      vk_free2(&device->vk.alloc, pAllocator, ranges);
      vk_pipeline_layout_unref(&device->vk, &layout->vk);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }


   D3D12_ROOT_PARAMETER1 root_params[MAX_ROOT_PARAMS] = { 0 };
   D3D12_DESCRIPTOR_RANGE1 *range_ptr = ranges;
   D3D12_ROOT_PARAMETER1 *root_param;
   ASSERTED uint32_t root_dwords = 0;

   if (device->bindless) {
      for (uint32_t j = 0; j < pCreateInfo->setLayoutCount; j++) {
         root_param = &root_params[layout->root.param_count++];
         root_param->ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
         root_param->Descriptor.RegisterSpace = 0;
         root_param->Descriptor.ShaderRegister = j;
         root_param->Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
         root_param->ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
         root_dwords += 2;
      }
   } else {
      for (uint32_t i = 0; i < MAX_SHADER_VISIBILITIES; i++) {
         dzn_foreach_pool_type(type) {
            root_param = &root_params[layout->root.param_count];
            root_param->ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            root_param->DescriptorTable.pDescriptorRanges = range_ptr;
            root_param->DescriptorTable.NumDescriptorRanges = 0;
            root_param->ShaderVisibility = (D3D12_SHADER_VISIBILITY)i;

            for (uint32_t j = 0; j < pCreateInfo->setLayoutCount; j++) {
               VK_FROM_HANDLE(dzn_descriptor_set_layout, set_layout, pCreateInfo->pSetLayouts[j]);
               uint32_t range_count = set_layout->range_count[i][type];

               memcpy(range_ptr, set_layout->ranges[i][type],
                      range_count * sizeof(D3D12_DESCRIPTOR_RANGE1));
               for (uint32_t k = 0; k < range_count; k++) {
                  range_ptr[k].RegisterSpace = j;
                  range_ptr[k].OffsetInDescriptorsFromTableStart +=
                     layout->sets[j].heap_offsets[type];
               }
               root_param->DescriptorTable.NumDescriptorRanges += range_count;
               range_ptr += range_count;
            }

            if (i == D3D12_SHADER_VISIBILITY_ALL &&
                type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
               *range_ptr++ = (D3D12_DESCRIPTOR_RANGE1){
                   .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                   .NumDescriptors = 1,
                   .BaseShaderRegister = DZN_IMAGE_METADATA_BINDING,
                   .RegisterSpace = DZN_REGISTER_SPACE_SYSVALS,
                   .Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
                   .OffsetInDescriptorsFromTableStart = layout->image_metadata_offset,
               };
               root_param->DescriptorTable.NumDescriptorRanges++;
            }

            if (root_param->DescriptorTable.NumDescriptorRanges) {
               layout->root.type[layout->root.param_count++] = (D3D12_DESCRIPTOR_HEAP_TYPE)type;
               root_dwords++;
            }
         }
      }
   }

   layout->root.sets_param_count = layout->root.param_count;

   if (layout->dynamic_buffer_count > 0 && device->bindless) {
      layout->root.dynamic_buffer_bindless_param_idx = layout->root.param_count;
      D3D12_ROOT_PARAMETER1 *root_param = &root_params[layout->root.param_count++];

      root_param->ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
      root_param->Descriptor.RegisterSpace = 0;
      root_param->Descriptor.ShaderRegister = layout->root.sets_param_count;
      root_param->Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
      root_param->ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      root_dwords += 2;
   }

   /* Add our sysval CBV, and make it visible to all shaders */
   layout->root.sysval_cbv_param_idx = layout->root.param_count;
   root_param = &root_params[layout->root.param_count++];
   root_param->ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
   root_param->Descriptor.RegisterSpace = DZN_REGISTER_SPACE_SYSVALS;
   root_param->Constants.ShaderRegister = 0;
   root_param->Constants.Num32BitValues =
       DIV_ROUND_UP(MAX2(offsetof(struct dxil_spirv_vertex_runtime_data, viewport_sizes),
                         sizeof(struct dxil_spirv_compute_runtime_data)),
                    4);
   root_param->ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
   root_dwords += root_param->Constants.Num32BitValues;
   layout->root.viewport_cbv_param_idx = layout->root.param_count;
   root_param = &root_params[layout->root.param_count++];
   root_param->ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
   root_param->Descriptor.RegisterSpace = DZN_REGISTER_SPACE_SYSVALS;
   root_param->Descriptor.ShaderRegister = 1;
   root_param->Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
   root_param->ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
   root_dwords += 2;


   if (pdev->root_sig_version >= D3D_ROOT_SIGNATURE_VERSION_1_2) {
      D3D12_STATIC_SAMPLER_DESC1 *static_sampler_ptr = static_sampler_descs;
      for (uint32_t j = 0; j < pCreateInfo->setLayoutCount; j++) {
         VK_FROM_HANDLE(dzn_descriptor_set_layout, set_layout, pCreateInfo->pSetLayouts[j]);

         memcpy(static_sampler_ptr, set_layout->static_samplers,
                set_layout->static_sampler_count * sizeof(*set_layout->static_samplers));
         if (j > 0) {
            for (uint32_t k = 0; k < set_layout->static_sampler_count; k++)
               static_sampler_ptr[k].RegisterSpace = j;
         }
         static_sampler_ptr += set_layout->static_sampler_count;
      }
   } else {
      D3D12_STATIC_SAMPLER_DESC *static_sampler_ptr = (void *)static_sampler_descs;
      for (uint32_t j = 0; j < pCreateInfo->setLayoutCount; j++) {
         VK_FROM_HANDLE(dzn_descriptor_set_layout, set_layout, pCreateInfo->pSetLayouts[j]);

         for (uint32_t k = 0; k < set_layout->static_sampler_count; k++) {
            memcpy(static_sampler_ptr, &set_layout->static_samplers[k],
                     sizeof(*static_sampler_ptr));
            static_sampler_ptr->RegisterSpace = j;
            static_sampler_ptr++;
         }
      }
   }

   uint32_t push_constant_size = 0;
   uint32_t push_constant_flags = 0;
   for (uint32_t j = 0; j < pCreateInfo->pushConstantRangeCount; j++) {
      const VkPushConstantRange *range = pCreateInfo->pPushConstantRanges + j;
      push_constant_size = MAX2(push_constant_size, range->offset + range->size);
      push_constant_flags |= range->stageFlags;
   }

   layout->root.push_constant_size = push_constant_size;
   if (push_constant_size > 0) {
      layout->root.push_constant_cbv_param_idx = layout->root.param_count;
      D3D12_ROOT_PARAMETER1 *root_param = &root_params[layout->root.param_count++];

      root_param->ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      root_param->Constants.ShaderRegister = 0;
      root_param->Constants.Num32BitValues = align(push_constant_size, 4) / 4;
      root_param->Constants.RegisterSpace = DZN_REGISTER_SPACE_PUSH_CONSTANT;
      root_param->ShaderVisibility = translate_desc_visibility(push_constant_flags);
      root_dwords += root_param->Constants.Num32BitValues;
   }

   assert(layout->root.param_count <= ARRAY_SIZE(root_params));
   assert(root_dwords <= MAX_ROOT_DWORDS);

   D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_sig_desc = {
      .Version = pdev->root_sig_version,
   };
   /* TODO Only enable this flag when needed (optimization) */
   D3D12_ROOT_SIGNATURE_FLAGS root_flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
   if (device->vk.enabled_features.transformFeedback)
      root_flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
   if (device->bindless)
      root_flags |= D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED |
                    D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
   if (pdev->root_sig_version >= D3D_ROOT_SIGNATURE_VERSION_1_2) {
      root_sig_desc.Desc_1_2 = (D3D12_ROOT_SIGNATURE_DESC2){
         .NumParameters = layout->root.param_count,
         .pParameters = layout->root.param_count ? root_params : NULL,
         .NumStaticSamplers = static_sampler_count,
         .pStaticSamplers = static_sampler_descs,
         .Flags = root_flags,
      };
   } else {
      root_sig_desc.Desc_1_1 = (D3D12_ROOT_SIGNATURE_DESC1){
            .NumParameters = layout->root.param_count,
            .pParameters = layout->root.param_count ? root_params : NULL,
            .NumStaticSamplers = static_sampler_count,
            .pStaticSamplers = (void *)static_sampler_descs,
            .Flags = root_flags,
      };
   }

   layout->root.sig = dzn_device_create_root_sig(device, &root_sig_desc);
   vk_free2(&device->vk.alloc, pAllocator, ranges);
   vk_free2(&device->vk.alloc, pAllocator, static_sampler_descs);

   if (!layout->root.sig) {
      vk_pipeline_layout_unref(&device->vk, &layout->vk);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   dzn_pipeline_layout_hash_stages(layout, pCreateInfo);
   *out = dzn_pipeline_layout_to_handle(layout);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
dzn_CreatePipelineLayout(VkDevice device,
                         const VkPipelineLayoutCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkPipelineLayout *pPipelineLayout)
{
   return dzn_pipeline_layout_create(dzn_device_from_handle(device),
                                     pCreateInfo, pAllocator, pPipelineLayout);
}

static void
dzn_descriptor_heap_copy_raw(struct dzn_device *device, struct dzn_descriptor_heap *dst_heap,
                              uint32_t dst_offset, const struct dzn_descriptor_heap *src_heap,
                              uint32_t src_offset, uint32_t count, D3D12_DESCRIPTOR_HEAP_TYPE type);

static void
dzn_descriptor_heap_apply_feedback(struct dzn_device *device, const struct dzn_feedback_scope *scope,
                                    struct dzn_descriptor_heap *destination, uint32_t dst_offset,
                                    const struct dzn_descriptor_heap *source, uint32_t src_offset,
                                    uint32_t count)
{
   if (!scope || !source->feedback)
      return;
   for (unsigned d = 0; d < count; d++) {
      const struct dzn_feedback_descriptor *desc = &source->feedback[src_offset + d];
      if (!desc->image_id)
         continue;
      for (unsigned i = 0; i < scope->count; i++) {
         const struct dzn_feedback_binding *binding = &scope->bindings[i];
         const VkImageSubresourceRange *a = &binding->range, *v = &desc->range;
         if (binding->image_id != desc->image_id || !(a->aspectMask & v->aspectMask) ||
             a->baseMipLevel >= v->baseMipLevel + v->levelCount ||
             v->baseMipLevel >= a->baseMipLevel + a->levelCount ||
             a->baseArrayLayer >= v->baseArrayLayer + v->layerCount ||
             v->baseArrayLayer >= a->baseArrayLayer + a->layerCount)
            continue;
         ID3D12Device1_CreateShaderResourceView(device->dev, binding->resource, &desc->desc,
            dzn_descriptor_heap_get_cpu_handle(destination, dst_offset + d));
         break;
      }
   }
}

static void
dzn_descriptor_heap_notify(struct dzn_descriptor_heap *heap, uint32_t offset, uint32_t count)
{
   if (!heap->device || !count)
      return;
   struct dzn_device *device = heap->device;
   mtx_lock(&device->descriptor_update_lock);
   list_for_each_entry(struct dzn_descriptor_watch, watch, &heap->watches, source_link) {
      uint32_t first = MAX2(offset, watch->source_offset);
      uint32_t end = MIN2(offset + count, watch->source_offset + watch->count);
      if (first >= end)
         continue;
      uint32_t delta = first - watch->source_offset;
      dzn_descriptor_heap_copy_raw(device, watch->destination, watch->destination_offset + delta,
                                    heap, first, end - first, heap->type);
      dzn_descriptor_heap_apply_feedback(device, watch->feedback_scope, watch->destination,
         watch->destination_offset + delta, heap, first, end - first);
      if (watch->metadata) {
         void *mapped;
         D3D12_RANGE read = {0, 0};
         if (FAILED(ID3D12Resource_Map(watch->metadata, 0, &read, &mapped))) {
            vk_device_set_lost(&device->vk, "Descriptor metadata update failed");
            continue;
         }
         uint64_t begin = watch->metadata_offset + delta * sizeof(uint32_t);
         uint64_t bytes = (end - first) * sizeof(uint32_t);
         memcpy((char *)mapped + begin, heap->format_defaults + first, bytes);
         D3D12_RANGE written = {begin, begin + bytes};
         ID3D12Resource_Unmap(watch->metadata, 0, &written);
      }
   }
   mtx_unlock(&device->descriptor_update_lock);
}

void
dzn_cmd_buffer_clear_descriptor_watches(struct dzn_cmd_buffer *cmdbuf)
{
   if (!cmdbuf->descriptor_watches.next)
      return;
   struct dzn_device *device = container_of(cmdbuf->vk.base.device, struct dzn_device, vk);
   mtx_lock(&device->descriptor_update_lock);
   list_for_each_entry_safe(struct dzn_descriptor_watch, watch, &cmdbuf->descriptor_watches, command_link) {
      if (watch->source)
         list_del(&watch->source_link);
      list_del(&watch->command_link);
      free(watch);
   }
   mtx_unlock(&device->descriptor_update_lock);
}

void
dzn_descriptor_set_copy_snapshots(struct dzn_cmd_buffer *cmdbuf, const struct dzn_descriptor_set *set,
                        D3D12_DESCRIPTOR_HEAP_TYPE type, struct dzn_descriptor_heap *destination,
                        uint32_t destination_offset, uint32_t count,
                        const struct dzn_feedback_scope *feedback_scope)
{
   struct dzn_device *device = container_of(cmdbuf->vk.base.device, struct dzn_device, vk);
   struct dzn_descriptor_heap *source = dzn_descriptor_set_heap(set, type);
   mtx_lock(&device->descriptor_update_lock);
   dzn_descriptor_heap_copy_raw(device, destination, destination_offset, source,
                                 set->heap_offsets[type], count, type);
   dzn_descriptor_heap_apply_feedback(device, feedback_scope, destination, destination_offset,
      source, set->heap_offsets[type], count);
   for (unsigned b = 0; b < set->layout->binding_count; b++) {
      const struct dzn_descriptor_set_layout_binding *binding = &set->layout->bindings[b];
      if (!binding->descriptor_count ||
          !(binding->flags & (VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                              VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT)))
         continue;
      if (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER &&
          (!dzn_desc_type_has_sampler(binding->type) || binding->immutable_sampler_idx == STATIC_SAMPLER_TAG))
         continue;
      if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && binding->type == VK_DESCRIPTOR_TYPE_SAMPLER)
         continue;
      unsigned ranges = type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
         dzn_descriptor_type_depends_on_shader_usage(binding->type, false) ? 2 : 1;
      for (unsigned r = 0; r < ranges; r++) {
         const D3D12_DESCRIPTOR_RANGE1 *range =
            &set->layout->ranges[binding->visibility][type][binding->range_idx[type] + r];
         uint32_t start = range->OffsetInDescriptorsFromTableStart;
         if (start >= count)
            continue;
         struct dzn_descriptor_watch *watch = calloc(1, sizeof(*watch));
         if (!watch) {
            vk_command_buffer_set_error(&cmdbuf->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
            break;
         }
         watch->source = source;
         watch->destination = destination;
         watch->feedback_scope = feedback_scope;
         watch->source_offset = set->heap_offsets[type] + start;
         watch->destination_offset = destination_offset + start;
         watch->count = MIN2(range->NumDescriptors, count - start);
         if (binding->variable_size) {
            uint32_t variable_descriptors = binding->type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK ?
               (set->variable_count != 0) : set->variable_count * binding->descriptor_stride;
            watch->count = MIN2(watch->count, variable_descriptors);
         }
         list_addtail(&watch->source_link, &source->watches);
         list_addtail(&watch->command_link, &cmdbuf->descriptor_watches);
      }
   }
   mtx_unlock(&device->descriptor_update_lock);
}

void
dzn_descriptor_watch_metadata(struct dzn_cmd_buffer *cmdbuf, struct dzn_descriptor_heap *heap,
                               uint32_t base, uint32_t count, ID3D12Resource *metadata,
                               uint64_t metadata_offset)
{
   struct dzn_device *device = container_of(cmdbuf->vk.base.device, struct dzn_device, vk);
   mtx_lock(&device->descriptor_update_lock);
   void *mapped;
   D3D12_RANGE read = {0, 0};
   if (FAILED(ID3D12Resource_Map(metadata, 0, &read, &mapped))) {
      vk_command_buffer_set_error(&cmdbuf->vk, VK_ERROR_MEMORY_MAP_FAILED);
      mtx_unlock(&device->descriptor_update_lock);
      return;
   }
   memcpy((char *)mapped + metadata_offset, heap->format_defaults + base, count * sizeof(uint32_t));
   list_for_each_entry(struct dzn_descriptor_watch, watch, &cmdbuf->descriptor_watches, command_link) {
      if (!watch->source || watch->metadata || watch->destination != heap ||
          watch->destination_offset < base || watch->destination_offset + watch->count > base + count)
         continue;
      watch->metadata = metadata;
      watch->metadata_offset = metadata_offset + (watch->destination_offset - base) * sizeof(uint32_t);
      memcpy((char *)mapped + watch->metadata_offset,
             watch->source->format_defaults + watch->source_offset, watch->count * sizeof(uint32_t));
   }
   D3D12_RANGE written = {metadata_offset, metadata_offset + count * sizeof(uint32_t)};
   ID3D12Resource_Unmap(metadata, 0, &written);
   mtx_unlock(&device->descriptor_update_lock);
}

void
dzn_descriptor_heap_finish(struct dzn_descriptor_heap *heap)
{
   if (heap->device) {
      mtx_lock(&heap->device->descriptor_update_lock);
      list_for_each_entry_safe(struct dzn_descriptor_watch, watch, &heap->watches, source_link) {
         list_del(&watch->source_link);
         watch->source = NULL;
      }
      mtx_unlock(&heap->device->descriptor_update_lock);
      heap->device = NULL;
   }
   free(heap->feedback);
   heap->feedback = NULL;
   free(heap->format_defaults);
   heap->format_defaults = NULL;
   if (heap->heap)
      ID3D12DescriptorHeap_Release(heap->heap);
   heap->heap = NULL;
}

VkResult
dzn_descriptor_heap_init(struct dzn_descriptor_heap *heap,
                         struct dzn_device *device,
                         D3D12_DESCRIPTOR_HEAP_TYPE type,
                         uint32_t desc_count,
                         bool shader_visible)
{
   heap->device = device;
   heap->type = type;
   list_inithead(&heap->watches);
   heap->desc_count = desc_count;
   heap->desc_sz = ID3D12Device1_GetDescriptorHandleIncrementSize(device->dev, type);

   D3D12_DESCRIPTOR_HEAP_DESC desc = {
      .Type = type,
      .NumDescriptors = desc_count,
      .Flags = shader_visible ?
               D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE :
               D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
   };

   if (FAILED(ID3D12Device1_CreateDescriptorHeap(device->dev, &desc,
                                                 &IID_ID3D12DescriptorHeap,
                                                 (void **)&heap->heap))) {
      return vk_error(device,
                      shader_visible ?
                      VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
      heap->format_defaults = calloc(desc_count, sizeof(uint32_t));
      if (!heap->format_defaults) {
         ID3D12DescriptorHeap_Release(heap->heap);
         heap->heap = NULL;
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }

   if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
       device->vk.enabled_features.attachmentFeedbackLoopLayout) {
      heap->feedback = calloc(desc_count, sizeof(*heap->feedback));
      if (!heap->feedback) {
         dzn_descriptor_heap_finish(heap);
         heap->heap = NULL;
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }

   D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = dzn_ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap->heap);
   heap->cpu_base = cpu_handle.ptr;
   if (shader_visible) {
      D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = dzn_ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap->heap);
      heap->gpu_base = gpu_handle.ptr;
   }

   return VK_SUCCESS;
}

D3D12_CPU_DESCRIPTOR_HANDLE
dzn_descriptor_heap_get_cpu_handle(const struct dzn_descriptor_heap *heap, uint32_t desc_offset)
{
   return (D3D12_CPU_DESCRIPTOR_HANDLE) {
      .ptr = heap->cpu_base + (desc_offset * heap->desc_sz),
   };
}

D3D12_GPU_DESCRIPTOR_HANDLE
dzn_descriptor_heap_get_gpu_handle(const struct dzn_descriptor_heap *heap, uint32_t desc_offset)
{
   return (D3D12_GPU_DESCRIPTOR_HANDLE) {
      .ptr = heap->gpu_base ? heap->gpu_base + (desc_offset * heap->desc_sz) : 0,
   };
}

void
dzn_descriptor_heap_write_sampler_desc(struct dzn_device *device,
                                       struct dzn_descriptor_heap *heap,
                                       uint32_t desc_offset,
                                       const struct dzn_sampler *sampler)
{
   mtx_lock(&device->descriptor_update_lock);
   struct dzn_physical_device *pdev = container_of(device->vk.physical, struct dzn_physical_device, vk);
   if (device->dev11 && pdev->options14.AdvancedTextureOpsSupported)
      ID3D12Device11_CreateSampler2(device->dev11, &sampler->desc,
                                    dzn_descriptor_heap_get_cpu_handle(heap, desc_offset));
   else
      ID3D12Device1_CreateSampler(device->dev, (D3D12_SAMPLER_DESC *)&sampler->desc,
                                  dzn_descriptor_heap_get_cpu_handle(heap, desc_offset));
   dzn_descriptor_heap_notify(heap, desc_offset, 1);
   mtx_unlock(&device->descriptor_update_lock);
}

static void
dzn_descriptor_heap_write_null_desc(struct dzn_device *device,
                                    struct dzn_descriptor_heap *heap,
                                    uint32_t offset, bool writable,
                                    bool buffer)
{
   mtx_lock(&device->descriptor_update_lock);
   if (heap->feedback)
      memset(&heap->feedback[offset], 0, sizeof(*heap->feedback));

   D3D12_CPU_DESCRIPTOR_HANDLE handle = dzn_descriptor_heap_get_cpu_handle(heap, offset);
   if (writable) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {
         .Format = buffer ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32G32B32A32_FLOAT,
         .ViewDimension = buffer ? D3D12_UAV_DIMENSION_BUFFER : D3D12_UAV_DIMENSION_TEXTURE2D,
      };
      if (buffer) {
         desc.Buffer.NumElements = 1;
         desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
      }
      ID3D12Device1_CreateUnorderedAccessView(device->dev, NULL, NULL, &desc, handle);
   } else {
      D3D12_SHADER_RESOURCE_VIEW_DESC desc = {
         .Format = buffer ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32G32B32A32_FLOAT,
         .ViewDimension = buffer ? D3D12_SRV_DIMENSION_BUFFER : D3D12_SRV_DIMENSION_TEXTURE2D,
         .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
      };
      if (buffer) {
         desc.Buffer.NumElements = 1;
         desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
      } else {
         desc.Texture2D.MipLevels = 1;
      }
      ID3D12Device1_CreateShaderResourceView(device->dev, NULL, &desc, handle);
   }
   dzn_descriptor_heap_notify(heap, offset, 1);
   mtx_unlock(&device->descriptor_update_lock);
}

static uint32_t dzn_format_defaults(VkFormat format, uint32_t mapping)
{
   const struct util_format_description *desc =
       util_format_description(vk_format_to_pipe_format(format));
   uint32_t ones = 0, mapped = 0, present = 0;
   for (unsigned c = 0; c < 4; c++)
      ones |= (desc->swizzle[c] == PIPE_SWIZZLE_1) << c;
   for (unsigned c = 0; c < 4; c++) {
      unsigned source = D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(c, mapping);
      if (source < 4 && desc->swizzle[source] < PIPE_SWIZZLE_0)
         present |= BITFIELD_BIT(c);
      if (source == D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1 ||
          (source < 4 && (ones & BITFIELD_BIT(source))))
         mapped |= BITFIELD_BIT(c);
   }
   return mapped | (present << 4);
}

void
dzn_descriptor_heap_write_image_view_desc(struct dzn_device *device,
                                          struct dzn_descriptor_heap *heap,
                                          uint32_t desc_offset,
                                          bool writeable, bool cube_as_2darray,
                                          const struct dzn_image_view *iview, bool null_resource)
{
   mtx_lock(&device->descriptor_update_lock);
   if (heap->feedback)
      memset(&heap->feedback[desc_offset], 0, sizeof(*heap->feedback));

   heap->format_defaults[desc_offset] =
       iview && !null_resource ? dzn_format_defaults(iview->vk.format, writeable
                                                         ? D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
                                                         : iview->srv_desc.Shader4ComponentMapping)
             : 0;
   if (!iview) {
      dzn_descriptor_heap_write_null_desc(device, heap, desc_offset, writeable, false);
      mtx_unlock(&device->descriptor_update_lock);
      return;
   }

   D3D12_CPU_DESCRIPTOR_HANDLE view_handle =
      dzn_descriptor_heap_get_cpu_handle(heap, desc_offset);
   struct dzn_image *image = container_of(iview->vk.image, struct dzn_image, vk);

   if (!null_resource && !writeable && heap->feedback &&
       (image->vk.usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT)) {
      heap->feedback[desc_offset] = (struct dzn_feedback_descriptor) {
         .image_id = image->feedback_id,
         .range = {iview->vk.aspects, iview->vk.base_mip_level, iview->vk.level_count,
                   image->vk.image_type == VK_IMAGE_TYPE_3D ? 0 : iview->vk.base_array_layer,
                   image->vk.image_type == VK_IMAGE_TYPE_3D ? 1 : iview->vk.layer_count},
         .desc = iview->srv_desc,
      };
   }
   if (writeable) {
      if (iview->uav_desc.ViewDimension == D3D12_UAV_DIMENSION_UNKNOWN)
         _debug_printf("Dozen storage image view lacks UAV: format=%u view_usage=0x%x image_usage=0x%x type=%u\n",
                       iview->vk.format, iview->vk.usage, image->vk.usage, iview->vk.view_type);
      ID3D12Device1_CreateUnorderedAccessView(device->dev, null_resource ? NULL : image->res, NULL, &iview->uav_desc, view_handle);
   } else if (cube_as_2darray &&
              (iview->srv_desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY ||
               iview->srv_desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE)) {
      D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = iview->srv_desc;
      srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      srv_desc.Texture2DArray.PlaneSlice = 0;
      srv_desc.Texture2DArray.ResourceMinLODClamp = iview->srv_min_lod_clamp;
      if (iview->srv_desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY) {
         srv_desc.Texture2DArray.MostDetailedMip =
            iview->srv_desc.TextureCubeArray.MostDetailedMip;
         srv_desc.Texture2DArray.MipLevels =
            iview->srv_desc.TextureCubeArray.MipLevels;
         srv_desc.Texture2DArray.FirstArraySlice =
            iview->srv_desc.TextureCubeArray.First2DArrayFace;
         srv_desc.Texture2DArray.ArraySize =
            iview->srv_desc.TextureCubeArray.NumCubes * 6;
      } else {
         srv_desc.Texture2DArray.MostDetailedMip =
            iview->srv_desc.TextureCube.MostDetailedMip;
         srv_desc.Texture2DArray.MipLevels =
            iview->srv_desc.TextureCube.MipLevels;
         srv_desc.Texture2DArray.FirstArraySlice = 0;
         srv_desc.Texture2DArray.ArraySize = 6;
      }

      if (heap->feedback)
         heap->feedback[desc_offset].desc = srv_desc;
      ID3D12Device1_CreateShaderResourceView(device->dev, null_resource ? NULL : image->res, &srv_desc, view_handle);
   } else {
      ID3D12Device1_CreateShaderResourceView(device->dev, null_resource ? NULL : image->res, &iview->srv_desc, view_handle);
   }
   dzn_descriptor_heap_notify(heap, desc_offset, 1);
   mtx_unlock(&device->descriptor_update_lock);
}

void
dzn_descriptor_heap_write_buffer_view_desc(struct dzn_device *device,
                                           struct dzn_descriptor_heap *heap,
                                           uint32_t desc_offset,
                                           bool writeable,
                                           const struct dzn_buffer_view *bview)
{
   mtx_lock(&device->descriptor_update_lock);
   if (heap->feedback)
      memset(&heap->feedback[desc_offset], 0, sizeof(*heap->feedback));

   heap->format_defaults[desc_offset] =
       bview ? dzn_format_defaults(bview->format, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING) : 0;
   if (!bview) {
      dzn_descriptor_heap_write_null_desc(device, heap, desc_offset, writeable, true);
      mtx_unlock(&device->descriptor_update_lock);
      return;
   }

   D3D12_CPU_DESCRIPTOR_HANDLE view_handle =
      dzn_descriptor_heap_get_cpu_handle(heap, desc_offset);

   if (writeable)
      ID3D12Device1_CreateUnorderedAccessView(device->dev, bview->buffer->res, NULL, &bview->uav_desc, view_handle);
   else
      ID3D12Device1_CreateShaderResourceView(device->dev, bview->buffer->res, &bview->srv_desc, view_handle);
   dzn_descriptor_heap_notify(heap, desc_offset, 1);
   mtx_unlock(&device->descriptor_update_lock);
}

void
dzn_descriptor_heap_write_buffer_desc(struct dzn_device *device,
                                      struct dzn_descriptor_heap *heap,
                                      uint32_t desc_offset,
                                      bool writeable,
                                      const struct dzn_buffer_desc *info)
{
   mtx_lock(&device->descriptor_update_lock);
   if (heap->feedback)
      memset(&heap->feedback[desc_offset], 0, sizeof(*heap->feedback));

   if (!info->buffer) {
      if (info->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
          info->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
         ID3D12Device1_CreateConstantBufferView(device->dev, NULL,
            dzn_descriptor_heap_get_cpu_handle(heap, desc_offset));
      else
         dzn_descriptor_heap_write_null_desc(device, heap, desc_offset, writeable, true);
      dzn_descriptor_heap_notify(heap, desc_offset, 1);
      mtx_unlock(&device->descriptor_update_lock);
      return;
   }

   D3D12_CPU_DESCRIPTOR_HANDLE view_handle =
      dzn_descriptor_heap_get_cpu_handle(heap, desc_offset);

   VkDeviceSize size =
      info->range == VK_WHOLE_SIZE ?
      info->buffer->size - info->offset :
      info->range;

   if (info->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
       info->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
      assert(!writeable);
      D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {
         .BufferLocation = info->buffer->gpuva + info->offset,
         .SizeInBytes = MIN2(ALIGN_POT(size, 256), D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 4 * sizeof(float)),
      };
      ID3D12Device1_CreateConstantBufferView(device->dev, &cbv_desc, view_handle);
   } else if (writeable) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {
         .Format = DXGI_FORMAT_R32_TYPELESS,
         .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
         .Buffer = {
            .FirstElement = info->offset / sizeof(uint32_t),
            .NumElements = (UINT)DIV_ROUND_UP(size, sizeof(uint32_t)),
            .Flags = D3D12_BUFFER_UAV_FLAG_RAW,
         },
      };
      ID3D12Device1_CreateUnorderedAccessView(device->dev, info->buffer->res, NULL, &uav_desc, view_handle);
   } else {
      D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {
         .Format = DXGI_FORMAT_R32_TYPELESS,
         .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
         .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
         .Buffer = {
            .FirstElement = info->offset / sizeof(uint32_t),
            .NumElements = (UINT)DIV_ROUND_UP(size, sizeof(uint32_t)),
            .Flags = D3D12_BUFFER_SRV_FLAG_RAW,
         },
      };
      ID3D12Device1_CreateShaderResourceView(device->dev, info->buffer->res, &srv_desc, view_handle);
   }
   dzn_descriptor_heap_notify(heap, desc_offset, 1);
   mtx_unlock(&device->descriptor_update_lock);
}

static void
dzn_bindless_descriptor_set_write_sampler_desc(volatile struct dxil_spirv_bindless_entry *map,
                                               uint32_t desc_offset,
                                               const struct dzn_sampler *sampler)
{
   map[desc_offset].sampler_idx = sampler->bindless_slot;
}

static void
dzn_bindless_descriptor_set_write_image_view_desc(volatile struct dxil_spirv_bindless_entry *map,
                                                  VkDescriptorType type,
                                                  uint32_t desc_offset,
                                                  const struct dzn_image_view *iview)
{
   switch (type) {
   case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      map[desc_offset].texture_idx = iview->uav_bindless_slot;
      break;
   case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
   case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
   case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      map[desc_offset].texture_idx = iview->srv_bindless_slot;
      break;
   default:
      UNREACHABLE("Unexpected descriptor type");
   }
}

static void
dzn_bindless_descriptor_set_write_buffer_view_desc(volatile struct dxil_spirv_bindless_entry *map,
                                                   VkDescriptorType type,
                                                   uint32_t desc_offset,
                                                   const struct dzn_buffer_view *bview)
{
   switch (type) {
   case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      map[desc_offset].texture_idx = bview->srv_bindless_slot;
      break;
   case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
      map[desc_offset].texture_idx = bview->uav_bindless_slot;
      break;
   default:
      UNREACHABLE("Unexpected descriptor type");
   }
}

static void
dzn_bindless_descriptor_set_write_buffer_desc(struct dzn_device *device,
                                              volatile struct dxil_spirv_bindless_entry *map,
                                              uint32_t desc_offset,
                                              const struct dzn_buffer_desc *info)
{
   dzn_buffer_get_bindless_buffer_descriptor(device, info, &map[desc_offset]);
}

static bool
need_custom_buffer_descriptor(struct dzn_device *device, const struct dzn_buffer_desc *info,
                              struct dzn_buffer_desc *out_desc)
{
   *out_desc = *info;
   uint32_t upper_bound_default_descriptor;
   uint64_t size_align, offset_align;
   /* Canonicalize descriptor types for hash/compare, and get size/align info */
   switch (info->type) {
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      out_desc->type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      FALLTHROUGH;
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      upper_bound_default_descriptor =
         MIN2(D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * sizeof(float) * 4, info->buffer->size);
      size_align = offset_align = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
      break;
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
      out_desc->type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      FALLTHROUGH;
   default:
      upper_bound_default_descriptor = MIN2(UINT32_MAX, info->buffer->size);
      offset_align = D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT;
      size_align = 4;
      break;
   }

   uint64_t upper_bound = info->range == VK_WHOLE_SIZE ?
      info->buffer->size :
      info->offset + info->range;
   /* Addressing the whole buffer, no custom descriptor needed. */
   if (upper_bound == upper_bound_default_descriptor)
      return false;

   out_desc->range = ALIGN_POT(upper_bound, size_align);
   if (out_desc->range <= upper_bound_default_descriptor) {
      /* Use a larger descriptor with the hope that we'll be more likely
       * to be able to re-use it. The shader is already doing the offset
       * add, so there's not really a cost to putting a nonzero value there. */
      out_desc->offset = 0;
   } else {
      /* At least align-down the base offset to ensure that's a valid view to create */
      out_desc->offset = (out_desc->offset / offset_align) * offset_align;
      out_desc->range -= out_desc->offset;
   }
   return true;
}

static uint32_t
hash_buffer_desc(const void *data)
{
   const struct dzn_buffer_desc *bdesc = data;
   /* Avoid any potential padding in the struct */
   uint32_t type_hash = _mesa_hash_data(&bdesc->type, sizeof(bdesc->type));
   return _mesa_hash_data_with_seed(&bdesc->range, sizeof(bdesc->range) * 2, type_hash);
}

static bool
compare_buffer_desc(const void *_a, const void *_b)
{
   const struct dzn_buffer_desc *a = _a, *b = _b;
   assert(a->buffer == b->buffer);
   /* Avoid any potential padding in the struct */
   return a->type == b->type &&
          a->range == b->range &&
          a->offset == b->offset;
}

static int
handle_custom_descriptor_cache(struct dzn_device *device,
                               const struct dzn_buffer_desc *stack_desc)
{
   /* Buffer lock is held */

   /* Initialize hash map */
   if (!stack_desc->buffer->custom_views)
      stack_desc->buffer->custom_views = _mesa_hash_table_create(NULL, hash_buffer_desc, compare_buffer_desc);

   if (!stack_desc->buffer->custom_views)
      return -1;

   uint32_t hash = hash_buffer_desc(stack_desc);
   struct hash_entry *entry = _mesa_hash_table_search_pre_hashed(stack_desc->buffer->custom_views, hash, stack_desc);
   if (entry)
      return (int)(intptr_t)entry->data;

   int slot = dzn_device_descriptor_heap_alloc_slot(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
   if (slot < 0)
      return slot;

   struct dzn_buffer_desc *key = malloc(sizeof(*stack_desc));
   if (!key) {
      dzn_device_descriptor_heap_free_slot(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, slot);
      return -1;
   }

   *key = *stack_desc;
   entry = _mesa_hash_table_insert_pre_hashed(stack_desc->buffer->custom_views, hash, key, (void *)(intptr_t)slot);
   if (!entry) {
      free(key);
      dzn_device_descriptor_heap_free_slot(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, slot);
      return -1;
   }

   dzn_descriptor_heap_write_buffer_desc(device, &device->device_heaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].heap,
                                         slot, key->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, key);
   return slot;
}

void
dzn_buffer_get_bindless_buffer_descriptor(struct dzn_device *device,
                                          const struct dzn_buffer_desc *bdesc,
                                          volatile struct dxil_spirv_bindless_entry *out)
{
   int slot;
   uint32_t offset = bdesc->offset;
   switch (bdesc->type) {
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      slot = bdesc->buffer->cbv_bindless_slot;
      break;
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
      slot = bdesc->buffer->uav_bindless_slot;
      break;
   default:
      UNREACHABLE("Unexpected descriptor type");
   }

   struct dzn_buffer_desc local_desc;
   if (need_custom_buffer_descriptor(device, bdesc, &local_desc)) {
      mtx_lock(&bdesc->buffer->bindless_view_lock);

      int new_slot = handle_custom_descriptor_cache(device, &local_desc);
      if (new_slot >= 0) {
         slot = new_slot;
         offset = bdesc->offset - local_desc.offset;
      }
      /* In the case of cache failure, just use the base view and try
       * shader-based offsetting, it'll probably still work in most cases. */

      mtx_unlock(&bdesc->buffer->bindless_view_lock);
   }

   out->buffer_idx = slot;
   out->buffer_offset = offset;
}

static void
dzn_descriptor_heap_copy_raw(struct dzn_device *device,
                         struct dzn_descriptor_heap *dst_heap,
                         uint32_t dst_offset,
                         const struct dzn_descriptor_heap *src_heap,
                         uint32_t src_offset,
                         uint32_t desc_count,
                         D3D12_DESCRIPTOR_HEAP_TYPE type)
{
   if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
      memmove(dst_heap->format_defaults + dst_offset, src_heap->format_defaults + src_offset,
              desc_count * sizeof(uint32_t));

   if (dst_heap->feedback && src_heap->feedback)
      memmove(dst_heap->feedback + dst_offset, src_heap->feedback + src_offset,
              desc_count * sizeof(*dst_heap->feedback));

   D3D12_CPU_DESCRIPTOR_HANDLE dst_handle =
      dzn_descriptor_heap_get_cpu_handle(dst_heap, dst_offset);
   D3D12_CPU_DESCRIPTOR_HANDLE src_handle =
      dzn_descriptor_heap_get_cpu_handle(src_heap, src_offset);

   ID3D12Device1_CopyDescriptorsSimple(device->dev, desc_count,
                                       dst_handle,
                                       src_handle,
                                       type);
}

void
dzn_descriptor_heap_copy(struct dzn_device *device, struct dzn_descriptor_heap *destination,
                         uint32_t destination_offset, const struct dzn_descriptor_heap *source,
                         uint32_t source_offset, uint32_t count, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
   mtx_lock(&device->descriptor_update_lock);
   dzn_descriptor_heap_copy_raw(device, destination, destination_offset, source, source_offset, count, type);
   dzn_descriptor_heap_notify(destination, destination_offset, count);
   mtx_unlock(&device->descriptor_update_lock);
}

struct dzn_descriptor_set_ptr {
   uint32_t binding, elem;
};

static void
dzn_descriptor_set_ptr_validate(const struct dzn_descriptor_set_layout *layout,
                                struct dzn_descriptor_set_ptr *ptr)
{

   if (ptr->binding >= layout->binding_count) {
      ptr->binding = ~0;
      ptr->elem = ~0;
      return;
   }

   uint32_t desc_count =
      dzn_descriptor_set_layout_get_desc_count(layout, ptr->binding);
   if (ptr->elem >= desc_count) {
      ptr->binding = ~0;
      ptr->elem = ~0;
   }
}

static void
dzn_descriptor_set_ptr_init(const struct dzn_descriptor_set_layout *layout,
                            struct dzn_descriptor_set_ptr *ptr,
                            uint32_t binding, uint32_t elem)
{
   ptr->binding = binding;
   ptr->elem = elem;
   dzn_descriptor_set_ptr_validate(layout, ptr);
}

static void
dzn_descriptor_set_ptr_move(const struct dzn_descriptor_set_layout *layout,
                            struct dzn_descriptor_set_ptr *ptr,
                            uint32_t count)
{
   if (ptr->binding == ~0)
      return;

   while (count) {
      uint32_t desc_count =
         dzn_descriptor_set_layout_get_desc_count(layout, ptr->binding);

      if (count >= desc_count - ptr->elem) {
         count -= desc_count - ptr->elem;
         ptr->binding++;
         ptr->elem = 0;
      } else {
         ptr->elem += count;
         count = 0;
      }
   }

   dzn_descriptor_set_ptr_validate(layout, ptr);
}

static bool
dzn_descriptor_set_ptr_is_valid(const struct dzn_descriptor_set_ptr *ptr)
{
   return ptr->binding != ~0 && ptr->elem != ~0;
}

static uint32_t
dzn_descriptor_set_remaining_descs_in_binding(const struct dzn_descriptor_set_layout *layout,
                                              const struct dzn_descriptor_set_ptr *ptr)
{
   if (ptr->binding >= layout->binding_count)
      return 0;

   uint32_t desc_count =
      dzn_descriptor_set_layout_get_desc_count(layout, ptr->binding);

   return desc_count >= ptr->elem ? desc_count - ptr->elem : 0;
}


static uint32_t
dzn_descriptor_set_ptr_get_heap_offset(const struct dzn_descriptor_set_layout *layout,
                                       D3D12_DESCRIPTOR_HEAP_TYPE type,
                                       const struct dzn_descriptor_set_ptr *ptr,
                                       bool alt,
                                       bool bindless)
{
   if (ptr->binding == ~0)
      return ~0;

   uint32_t base =
      dzn_descriptor_set_layout_get_heap_offset(layout, ptr->binding, type, alt, bindless);
   if (base == ~0)
      return ~0;

   return base + ptr->elem * layout->bindings[ptr->binding].descriptor_stride;
}

static void
dzn_descriptor_set_write_sampler_desc(struct dzn_device *device,
                                      struct dzn_descriptor_set *set,
                                      uint32_t heap_offset,
                                      const struct dzn_sampler *sampler)
{
   if (heap_offset == ~0)
      return;

   D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
   if (device->bindless) {
      dzn_bindless_descriptor_set_write_sampler_desc(set->pool->bindless.map,
                                                     set->heap_offsets[0] + heap_offset,
                                                     sampler);
   } else {
      dzn_descriptor_heap_write_sampler_desc(device,
                                             dzn_descriptor_set_heap(set, type),
                                             set->heap_offsets[type] + heap_offset,
                                             sampler);
   }
}

static void
dzn_descriptor_set_ptr_write_sampler_desc(struct dzn_device *device,
                                          struct dzn_descriptor_set *set,
                                          const struct dzn_descriptor_set_ptr *ptr,
                                          const struct dzn_sampler *sampler)
{
   D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
   uint32_t heap_offset =
      dzn_descriptor_set_ptr_get_heap_offset(set->layout, type, ptr, false, device->bindless);

   dzn_descriptor_set_write_sampler_desc(device, set, heap_offset, sampler);
}

static uint32_t
dzn_descriptor_set_ptr_get_buffer_idx(const struct dzn_descriptor_set_layout *layout,
                                      const struct dzn_descriptor_set_ptr *ptr)
{
   if (ptr->binding == ~0)
      return ~0;

   uint32_t base = layout->bindings[ptr->binding].buffer_idx;

   if (base == ~0)
      return ~0;

   return base + ptr->elem;
}

static void
dzn_descriptor_set_write_dynamic_buffer_desc(struct dzn_device *device,
                                             struct dzn_descriptor_set *set,
                                             uint32_t dynamic_buffer_idx,
                                             const struct dzn_buffer_desc *info)
{
   if (dynamic_buffer_idx == ~0)
      return;

   assert(dynamic_buffer_idx < set->layout->dynamic_buffers.count);
   set->dynamic_buffers[dynamic_buffer_idx] = *info;
}

static void
dzn_descriptor_set_ptr_write_dynamic_buffer_desc(struct dzn_device *device,
                                                 struct dzn_descriptor_set *set,
                                                 const struct dzn_descriptor_set_ptr *ptr,
                                                 const struct dzn_buffer_desc *info)
{
   uint32_t dynamic_buffer_idx =
      dzn_descriptor_set_ptr_get_buffer_idx(set->layout, ptr);

   dzn_descriptor_set_write_dynamic_buffer_desc(device, set, dynamic_buffer_idx, info);
}

static VkDescriptorType
dzn_descriptor_set_ptr_get_vk_type(const struct dzn_descriptor_set_layout *layout,
                                   const struct dzn_descriptor_set_ptr *ptr)
{
   if (ptr->binding >= layout->binding_count)
      return (VkDescriptorType)~0;

   return layout->bindings[ptr->binding].type;
}

static void
dzn_descriptor_set_write_image_view_desc(struct dzn_device *device,
                                         VkDescriptorType desc_type,
                                         struct dzn_descriptor_set *set,
                                         uint32_t heap_offset,
                                         uint32_t alt_heap_offset,
                                         bool cube_as_2darray,
                                         const struct dzn_image_view *iview)
{
   D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

   if (heap_offset == ~0)
      return;

   if (desc_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
       device->vk.enabled_extensions.AMD_shader_image_load_store_lod) {
      for (unsigned mip = 0; mip < DZN_IMAGE_MIP_DESCRIPTORS; mip++) {
         if (device->bindless) {
            set->pool->bindless.map[set->heap_offsets[0] + heap_offset + mip].texture_idx =
               iview ? iview->mip_uav_slots[mip] : 0;
         } else {
            struct dzn_image_view mip_view;
            bool valid = dzn_image_view_get_mip(iview, mip, &mip_view);
            const struct dzn_image_view *view = valid ? &mip_view : iview;
            dzn_descriptor_heap_write_image_view_desc(device, dzn_descriptor_set_heap(set, type),
               set->heap_offsets[type] + heap_offset + mip, true, true, view, !valid);
            if (alt_heap_offset != ~0)
               dzn_descriptor_heap_write_image_view_desc(device, dzn_descriptor_set_heap(set, type),
                  set->heap_offsets[type] + alt_heap_offset + mip, false, true, view, !valid);
            if (!valid && iview) {
               mtx_lock(&device->descriptor_update_lock);
               dzn_descriptor_set_heap(set, type)->format_defaults[set->heap_offsets[type] + heap_offset + mip] =
                  dzn_format_defaults(iview->vk.format, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
               if (alt_heap_offset != ~0)
                  dzn_descriptor_set_heap(set, type)->format_defaults[set->heap_offsets[type] + alt_heap_offset + mip] =
                     dzn_format_defaults(iview->vk.format, iview->srv_desc.Shader4ComponentMapping);
               dzn_descriptor_heap_notify(dzn_descriptor_set_heap(set, type), set->heap_offsets[type] + heap_offset + mip, 1);
               if (alt_heap_offset != ~0)
                  dzn_descriptor_heap_notify(dzn_descriptor_set_heap(set, type), set->heap_offsets[type] + alt_heap_offset + mip, 1);
               mtx_unlock(&device->descriptor_update_lock);
            }
         }
      }
      return;
   }

   if (device->bindless) {
      dzn_bindless_descriptor_set_write_image_view_desc(set->pool->bindless.map,
                                                        desc_type,
                                                        set->heap_offsets[0] + heap_offset,
                                                        iview);
      return;
   }

   bool primary_writable = desc_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

   dzn_descriptor_heap_write_image_view_desc(device,
                                             dzn_descriptor_set_heap(set, type),
                                             set->heap_offsets[type] + heap_offset,
                                             primary_writable, cube_as_2darray,
                                             iview, false);

   if (alt_heap_offset != ~0) {
      assert(primary_writable);
      dzn_descriptor_heap_write_image_view_desc(device,
                                                dzn_descriptor_set_heap(set, type),
                                                set->heap_offsets[type] + alt_heap_offset,
                                                false, cube_as_2darray,
                                                iview, false);
   }
}

static void
dzn_descriptor_set_ptr_write_image_view_desc(struct dzn_device *device,
                                             VkDescriptorType desc_type,
                                             struct dzn_descriptor_set *set,
                                             const struct dzn_descriptor_set_ptr *ptr,
                                             bool cube_as_2darray,
                                             const struct dzn_image_view *iview)
{
   D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
   uint32_t heap_offset =
      dzn_descriptor_set_ptr_get_heap_offset(set->layout, type, ptr, false, device->bindless);
   uint32_t alt_heap_offset =
      dzn_descriptor_set_ptr_get_heap_offset(set->layout, type, ptr, true, device->bindless);

   dzn_descriptor_set_write_image_view_desc(device, desc_type, set, heap_offset, alt_heap_offset,
                                            cube_as_2darray, iview);
}

static void
dzn_descriptor_set_write_buffer_view_desc(struct dzn_device *device,
                                          VkDescriptorType desc_type,
                                          struct dzn_descriptor_set *set,
                                          uint32_t heap_offset,
                                          uint32_t alt_heap_offset,
                                          const struct dzn_buffer_view *bview)
{
   if (heap_offset == ~0)
      return;

   if (device->bindless) {
      dzn_bindless_descriptor_set_write_buffer_view_desc(set->pool->bindless.map,
                                                         desc_type,
                                                         set->heap_offsets[0] + heap_offset,
                                                         bview);
      return;
   }

   D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
   bool primary_writable = desc_type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;

   dzn_descriptor_heap_write_buffer_view_desc(device,
                                              dzn_descriptor_set_heap(set, type),
                                              set->heap_offsets[type] +
                                              heap_offset,
                                              primary_writable, bview);

   if (alt_heap_offset != ~0) {
      assert(primary_writable);
      dzn_descriptor_heap_write_buffer_view_desc(device,
                                                 dzn_descriptor_set_heap(set, type),
                                                 set->heap_offsets[type] +
                                                 alt_heap_offset,
                                                 false, bview);
   }
}

static void
dzn_descriptor_set_ptr_write_buffer_view_desc(struct dzn_device *device,
                                              VkDescriptorType desc_type,
                                              struct dzn_descriptor_set *set,
                                              const struct dzn_descriptor_set_ptr *ptr,
                                              const struct dzn_buffer_view *bview)
{
   D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
   uint32_t heap_offset =
      dzn_descriptor_set_ptr_get_heap_offset(set->layout, type, ptr, false, device->bindless);
   uint32_t alt_heap_offset =
      dzn_descriptor_set_ptr_get_heap_offset(set->layout, type, ptr, true, device->bindless);

   dzn_descriptor_set_write_buffer_view_desc(device, desc_type, set, heap_offset, alt_heap_offset, bview);
}

static void
dzn_descriptor_set_write_buffer_desc(struct dzn_device *device,
                                     VkDescriptorType desc_type,
                                     struct dzn_descriptor_set *set,
                                     uint32_t heap_offset,
                                     uint32_t alt_heap_offset,
                                     const struct dzn_buffer_desc *bdesc)
{
   D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
   if (heap_offset == ~0)
      return;

   if (device->bindless) {
      dzn_bindless_descriptor_set_write_buffer_desc(device,
                                                    set->pool->bindless.map,
                                                    set->heap_offsets[0] + heap_offset,
                                                    bdesc);
      return;
   }

   bool primary_writable = desc_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

   dzn_descriptor_heap_write_buffer_desc(device, dzn_descriptor_set_heap(set, type),
                                         set->heap_offsets[type] + heap_offset,
                                         primary_writable, bdesc);

   if (alt_heap_offset != ~0) {
      assert(primary_writable);
      dzn_descriptor_heap_write_buffer_desc(device, dzn_descriptor_set_heap(set, type),
                                            set->heap_offsets[type] +
                                            alt_heap_offset,
                                            false, bdesc);
   }
}

static void
dzn_descriptor_set_ptr_write_buffer_desc(struct dzn_device *device,
                                         VkDescriptorType desc_type,
                                         struct dzn_descriptor_set *set,
                                         const struct dzn_descriptor_set_ptr *ptr,
                                         const struct dzn_buffer_desc *bdesc)
{
   D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
   uint32_t heap_offset =
      dzn_descriptor_set_ptr_get_heap_offset(set->layout, type, ptr, false, device->bindless);
   uint32_t alt_heap_offset =
      dzn_descriptor_set_ptr_get_heap_offset(set->layout, type, ptr, true, device->bindless);

   dzn_descriptor_set_write_buffer_desc(device, desc_type, set, heap_offset, alt_heap_offset, bdesc);
}

static void
dzn_descriptor_set_init_inline_descs(struct dzn_descriptor_set *set,
                                     struct dzn_device *device)
{
   const struct dzn_descriptor_set_layout *layout = set->layout;
   struct dzn_descriptor_pool *pool = set->pool;

   for (uint32_t b = 0; b < layout->binding_count; b++) {
      if (layout->bindings[b].type != VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK)
         continue;

      uint32_t heap_offset =
         dzn_descriptor_set_layout_get_heap_offset(layout, b, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                   false, device->bindless);
      if (heap_offset == ~0)
         continue;

      uint32_t data_offset = set->inline_offset + layout->bindings[b].inline_offset;
      uint32_t size = layout->bindings[b].variable_size ? set->variable_count : layout->bindings[b].inline_size;
      if (device->bindless) {
         uint32_t w = data_offset / DZN_INLINE_UNIFORM_WINDOW_STRIDE;
         volatile struct dxil_spirv_bindless_entry *entry =
            &pool->bindless.map[set->heap_offsets[0] + heap_offset];
         entry->buffer_idx = pool->inline_data.window_slots[w];
         entry->buffer_offset = data_offset - w * DZN_INLINE_UNIFORM_WINDOW_STRIDE;
      } else {
         struct dzn_descriptor_heap *heap = dzn_descriptor_set_heap(set, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
         if (heap->feedback)
            memset(&heap->feedback[set->heap_offsets[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV] + heap_offset],
                   0, sizeof(*heap->feedback));
         D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {
            .BufferLocation = pool->inline_data.gpuva + data_offset,
            .SizeInBytes = ALIGN_POT(size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT),
         };
         D3D12_CPU_DESCRIPTOR_HANDLE handle =
            dzn_descriptor_heap_get_cpu_handle(dzn_descriptor_set_heap(set, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),
                                               set->heap_offsets[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV] +
                                               heap_offset);
         ID3D12Device1_CreateConstantBufferView(device->dev, size ? &cbv_desc : NULL, handle);
      }
   }
}

static void
dzn_descriptor_set_write_inline_data(struct dzn_descriptor_set *set,
                                     uint32_t binding, uint32_t offset,
                                     const void *data, uint32_t size)
{
   const struct dzn_descriptor_set_layout *layout = set->layout;
   const uint8_t *src = data;

   for (; size && binding < layout->binding_count; binding++, offset = 0) {
      const struct dzn_descriptor_set_layout_binding *binfo = &layout->bindings[binding];
      if (binfo->type != VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK || offset >= binfo->inline_size)
         continue;

      uint32_t n = MIN2(size, binfo->inline_size - offset);
      memcpy(set->pool->inline_data.map + set->inline_offset + binfo->inline_offset + offset,
             src, n);
      src += n;
      size -= n;
   }
}

static void
dzn_descriptor_set_copy_inline_data(struct dzn_descriptor_set *dst_set,
                                    uint32_t dst_binding, uint32_t dst_offset,
                                    const struct dzn_descriptor_set *src_set,
                                    uint32_t src_binding, uint32_t src_offset,
                                    uint32_t size)
{
   const struct dzn_descriptor_set_layout *dst_layout = dst_set->layout;
   const struct dzn_descriptor_set_layout *src_layout = src_set->layout;

   while (size && dst_binding < dst_layout->binding_count &&
          src_binding < src_layout->binding_count) {
      const struct dzn_descriptor_set_layout_binding *dst = &dst_layout->bindings[dst_binding];
      const struct dzn_descriptor_set_layout_binding *src = &src_layout->bindings[src_binding];

      if (dst_offset >= dst->inline_size) {
         dst_binding++;
         dst_offset = 0;
         continue;
      }
      if (src_offset >= src->inline_size) {
         src_binding++;
         src_offset = 0;
         continue;
      }

      uint32_t n = MIN3(size, dst->inline_size - dst_offset, src->inline_size - src_offset);
      memmove(dst_set->pool->inline_data.map + dst_set->inline_offset + dst->inline_offset + dst_offset,
              src_set->pool->inline_data.map + src_set->inline_offset + src->inline_offset + src_offset,
              n);
      dst_offset += n;
      src_offset += n;
      size -= n;
   }
}

static VkResult
dzn_descriptor_set_init(struct dzn_descriptor_set *set,
                        struct dzn_device *device,
                        struct dzn_descriptor_pool *pool,
                        struct dzn_descriptor_set_layout *layout,
                        bool reuse,
                        uint32_t *variable_descriptor_count,
                        uint32_t variable_count,
                        uint32_t inline_size,
                        const uint32_t *pool_desc_count)
{
   vk_object_base_init(&device->vk, &set->base, VK_OBJECT_TYPE_DESCRIPTOR_SET);

   set->pool = pool;
   set->layout = layout;
   set->variable_count = variable_count;
   memcpy(set->pool_desc_count, pool_desc_count, sizeof(set->pool_desc_count));
   set->private_heaps = !device->bindless && layout->binding_count &&
      layout->bindings[layout->binding_count - 1].variable_size;

   if (set->private_heaps) {
      dzn_foreach_pool_type(type) {
         set->heap_offsets[type] = 0;
         set->heap_sizes[type] = layout->range_desc_count[type];
         if (set->heap_sizes[type]) {
            VkResult result = dzn_descriptor_heap_init(&set->heaps[type], device, type,
                                                        set->heap_sizes[type], false);
            if (result != VK_SUCCESS) {
               dzn_foreach_pool_type(t)
                  dzn_descriptor_heap_finish(&set->heaps[t]);
               vk_object_base_finish(&set->base);
               set->layout = NULL;
               set->pool = NULL;
               return result;
            }
         }
      }
   }
   if (!reuse) {
      dzn_foreach_pool_type(type) {
         if (set->private_heaps)
            continue;
         set->heap_offsets[type] = pool->free_offset[type];
         if (device->bindless)
            set->heap_offsets[type] = align(set->heap_offsets[type], 2);
         set->heap_sizes[type] = layout->range_desc_count[type] + variable_descriptor_count[type];
         set->pool->free_offset[type] = set->heap_offsets[type] + set->heap_sizes[type];
      }
      set->inline_offset = pool->inline_data.free_offset;
      set->inline_size = inline_size;
      pool->inline_data.free_offset += set->inline_size;
   }

   if (layout->inline_data_size)
      dzn_descriptor_set_init_inline_descs(set, device);

   /* Pre-fill the immutable samplers */
   if (layout->immutable_sampler_count) {
      for (uint32_t b = 0; b < layout->binding_count; b++) {
         bool has_samplers =
            dzn_desc_type_has_sampler(layout->bindings[b].type);

         if (!has_samplers ||
             layout->bindings[b].immutable_sampler_idx == ~0 ||
             layout->bindings[b].immutable_sampler_idx == STATIC_SAMPLER_TAG)
            continue;

         struct dzn_descriptor_set_ptr ptr;
         const struct dzn_sampler **sampler =
            &layout->immutable_samplers[layout->bindings[b].immutable_sampler_idx];
         for (dzn_descriptor_set_ptr_init(set->layout, &ptr, b, 0);
              dzn_descriptor_set_ptr_is_valid(&ptr) && ptr.binding == b;
              dzn_descriptor_set_ptr_move(set->layout, &ptr, 1)) {
            dzn_descriptor_set_ptr_write_sampler_desc(device, set, &ptr, *sampler);
            sampler++;
         }
      }
   }
   return VK_SUCCESS;
}

static void
dzn_descriptor_set_finish(struct dzn_descriptor_set *set)
{
   if (set->private_heaps) {
      dzn_foreach_pool_type(type)
         dzn_descriptor_heap_finish(&set->heaps[type]);
   }
   vk_object_base_finish(&set->base);
   set->pool = NULL;
   set->layout = NULL;
}

static void
dzn_descriptor_pool_destroy(struct dzn_descriptor_pool *pool,
                            const VkAllocationCallbacks *pAllocator)
{
   if (!pool)
      return;

   struct dzn_device *device = container_of(pool->base.device, struct dzn_device, vk);

   if (pool->inline_data.window_slots) {
      for (uint32_t w = 0; w < pool->inline_data.window_count; w++) {
         if (pool->inline_data.window_slots[w] >= 0)
            dzn_device_descriptor_heap_free_slot(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                 pool->inline_data.window_slots[w]);
      }
      vk_free2(&device->vk.alloc, pAllocator, pool->inline_data.window_slots);
   }

   if (pool->inline_data.buf)
      ID3D12Resource_Release(pool->inline_data.buf);

   for (uint32_t i = 0; i < pool->set_count; i++) {
      if (pool->sets[i].private_heaps) {
         dzn_foreach_pool_type(type)
            dzn_descriptor_heap_finish(&pool->sets[i].heaps[type]);
      }
   }

   if (device->bindless) {
      if (pool->bindless.buf)
         ID3D12Resource_Release(pool->bindless.buf);
   } else {
      dzn_foreach_pool_type(type) {
         if (pool->desc_count[type])
            dzn_descriptor_heap_finish(&pool->heaps[type]);
      }
   }

   vk_object_base_finish(&pool->base);
   vk_free2(&device->vk.alloc, pAllocator, pool);
}

static VkResult
dzn_descriptor_pool_init_inline_data(struct dzn_device *device,
                                     struct dzn_descriptor_pool *pool,
                                     const VkAllocationCallbacks *pAllocator)
{
   D3D12_RESOURCE_DESC buf_desc = {
      .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
      .Width = pool->inline_data.size,
      .Height = 1, .DepthOrArraySize = 1,
      .MipLevels = 1, .SampleDesc = {.Count = 1},
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR
   };
   D3D12_HEAP_PROPERTIES heap_props = { .Type = D3D12_HEAP_TYPE_UPLOAD };
   if (FAILED(ID3D12Device_CreateCommittedResource(device->dev, &heap_props, 0, &buf_desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                   &IID_ID3D12Resource,
                                                   (void **)&pool->inline_data.buf)))
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   pool->inline_data.gpuva = ID3D12Resource_GetGPUVirtualAddress(pool->inline_data.buf);
   ID3D12Resource_Map(pool->inline_data.buf, 0, NULL, (void **)&pool->inline_data.map);
   memset(pool->inline_data.map, 0, pool->inline_data.size);

   if (!device->bindless)
      return VK_SUCCESS;

   pool->inline_data.window_count =
      DIV_ROUND_UP(pool->inline_data.size, DZN_INLINE_UNIFORM_WINDOW_STRIDE);
   pool->inline_data.window_slots =
      vk_alloc2(&device->vk.alloc, pAllocator,
                pool->inline_data.window_count * sizeof(*pool->inline_data.window_slots), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pool->inline_data.window_slots)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   for (uint32_t w = 0; w < pool->inline_data.window_count; w++)
      pool->inline_data.window_slots[w] = -1;

   for (uint32_t w = 0; w < pool->inline_data.window_count; w++) {
      int slot = dzn_device_descriptor_heap_alloc_slot(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      if (slot < 0)
         return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

      uint32_t start = w * DZN_INLINE_UNIFORM_WINDOW_STRIDE;
      struct dzn_descriptor_heap *heap = &device->device_heaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].heap;
      if (heap->feedback)
         memset(&heap->feedback[slot], 0, sizeof(*heap->feedback));
      D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {
         .BufferLocation = pool->inline_data.gpuva + start,
         .SizeInBytes = MIN2(DZN_INLINE_UNIFORM_WINDOW_SIZE, pool->inline_data.size - start),
      };
      ID3D12Device1_CreateConstantBufferView(device->dev, &cbv_desc,
                                             dzn_descriptor_heap_get_cpu_handle(&device->device_heaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].heap, slot));
      pool->inline_data.window_slots[w] = slot;
   }

   return VK_SUCCESS;
}

static VkResult
dzn_descriptor_pool_create(struct dzn_device *device,
                           const VkDescriptorPoolCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkDescriptorPool *out)
{
   VK_MULTIALLOC(ma);
   VK_MULTIALLOC_DECL(&ma, struct dzn_descriptor_pool, pool, 1);
   VK_MULTIALLOC_DECL(&ma, struct dzn_descriptor_set, sets, pCreateInfo->maxSets);

   if (!vk_multialloc_zalloc2(&ma, &device->vk.alloc, pAllocator,
                              VK_SYSTEM_ALLOCATION_SCOPE_OBJECT))
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   pool->alloc = pAllocator ? *pAllocator : device->vk.alloc;
   pool->sets = sets;
   pool->set_count = pCreateInfo->maxSets;

   vk_object_base_init(&device->vk, &pool->base, VK_OBJECT_TYPE_DESCRIPTOR_POOL);

   uint32_t inline_bytes = 0;
   for (uint32_t p = 0; p < pCreateInfo->poolSizeCount; p++) {
      VkDescriptorType type = pCreateInfo->pPoolSizes[p].type;
      uint32_t num_desc = pCreateInfo->pPoolSizes[p].descriptorCount;

      if (type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
         inline_bytes += num_desc;
         continue;
      }

      if (type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
          device->vk.enabled_extensions.AMD_shader_image_load_store_lod)
         num_desc *= DZN_IMAGE_MIP_DESCRIPTORS;

      if (device->bindless) {
         if (!vk_descriptor_type_is_dynamic(type))
            pool->desc_count[0] += num_desc;
      } else {
         switch (type) {
         case VK_DESCRIPTOR_TYPE_SAMPLER:
            pool->desc_count[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER] += num_desc;
            break;
         case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            pool->desc_count[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV] += num_desc;
            pool->desc_count[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER] += num_desc;
            break;
         case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
         case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
         case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
         case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            pool->desc_count[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV] += num_desc;
            break;
         case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
         case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            /* Reserve one UAV and one SRV slot for those. */
            pool->desc_count[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV] += num_desc * 2;
            break;
         case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
         case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            break;
         default:
            UNREACHABLE("Unsupported desc type");
         }
      }
   }

   if (inline_bytes) {
      const VkDescriptorPoolInlineUniformBlockCreateInfo *inline_info =
         vk_find_struct_const(pCreateInfo->pNext, DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO);
      uint32_t inline_bindings = inline_info && inline_info->maxInlineUniformBlockBindings ?
         inline_info->maxInlineUniformBlockBindings : DIV_ROUND_UP(inline_bytes, 4);

      pool->desc_count[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV] += inline_bindings;
      pool->inline_data.size =
         ALIGN_POT(inline_bytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) +
         inline_bindings * D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
   }

   if (device->bindless) {
      if (pool->desc_count[0]) {
         /* Include extra descriptors so that we can align each allocated descriptor set to a 16-byte boundary */
         static_assert(D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT / sizeof(struct dxil_spirv_bindless_entry) == 2,
                       "Ensure only one extra descriptor is needed to produce correct alignments");
         uint32_t extra_descriptors = pool->set_count - 1;
         pool->desc_count[0] += extra_descriptors;

         /* Going to raw APIs to avoid allocating descriptors for this */
         D3D12_RESOURCE_DESC buf_desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = pool->desc_count[0] * sizeof(struct dxil_spirv_bindless_entry),
            .Height = 1, .DepthOrArraySize = 1,
            .MipLevels = 1, .SampleDesc = {.Count = 1},
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR
         };
         D3D12_HEAP_PROPERTIES heap_props = { .Type = D3D12_HEAP_TYPE_UPLOAD };
         if (FAILED(ID3D12Device_CreateCommittedResource(device->dev, &heap_props, 0, &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                         NULL, &IID_ID3D12Resource, (void **)&pool->bindless.buf))) {
            dzn_descriptor_pool_destroy(pool, pAllocator);
            return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         }
         pool->bindless.gpuva = ID3D12Resource_GetGPUVirtualAddress(pool->bindless.buf);
         ID3D12Resource_Map(pool->bindless.buf, 0, NULL, (void **)&pool->bindless.map);
      }
   } else {
      dzn_foreach_pool_type(type) {
         if (!pool->desc_count[type])
            continue;

         VkResult result =
            dzn_descriptor_heap_init(&pool->heaps[type], device, type, pool->desc_count[type], false);
         if (result != VK_SUCCESS) {
            dzn_descriptor_pool_destroy(pool, pAllocator);
            return result;
         }
      }
   }

   if (pool->inline_data.size) {
      VkResult result = dzn_descriptor_pool_init_inline_data(device, pool, pAllocator);
      if (result != VK_SUCCESS) {
         dzn_descriptor_pool_destroy(pool, pAllocator);
         return result;
      }
   }

   *out = dzn_descriptor_pool_to_handle(pool);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
dzn_CreateDescriptorPool(VkDevice device,
                         const VkDescriptorPoolCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkDescriptorPool *pDescriptorPool)
{
   return dzn_descriptor_pool_create(dzn_device_from_handle(device),
                                     pCreateInfo, pAllocator, pDescriptorPool);
}

VKAPI_ATTR void VKAPI_CALL
dzn_DestroyDescriptorPool(VkDevice device,
                          VkDescriptorPool descriptorPool,
                          const VkAllocationCallbacks *pAllocator)
{
   dzn_descriptor_pool_destroy(dzn_descriptor_pool_from_handle(descriptorPool),
                               pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL
dzn_ResetDescriptorPool(VkDevice device,
                        VkDescriptorPool descriptorPool,
                        VkDescriptorPoolResetFlags flags)
{
   VK_FROM_HANDLE(dzn_descriptor_pool, pool, descriptorPool);

   for (uint32_t s = 0; s < pool->set_count; s++)
      dzn_descriptor_set_finish(&pool->sets[s]);

   dzn_foreach_pool_type(type) {
      pool->free_offset[type] = 0;
      pool->used_desc_count[type] = 0;
   }
   pool->inline_data.free_offset = 0;
   pool->inline_data.used = 0;
   pool->used_set_count = 0;

   return VK_SUCCESS;
}

void
dzn_descriptor_heap_pool_finish(struct dzn_descriptor_heap_pool *pool)
{
   list_splicetail(&pool->active_heaps, &pool->free_heaps);
   list_for_each_entry_safe(struct dzn_descriptor_heap_pool_entry, entry, &pool->free_heaps, link) {
      list_del(&entry->link);
      dzn_descriptor_heap_finish(&entry->heap);
      vk_free(pool->alloc, entry);
   }
}

void
dzn_descriptor_heap_pool_init(struct dzn_descriptor_heap_pool *pool,
                              struct dzn_device *device,
                              D3D12_DESCRIPTOR_HEAP_TYPE type,
                              bool shader_visible,
                              const VkAllocationCallbacks *alloc)
{
   assert(!shader_visible ||
          type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
          type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

   pool->alloc = alloc;
   pool->type = type;
   pool->shader_visible = shader_visible;
   list_inithead(&pool->active_heaps);
   list_inithead(&pool->free_heaps);
   pool->offset = 0;
   pool->desc_sz = ID3D12Device1_GetDescriptorHandleIncrementSize(device->dev, type);
}

VkResult
dzn_descriptor_heap_pool_alloc_slots(struct dzn_descriptor_heap_pool *pool,
                                     struct dzn_device *device, uint32_t desc_count,
                                     struct dzn_descriptor_heap **heap,
                                     uint32_t *first_slot)
{
   struct dzn_descriptor_heap *last_heap =
      list_is_empty(&pool->active_heaps) ?
      NULL :
      &(list_last_entry(&pool->active_heaps, struct dzn_descriptor_heap_pool_entry, link)->heap);
   uint32_t last_heap_desc_count =
      last_heap ? last_heap->desc_count : 0;

   if (pool->offset + desc_count > last_heap_desc_count) {
      uint32_t granularity =
         (pool->type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
          pool->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) ?
         64 * 1024 : 4 * 1024;
      uint32_t alloc_step = ALIGN_POT(desc_count * pool->desc_sz, granularity);
      uint32_t heap_desc_count = MAX2(alloc_step / pool->desc_sz, 16);

      /* Maximum of 2048 samplers per heap when shader_visible is true. */
      if (pool->shader_visible &&
          pool->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
         assert(desc_count <= MAX_DESCS_PER_SAMPLER_HEAP);
         heap_desc_count = MIN2(heap_desc_count, MAX_DESCS_PER_SAMPLER_HEAP);
      }

      struct dzn_descriptor_heap_pool_entry *new_heap = NULL;

      list_for_each_entry_safe(struct dzn_descriptor_heap_pool_entry, entry, &pool->free_heaps, link) {
         if (entry->heap.desc_count >= heap_desc_count) {
            new_heap = entry;
            list_del(&entry->link);
            break;
         }
      }

      if (!new_heap) {
         new_heap =
            vk_zalloc(pool->alloc, sizeof(*new_heap), 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
         if (!new_heap)
            return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

         VkResult result =
            dzn_descriptor_heap_init(&new_heap->heap, device, pool->type,
                                     heap_desc_count, pool->shader_visible);
         if (result != VK_SUCCESS) {
            vk_free(&device->vk.alloc, new_heap);
            return result;
         }
      }

      list_addtail(&new_heap->link, &pool->active_heaps);
      pool->offset = 0;
      last_heap = &new_heap->heap;
   }

   *heap = last_heap;
   *first_slot = pool->offset;
   pool->offset += desc_count;
   return VK_SUCCESS;
}

void
dzn_descriptor_heap_pool_reset(struct dzn_descriptor_heap_pool *pool)
{
   pool->offset = 0;
   list_splicetail(&pool->active_heaps, &pool->free_heaps);
   list_inithead(&pool->active_heaps);
}

VKAPI_ATTR VkResult VKAPI_CALL
dzn_AllocateDescriptorSets(VkDevice dev,
                           const VkDescriptorSetAllocateInfo *pAllocateInfo,
                           VkDescriptorSet *pDescriptorSets)
{
   VK_FROM_HANDLE(dzn_descriptor_pool, pool, pAllocateInfo->descriptorPool);
   VK_FROM_HANDLE(dzn_device, device, dev);

   const struct VkDescriptorSetVariableDescriptorCountAllocateInfo *variable_counts =
      vk_find_struct_const(pAllocateInfo->pNext, DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO);

   uint32_t set_idx = 0;
   for (unsigned i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
      VK_FROM_HANDLE(dzn_descriptor_set_layout, layout, pAllocateInfo->pSetLayouts[i]);
      const struct dzn_descriptor_set_layout_binding *variable = layout->binding_count &&
         layout->bindings[layout->binding_count - 1].variable_size ?
         &layout->bindings[layout->binding_count - 1] : NULL;
      uint32_t variable_count = variable && variable_counts && variable_counts->descriptorSetCount ?
         variable_counts->pDescriptorCounts[i] : 0;
      bool private_heaps = variable && !device->bindless;
      uint32_t additional_desc_counts[NUM_POOL_TYPES] = {0};
      if (device->bindless && variable)
         additional_desc_counts[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV] =
            variable->type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK ?
            (variable_count != 0) : variable_count * variable->descriptor_stride;
      uint32_t total_desc_count[NUM_POOL_TYPES];
      uint32_t inline_size = layout->inline_data_size;
      if (variable && variable->type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK)
         inline_size -= ALIGN_POT(variable->inline_size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) -
            ALIGN_POT(variable_count, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
      dzn_foreach_pool_type(type) {
         total_desc_count[type] = layout->range_desc_count[type] + additional_desc_counts[type];
         if (private_heaps) {
            unsigned ranges = type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ?
               (dzn_desc_type_has_sampler(variable->type) && variable->immutable_sampler_idx != STATIC_SAMPLER_TAG) :
               variable->type == VK_DESCRIPTOR_TYPE_SAMPLER ? 0 :
               (dzn_descriptor_type_depends_on_shader_usage(variable->type, false) ? 2 : 1);
            uint32_t maximum = variable->type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK ?
               1 : variable->descriptor_count * variable->descriptor_stride;
            uint32_t actual = variable->type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK ?
               (variable_count != 0) : variable_count * variable->descriptor_stride;
            total_desc_count[type] -= (maximum - actual) * ranges;
         }
         if (pool->used_desc_count[type] + total_desc_count[type] > pool->desc_count[type]) {
            dzn_FreeDescriptorSets(dev, pAllocateInfo->descriptorPool, i, pDescriptorSets);
            return vk_error(device, VK_ERROR_OUT_OF_POOL_MEMORY);
         }
      }
      if (pool->inline_data.used + inline_size > pool->inline_data.size) {
         dzn_FreeDescriptorSets(dev, pAllocateInfo->descriptorPool, i, pDescriptorSets);
         return vk_error(device, VK_ERROR_OUT_OF_POOL_MEMORY);
      }

      struct dzn_descriptor_set *set = NULL;
      bool is_reuse = false;
      bool found_any_unused = false;
      for (; set_idx < pool->set_count; set_idx++) {
         /* Find a free set */
         if (!pool->sets[set_idx].layout) {
            /* Found one. If it's re-use, check if it has enough space */
            if (private_heaps || pool->sets[set_idx].private_heaps) {
               set = &pool->sets[set_idx];
               is_reuse = false;
               break;
            }
            if (set_idx < pool->used_set_count) {
               is_reuse = true;
               found_any_unused = true;
               dzn_foreach_pool_type(type) {
                  if (pool->sets[set_idx].heap_sizes[type] < total_desc_count[type]) {
                     /* No, not enough space */
                     is_reuse = false;
                     break;
                  }
               }
               if (pool->sets[set_idx].inline_size < inline_size)
                  is_reuse = false;
               if (!is_reuse)
                  continue;
            }
            set = &pool->sets[set_idx];
            break;
         }
      }

      /* Either all occupied, or no free space large enough */
      if (!set) {
         dzn_FreeDescriptorSets(dev, pAllocateInfo->descriptorPool, i, pDescriptorSets);
         return vk_error(device, found_any_unused ? VK_ERROR_FRAGMENTED_POOL : VK_ERROR_OUT_OF_POOL_MEMORY);
      }

      /* Couldn't find one for re-use, check if there's enough space at the end */
      if (!is_reuse) {
         dzn_foreach_pool_type(type) {
            if (!private_heaps && pool->free_offset[type] + total_desc_count[type] > pool->desc_count[type]) {
               dzn_FreeDescriptorSets(dev, pAllocateInfo->descriptorPool, i, pDescriptorSets);
               return vk_error(device, VK_ERROR_FRAGMENTED_POOL);
            }
         }
         if (pool->inline_data.free_offset + inline_size > pool->inline_data.size) {
            dzn_FreeDescriptorSets(dev, pAllocateInfo->descriptorPool, i, pDescriptorSets);
            return vk_error(device, VK_ERROR_FRAGMENTED_POOL);
         }
      }

      VkResult result = dzn_descriptor_set_init(set, device, pool, layout, is_reuse, additional_desc_counts,
                                 variable_count, inline_size, total_desc_count);
      if (result != VK_SUCCESS) {
         dzn_FreeDescriptorSets(dev, pAllocateInfo->descriptorPool, i, pDescriptorSets);
         return result;
      }

      pool->used_set_count = MAX2(pool->used_set_count, set_idx + 1);
      dzn_foreach_pool_type(type)
         pool->used_desc_count[type] += total_desc_count[type];
      pool->inline_data.used += set->inline_size;
      pDescriptorSets[i] = dzn_descriptor_set_to_handle(set);
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
dzn_FreeDescriptorSets(VkDevice dev,
                       VkDescriptorPool descriptorPool,
                       uint32_t count,
                       const VkDescriptorSet *pDescriptorSets)
{
   VK_FROM_HANDLE(dzn_descriptor_pool, pool, descriptorPool);

   for (uint32_t s = 0; s < count; s++) {
      VK_FROM_HANDLE(dzn_descriptor_set, set, pDescriptorSets[s]);

      if (!set)
         continue;

      assert(set->pool == pool);
      dzn_foreach_pool_type(type)
         pool->used_desc_count[type] -= set->pool_desc_count[type];
      pool->inline_data.used -= set->inline_size;

      dzn_descriptor_set_finish(set);
   }

   pool->used_set_count = 0;
   dzn_foreach_pool_type(type)
      pool->free_offset[type] = 0;
   pool->inline_data.free_offset = 0;

   for (uint32_t s = 0; s < pool->set_count; s++) {
      const struct dzn_descriptor_set *set = &pool->sets[s];

      if (set->layout) {
         pool->used_set_count = MAX2(pool->used_set_count, s + 1);
         pool->inline_data.free_offset =
            MAX2(pool->inline_data.free_offset, set->inline_offset + set->inline_size);
         dzn_foreach_pool_type (type) {
            if (set->private_heaps)
               continue;
            pool->free_offset[type] =
               MAX2(pool->free_offset[type],
                    set->heap_offsets[type] +
                    set->heap_sizes[type]);
         }
      }
   }

   return VK_SUCCESS;
}

static void
dzn_descriptor_set_write(struct dzn_device *device,
                         const VkWriteDescriptorSet *pDescriptorWrite)
{
   VK_FROM_HANDLE(dzn_descriptor_set, set, pDescriptorWrite->dstSet);

   struct dzn_descriptor_set_ptr ptr;

   dzn_descriptor_set_ptr_init(set->layout, &ptr,
                               pDescriptorWrite->dstBinding,
                               pDescriptorWrite->dstArrayElement);
   uint32_t desc_count = pDescriptorWrite->descriptorCount;

   uint32_t d = 0;
   bool cube_as_2darray =
      pDescriptorWrite->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

   if (pDescriptorWrite->descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
      const VkWriteDescriptorSetInlineUniformBlock *inline_write =
         vk_find_struct_const(pDescriptorWrite->pNext, WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK);
      dzn_descriptor_set_write_inline_data(set, pDescriptorWrite->dstBinding,
                                           pDescriptorWrite->dstArrayElement,
                                           inline_write->pData, inline_write->dataSize);
      return;
   }

   switch (pDescriptorWrite->descriptorType) {
   case VK_DESCRIPTOR_TYPE_SAMPLER:
      for (; dzn_descriptor_set_ptr_is_valid(&ptr) && d < desc_count;
           dzn_descriptor_set_ptr_move(set->layout, &ptr, 1)) {
         assert(dzn_descriptor_set_ptr_get_vk_type(set->layout, &ptr) == pDescriptorWrite->descriptorType);
         const VkDescriptorImageInfo *pImageInfo = pDescriptorWrite->pImageInfo + d;
         VK_FROM_HANDLE(dzn_sampler, sampler, pImageInfo->sampler);

         if (sampler &&
             set->layout->bindings[ptr.binding].immutable_sampler_idx == ~0)
            dzn_descriptor_set_ptr_write_sampler_desc(device, set, &ptr, sampler);

         d++;
      }
      break;
   case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      for (; dzn_descriptor_set_ptr_is_valid(&ptr) && d < desc_count;
           dzn_descriptor_set_ptr_move(set->layout, &ptr, 1)) {
         assert(dzn_descriptor_set_ptr_get_vk_type(set->layout, &ptr) == pDescriptorWrite->descriptorType);
         const VkDescriptorImageInfo *pImageInfo = pDescriptorWrite->pImageInfo + d;
         VK_FROM_HANDLE(dzn_sampler, sampler, pImageInfo->sampler);
         VK_FROM_HANDLE(dzn_image_view, iview, pImageInfo->imageView);

         if (sampler &&
             set->layout->bindings[ptr.binding].immutable_sampler_idx == ~0)
            dzn_descriptor_set_ptr_write_sampler_desc(device, set, &ptr, sampler);

         dzn_descriptor_set_ptr_write_image_view_desc(device, pDescriptorWrite->descriptorType,
                                                         set, &ptr, cube_as_2darray, iview);

         d++;
      }
      break;

   case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
   case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
   case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      for (; dzn_descriptor_set_ptr_is_valid(&ptr) && d < desc_count;
           dzn_descriptor_set_ptr_move(set->layout, &ptr, 1)) {
         assert(dzn_descriptor_set_ptr_get_vk_type(set->layout, &ptr) == pDescriptorWrite->descriptorType);
         const VkDescriptorImageInfo *pImageInfo = pDescriptorWrite->pImageInfo + d;
         VK_FROM_HANDLE(dzn_image_view, iview, pImageInfo->imageView);

         dzn_descriptor_set_ptr_write_image_view_desc(device, pDescriptorWrite->descriptorType,
                                                         set, &ptr, cube_as_2darray, iview);

         d++;
      }
      break;
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      for (; dzn_descriptor_set_ptr_is_valid(&ptr) && d < desc_count;
           dzn_descriptor_set_ptr_move(set->layout, &ptr, 1)) {
         assert(dzn_descriptor_set_ptr_get_vk_type(set->layout, &ptr) == pDescriptorWrite->descriptorType);
         const VkDescriptorBufferInfo *binfo = &pDescriptorWrite->pBufferInfo[d];
         struct dzn_buffer_desc desc = {
            pDescriptorWrite->descriptorType,
            dzn_buffer_from_handle(binfo->buffer),
            binfo->range, binfo->offset
         };

         dzn_descriptor_set_ptr_write_buffer_desc(device, pDescriptorWrite->descriptorType, set, &ptr, &desc);

         d++;
      }
      break;

   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
      for (; dzn_descriptor_set_ptr_is_valid(&ptr) && d < desc_count;
           dzn_descriptor_set_ptr_move(set->layout, &ptr, 1)) {
         assert(dzn_descriptor_set_ptr_get_vk_type(set->layout, &ptr) == pDescriptorWrite->descriptorType);
         const VkDescriptorBufferInfo *binfo = &pDescriptorWrite->pBufferInfo[d];
         struct dzn_buffer_desc desc = {
            pDescriptorWrite->descriptorType,
            dzn_buffer_from_handle(binfo->buffer),
            binfo->range, binfo->offset
         };

         dzn_descriptor_set_ptr_write_dynamic_buffer_desc(device, set, &ptr, &desc);

         d++;
      }
      break;

   case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
      for (; dzn_descriptor_set_ptr_is_valid(&ptr) && d < desc_count;
           dzn_descriptor_set_ptr_move(set->layout, &ptr, 1)) {
         assert(dzn_descriptor_set_ptr_get_vk_type(set->layout, &ptr) == pDescriptorWrite->descriptorType);
         VK_FROM_HANDLE(dzn_buffer_view, bview, pDescriptorWrite->pTexelBufferView[d]);

         dzn_descriptor_set_ptr_write_buffer_view_desc(device, pDescriptorWrite->descriptorType,
                                                          set, &ptr, bview);

         d++;
      }
      break;

   default:
      UNREACHABLE("invalid descriptor type");
      break;
   }

   assert(d == pDescriptorWrite->descriptorCount);
}

static void
dzn_descriptor_set_copy(struct dzn_device *device,
                        const VkCopyDescriptorSet *pDescriptorCopy)
{
   VK_FROM_HANDLE(dzn_descriptor_set, src_set, pDescriptorCopy->srcSet);
   VK_FROM_HANDLE(dzn_descriptor_set, dst_set, pDescriptorCopy->dstSet);
   struct dzn_descriptor_set_ptr src_ptr, dst_ptr;

   if (pDescriptorCopy->srcBinding < src_set->layout->binding_count &&
       src_set->layout->bindings[pDescriptorCopy->srcBinding].type ==
       VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
      dzn_descriptor_set_copy_inline_data(dst_set, pDescriptorCopy->dstBinding,
                                          pDescriptorCopy->dstArrayElement,
                                          src_set, pDescriptorCopy->srcBinding,
                                          pDescriptorCopy->srcArrayElement,
                                          pDescriptorCopy->descriptorCount);
      return;
   }

   dzn_descriptor_set_ptr_init(src_set->layout, &src_ptr,
                               pDescriptorCopy->srcBinding,
                               pDescriptorCopy->srcArrayElement);
   dzn_descriptor_set_ptr_init(dst_set->layout, &dst_ptr,
                               pDescriptorCopy->dstBinding,
                               pDescriptorCopy->dstArrayElement);

   uint32_t copied_count = 0;

   while (dzn_descriptor_set_ptr_is_valid(&src_ptr) &&
          dzn_descriptor_set_ptr_is_valid(&dst_ptr) &&
          copied_count < pDescriptorCopy->descriptorCount) {
      VkDescriptorType src_type =
         dzn_descriptor_set_ptr_get_vk_type(src_set->layout, &src_ptr);
      ASSERTED VkDescriptorType dst_type =
         dzn_descriptor_set_ptr_get_vk_type(dst_set->layout, &dst_ptr);

      assert(src_type == dst_type);
      uint32_t count =
         MIN2(dzn_descriptor_set_remaining_descs_in_binding(src_set->layout, &src_ptr),
              dzn_descriptor_set_remaining_descs_in_binding(dst_set->layout, &dst_ptr));

      count = MIN2(count, pDescriptorCopy->descriptorCount - copied_count);
      uint32_t heap_count = count * src_set->layout->bindings[src_ptr.binding].descriptor_stride;

      if (vk_descriptor_type_is_dynamic(src_type)) {
         uint32_t src_idx =
            dzn_descriptor_set_ptr_get_buffer_idx(src_set->layout, &src_ptr);
         uint32_t dst_idx =
            dzn_descriptor_set_ptr_get_buffer_idx(dst_set->layout, &dst_ptr);

         memcpy(&dst_set->dynamic_buffers[dst_idx],
                &src_set->dynamic_buffers[src_idx],
                sizeof(*dst_set->dynamic_buffers) * count);
      } else {
         dzn_foreach_pool_type(type) {
            uint32_t src_heap_offset =
               dzn_descriptor_set_ptr_get_heap_offset(src_set->layout, type, &src_ptr, false, device->bindless);
            uint32_t dst_heap_offset =
               dzn_descriptor_set_ptr_get_heap_offset(dst_set->layout, type, &dst_ptr, false, device->bindless);

            if (src_heap_offset == ~0) {
               assert(dst_heap_offset == ~0);
               continue;
            }

            src_heap_offset += src_set->heap_offsets[type];
            dst_heap_offset += dst_set->heap_offsets[type];

            if (device->bindless) {
               memcpy((void *)&dst_set->pool->bindless.map[dst_heap_offset],
                      (const void *)&src_set->pool->bindless.map[src_heap_offset],
                      sizeof(src_set->pool->bindless.map[0]) * heap_count);
               /* There's never a reason to loop and memcpy again for bindless */
               break;
            } else {
               dzn_descriptor_heap_copy(device,
                                        dzn_descriptor_set_heap(dst_set, type),
                                        dst_heap_offset,
                                        dzn_descriptor_set_heap(src_set, type),
                                        src_heap_offset,
                                        heap_count, type);
            }

            if (dzn_descriptor_type_depends_on_shader_usage(src_type, device->bindless)) {
               src_heap_offset = src_set->heap_offsets[type] +
                  dzn_descriptor_set_ptr_get_heap_offset(src_set->layout, type, &src_ptr, true, device->bindless);
               dst_heap_offset = dst_set->heap_offsets[type] +
                  dzn_descriptor_set_ptr_get_heap_offset(dst_set->layout, type, &dst_ptr, true, device->bindless);
               assert(src_heap_offset != ~0);
               assert(dst_heap_offset != ~0);
               dzn_descriptor_heap_copy(device,
                                        dzn_descriptor_set_heap(dst_set, type),
                                        dst_heap_offset,
                                        dzn_descriptor_set_heap(src_set, type),
                                        src_heap_offset,
                                        heap_count, type);
            }
         }
      }

      dzn_descriptor_set_ptr_move(src_set->layout, &src_ptr, count);
      dzn_descriptor_set_ptr_move(dst_set->layout, &dst_ptr, count);
      copied_count += count;
   }

   assert(copied_count == pDescriptorCopy->descriptorCount);
}

VKAPI_ATTR void VKAPI_CALL
dzn_UpdateDescriptorSets(VkDevice _device,
                         uint32_t descriptorWriteCount,
                         const VkWriteDescriptorSet *pDescriptorWrites,
                         uint32_t descriptorCopyCount,
                         const VkCopyDescriptorSet *pDescriptorCopies)
{
   VK_FROM_HANDLE(dzn_device, device, _device);
   for (unsigned i = 0; i < descriptorWriteCount; i++)
      dzn_descriptor_set_write(device, &pDescriptorWrites[i]);

   for (unsigned i = 0; i < descriptorCopyCount; i++)
      dzn_descriptor_set_copy(device, &pDescriptorCopies[i]);
}

VkResult
dzn_descriptor_set_push_allocate(struct dzn_device *device,
                                 const struct dzn_descriptor_set_layout *layout,
                                 const struct dzn_descriptor_set *previous,
                                 VkDescriptorPool *out_pool,
                                 VkDescriptorSet *out_set)
{
   if (!(layout->vk.flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR))
      return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);

   uint32_t descriptor_counts[VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT + 1] = { 0 };
   uint32_t total_push_descriptors = 0;
   for (uint32_t b = 0; b < layout->binding_count; b++) {
      const struct dzn_descriptor_set_layout_binding *binding = &layout->bindings[b];
      if (UINT32_MAX - total_push_descriptors < binding->descriptor_count)
         return vk_error(device, VK_ERROR_OUT_OF_POOL_MEMORY);
      total_push_descriptors += binding->descriptor_count;

      if (binding->descriptor_count &&
          (binding->type > VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT ||
           vk_descriptor_type_is_dynamic(binding->type)))
         return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);

      /* Static samplers are immutable root-signature state, not pool entries. */
      uint32_t heap_desc_count = dzn_descriptor_set_layout_get_desc_count(layout, b);
      if (!heap_desc_count)
         continue;
      if (heap_desc_count > UINT32_MAX - descriptor_counts[binding->type])
         return vk_error(device, VK_ERROR_OUT_OF_POOL_MEMORY);
      descriptor_counts[binding->type] += heap_desc_count;
   }
   if (total_push_descriptors > DZN_MAX_PUSH_DESCRIPTORS)
      return vk_error(device, VK_ERROR_OUT_OF_POOL_MEMORY);

   VkDescriptorPoolSize pool_sizes[VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT + 1];
   uint32_t pool_size_count = 0;
   for (uint32_t type = 0; type < ARRAY_SIZE(descriptor_counts); type++) {
      if (descriptor_counts[type]) {
         pool_sizes[pool_size_count++] = (VkDescriptorPoolSize) {
            .type = (VkDescriptorType)type,
            .descriptorCount = descriptor_counts[type],
         };
      }
   }

   VkDescriptorPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = pool_size_count,
      .pPoolSizes = pool_sizes,
   };
   VkDescriptorPool pool = VK_NULL_HANDLE;
   VkResult result = dzn_descriptor_pool_create(device, &pool_info, NULL, &pool);
   if (result != VK_SUCCESS)
      return result;

   VkDescriptorSetLayout layout_handle =
      dzn_descriptor_set_layout_to_handle((struct dzn_descriptor_set_layout *)layout);
   VkDescriptorSetAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &layout_handle,
   };
   VkDescriptorSet set = VK_NULL_HANDLE;
   result = dzn_AllocateDescriptorSets(dzn_device_to_handle(device), &alloc_info, &set);
   if (result != VK_SUCCESS) {
      dzn_descriptor_pool_destroy(dzn_descriptor_pool_from_handle(pool), NULL);
      return result;
   }

   if (previous &&
       memcmp(previous->layout->vk.blake3, layout->vk.blake3,
              sizeof(layout->vk.blake3)) == 0) {
      VkDescriptorSet previous_handle =
         dzn_descriptor_set_to_handle((struct dzn_descriptor_set *)previous);
      for (uint32_t b = 0; b < layout->binding_count; b++) {
         uint32_t desc_count = dzn_descriptor_set_layout_get_desc_count(layout, b);
         if (!desc_count)
            continue;
         VkCopyDescriptorSet copy = {
            .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
            .srcSet = previous_handle,
            .srcBinding = b,
            .dstSet = set,
            .dstBinding = b,
            .descriptorCount = desc_count,
         };
         dzn_descriptor_set_copy(device, &copy);
      }
   }

   *out_pool = pool;
   *out_set = set;
   return VK_SUCCESS;
}

void
dzn_descriptor_set_push_update(struct dzn_device *device,
                               VkDescriptorSet set,
                               uint32_t write_count,
                               const VkWriteDescriptorSet *writes)
{
   for (uint32_t i = 0; i < write_count; i++) {
      VkWriteDescriptorSet write = writes[i];
      write.dstSet = set;
      dzn_descriptor_set_write(device, &write);
   }
}

static void
dzn_descriptor_update_template_destroy(struct dzn_descriptor_update_template *templ,
                                       const VkAllocationCallbacks *alloc)
{
   if (!templ)
      return;

   struct dzn_device *device =
      container_of(templ->base.device, struct dzn_device, vk);

   if (templ->pipeline_layout)
      vk_pipeline_layout_unref(&device->vk, &templ->pipeline_layout->vk);
   vk_object_base_finish(&templ->base);
   vk_free2(&device->vk.alloc, alloc, templ);
}

static VkResult
dzn_descriptor_update_template_create(struct dzn_device *device,
                                      const VkDescriptorUpdateTemplateCreateInfo *info,
                                      const VkAllocationCallbacks *alloc,
                                      VkDescriptorUpdateTemplate *out)
{
   struct dzn_descriptor_set_layout *set_layout = NULL;
   struct dzn_pipeline_layout *push_layout = NULL;
   switch (info->templateType) {
   case VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET: {
      VK_FROM_HANDLE(dzn_descriptor_set_layout, layout, info->descriptorSetLayout);
      set_layout = layout;
      break;
   }
   case VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS: {
      VK_FROM_HANDLE(dzn_pipeline_layout, pipeline_layout, info->pipelineLayout);
      if (!pipeline_layout || info->set >= pipeline_layout->set_count)
         return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      push_layout = pipeline_layout;
      set_layout = container_of(pipeline_layout->vk.set_layouts[info->set],
                                struct dzn_descriptor_set_layout, vk);
      if (!(set_layout->vk.flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR))
         return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);
      break;
   }
   default:
      return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);
   }

   uint32_t entry_count = 0;
   for (uint32_t e = 0; e < info->descriptorUpdateEntryCount; e++) {
      struct dzn_descriptor_set_ptr ptr;
      dzn_descriptor_set_ptr_init(set_layout, &ptr,
                                  info->pDescriptorUpdateEntries[e].dstBinding,
                                  info->pDescriptorUpdateEntries[e].dstArrayElement);
      uint32_t desc_count = info->pDescriptorUpdateEntries[e].descriptorCount;
      ASSERTED VkDescriptorType type = info->pDescriptorUpdateEntries[e].descriptorType;
      uint32_t d = 0;

      if (type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
         entry_count++;
         continue;
      }

      while (dzn_descriptor_set_ptr_is_valid(&ptr) && d < desc_count) {
         uint32_t ndescs = dzn_descriptor_set_remaining_descs_in_binding(set_layout, &ptr);

         assert(dzn_descriptor_set_ptr_get_vk_type(set_layout, &ptr) == type);
         d += ndescs;
         dzn_descriptor_set_ptr_move(set_layout, &ptr, ndescs);
         entry_count++;
      }

      assert(d >= desc_count);
   }

   VK_MULTIALLOC(ma);
   VK_MULTIALLOC_DECL(&ma, struct dzn_descriptor_update_template, templ, 1);
   VK_MULTIALLOC_DECL(&ma, struct dzn_descriptor_update_template_entry, entries, entry_count);

   if (!vk_multialloc_zalloc2(&ma, &device->vk.alloc, alloc,
                              VK_SYSTEM_ALLOCATION_SCOPE_OBJECT))
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &templ->base, VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE);
   templ->type = info->templateType;
   templ->bind_point = info->pipelineBindPoint;
   templ->set = info->set;
   templ->pipeline_layout = push_layout;
   if (push_layout)
      vk_pipeline_layout_ref(&push_layout->vk);
   templ->entry_count = entry_count;
   templ->entries = entries;

   struct dzn_descriptor_update_template_entry *entry = entries;

   for (uint32_t e = 0; e < info->descriptorUpdateEntryCount; e++) {
      struct dzn_descriptor_set_ptr ptr;
      dzn_descriptor_set_ptr_init(set_layout, &ptr,
                                  info->pDescriptorUpdateEntries[e].dstBinding,
                                  info->pDescriptorUpdateEntries[e].dstArrayElement);
      uint32_t desc_count = info->pDescriptorUpdateEntries[e].descriptorCount;
      VkDescriptorType type = info->pDescriptorUpdateEntries[e].descriptorType;
      size_t user_data_offset = info->pDescriptorUpdateEntries[e].offset;
      size_t user_data_stride = info->pDescriptorUpdateEntries[e].stride;
      uint32_t d = 0;

      if (type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
         entry->type = type;
         entry->desc_count = desc_count;
         entry->user_data.offset = user_data_offset;
         entry->inline_binding = info->pDescriptorUpdateEntries[e].dstBinding;
         entry->inline_offset = info->pDescriptorUpdateEntries[e].dstArrayElement;
         ++entry;
         continue;
      }

      while (dzn_descriptor_set_ptr_is_valid(&ptr) && d < desc_count) {
         uint32_t ndescs = dzn_descriptor_set_remaining_descs_in_binding(set_layout, &ptr);

         entry->type = type;
         entry->immutable_sampler =
            dzn_desc_type_has_sampler(type) &&
            set_layout->bindings[ptr.binding].immutable_sampler_idx != ~0;
         entry->desc_count = MIN2(desc_count - d, ndescs);
         entry->user_data.stride = user_data_stride;
         entry->user_data.offset = user_data_offset;
         memset(&entry->heap_offsets, ~0, sizeof(entry->heap_offsets));

         assert(dzn_descriptor_set_ptr_get_vk_type(set_layout, &ptr) == type);
         if (dzn_desc_type_has_sampler(type)) {
            entry->heap_offsets.sampler =
               dzn_descriptor_set_ptr_get_heap_offset(set_layout,
                                                      D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                                      &ptr, false, device->bindless);
         }

         if (vk_descriptor_type_is_dynamic(type)) {
            entry->buffer_idx = dzn_descriptor_set_ptr_get_buffer_idx(set_layout, &ptr);
         } else if (type != VK_DESCRIPTOR_TYPE_SAMPLER) {
            if (is_buffer_desc_type_without_view(type))
               entry->buffer_idx = dzn_descriptor_set_ptr_get_buffer_idx(set_layout, &ptr);

            entry->heap_offsets.cbv_srv_uav =
               dzn_descriptor_set_ptr_get_heap_offset(set_layout,
                                                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                      &ptr, false, device->bindless);
            if (dzn_descriptor_type_depends_on_shader_usage(type, device->bindless)) {
               entry->heap_offsets.extra_srv =
                  dzn_descriptor_set_ptr_get_heap_offset(set_layout,
                                                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                         &ptr, true, device->bindless);
            }
         }

         d += ndescs;
         dzn_descriptor_set_ptr_move(set_layout, &ptr, ndescs);
         user_data_offset += user_data_stride * ndescs;
         ++entry;
      }
   }

   *out = dzn_descriptor_update_template_to_handle(templ);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
dzn_CreateDescriptorUpdateTemplate(VkDevice device,
                                   const VkDescriptorUpdateTemplateCreateInfo *pCreateInfo,
                                   const VkAllocationCallbacks *pAllocator,
                                   VkDescriptorUpdateTemplate *pDescriptorUpdateTemplate)
{
   return dzn_descriptor_update_template_create(dzn_device_from_handle(device),
                                                pCreateInfo, pAllocator,
                                                pDescriptorUpdateTemplate);
}

VKAPI_ATTR void VKAPI_CALL
dzn_DestroyDescriptorUpdateTemplate(VkDevice device,
                                    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                    const VkAllocationCallbacks *pAllocator)
{
   dzn_descriptor_update_template_destroy(dzn_descriptor_update_template_from_handle(descriptorUpdateTemplate),
                                          pAllocator);
}

static const void *
dzn_descriptor_update_template_get_desc_data(const struct dzn_descriptor_update_template *templ,
                                             uint32_t e, uint32_t d,
                                             const void *user_data)
{
   return (const void *)((const uint8_t *)user_data +
                         templ->entries[e].user_data.offset +
                         (d * templ->entries[e].user_data.stride));
}

VKAPI_ATTR void VKAPI_CALL
dzn_UpdateDescriptorSetWithTemplate(VkDevice _device,
                                    VkDescriptorSet descriptorSet,
                                    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                    const void *pData)
{
   VK_FROM_HANDLE(dzn_device, device, _device);
   VK_FROM_HANDLE(dzn_descriptor_set, set, descriptorSet);
   VK_FROM_HANDLE(dzn_descriptor_update_template, templ, descriptorUpdateTemplate);

   for (uint32_t e = 0; e < templ->entry_count; e++) {
      const struct dzn_descriptor_update_template_entry *entry = &templ->entries[e];
      bool cube_as_2darray =
         entry->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

      switch (entry->type) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
         for (uint32_t d = 0; d < entry->desc_count; d++) {
            const VkDescriptorImageInfo *info = (const VkDescriptorImageInfo *)
               dzn_descriptor_update_template_get_desc_data(templ, e, d, pData);
            VK_FROM_HANDLE(dzn_sampler, sampler, info->sampler);

            if (sampler && !entry->immutable_sampler)
               dzn_descriptor_set_write_sampler_desc(device, set, entry->heap_offsets.sampler + d, sampler);
         }
         break;

      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         for (uint32_t d = 0; d < entry->desc_count; d++) {
            const VkDescriptorImageInfo *info = (const VkDescriptorImageInfo *)
               dzn_descriptor_update_template_get_desc_data(templ, e, d, pData);
            VK_FROM_HANDLE(dzn_sampler, sampler, info->sampler);
            VK_FROM_HANDLE(dzn_image_view, iview, info->imageView);

            if (sampler && !entry->immutable_sampler)
               dzn_descriptor_set_write_sampler_desc(device, set, entry->heap_offsets.sampler + d, sampler);

            dzn_descriptor_set_write_image_view_desc(device, entry->type, set,
                                                        entry->heap_offsets.cbv_srv_uav + d, ~0,
                                                        cube_as_2darray, iview);
         }
         break;

      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
         for (uint32_t d = 0; d < entry->desc_count; d++) {
            const VkDescriptorImageInfo *info = (const VkDescriptorImageInfo *)
               dzn_descriptor_update_template_get_desc_data(templ, e, d, pData);
            unsigned stride = entry->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
               device->vk.enabled_extensions.AMD_shader_image_load_store_lod ?
               DZN_IMAGE_MIP_DESCRIPTORS : 1;
            uint32_t heap_offset = entry->heap_offsets.cbv_srv_uav + d * stride;
            uint32_t alt_heap_offset =
               dzn_descriptor_type_depends_on_shader_usage(entry->type, device->bindless) ?
               entry->heap_offsets.extra_srv + d * stride : ~0;
            VK_FROM_HANDLE(dzn_image_view, iview, info->imageView);

            dzn_descriptor_set_write_image_view_desc(device, entry->type, set,
                                                        heap_offset, alt_heap_offset,
                                                        cube_as_2darray, iview);
         }
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         for (uint32_t d = 0; d < entry->desc_count; d++) {
            const VkDescriptorBufferInfo *info = (const VkDescriptorBufferInfo *)
               dzn_descriptor_update_template_get_desc_data(templ, e, d, pData);
            uint32_t heap_offset = entry->heap_offsets.cbv_srv_uav + d;
            uint32_t alt_heap_offset =
               dzn_descriptor_type_depends_on_shader_usage(entry->type, device->bindless) ?
               entry->heap_offsets.extra_srv + d : ~0;

            struct dzn_buffer_desc desc = {
               entry->type,
               dzn_buffer_from_handle(info->buffer),
               info->range, info->offset
            };

            dzn_descriptor_set_write_buffer_desc(device, entry->type, set, heap_offset,
                                                    alt_heap_offset, &desc);
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         for (uint32_t d = 0; d < entry->desc_count; d++) {
            const VkDescriptorBufferInfo *info = (const VkDescriptorBufferInfo *)
               dzn_descriptor_update_template_get_desc_data(templ, e, d, pData);
            uint32_t dyn_buf_idx = entry->buffer_idx + d;

            struct dzn_buffer_desc desc = {
               entry->type,
               dzn_buffer_from_handle(info->buffer),
               info->range, info->offset
            };

            dzn_descriptor_set_write_dynamic_buffer_desc(device, set, dyn_buf_idx, &desc);
         }
         break;

      case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
         dzn_descriptor_set_write_inline_data(set, entry->inline_binding, entry->inline_offset,
                                              (const uint8_t *)pData + entry->user_data.offset,
                                              entry->desc_count);
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         for (uint32_t d = 0; d < entry->desc_count; d++) {
            VkBufferView *info = (VkBufferView *)
               dzn_descriptor_update_template_get_desc_data(templ, e, d, pData);
            VK_FROM_HANDLE(dzn_buffer_view, bview, *info);
            uint32_t heap_offset = entry->heap_offsets.cbv_srv_uav + d;
            uint32_t alt_heap_offset =
               dzn_descriptor_type_depends_on_shader_usage(entry->type, device->bindless) ?
               entry->heap_offsets.extra_srv + d : ~0;

            dzn_descriptor_set_write_buffer_view_desc(device, entry->type, set, heap_offset, alt_heap_offset, bview);
         }
         break;

      default:
         UNREACHABLE("invalid descriptor type");
      }
   }
}
