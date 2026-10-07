// Standalone native-PC component probe. Not installed or linked into RPCS3.
// SPDX-License-Identifier: MIT
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <vulkan/vulkan.h>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <cstdio>

static LONG CALLBACK report_fault(EXCEPTION_POINTERS* fault)
{
   if (fault->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
   void* frames[48]{};
   unsigned count=CaptureStackBackTrace(0, 48, frames, nullptr);
   for (unsigned i=0; i<count; ++i) {
      HMODULE module{}; wchar_t path[MAX_PATH]{};
      if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(frames[i]), &module)) {
         GetModuleFileNameW(module, path, MAX_PATH);
         std::fprintf(stderr, "STACK %ls + 0x%llx\n", path,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i])-reinterpret_cast<uintptr_t>(module)));
      }
   }
   return EXCEPTION_CONTINUE_SEARCH;
}

static void check(VkResult result, const char* operation)
{
   std::cout << operation << ": " << result << '\n';
   if (result != VK_SUCCESS)
      throw std::runtime_error(std::string(operation) + " VkResult=" + std::to_string(result));
}

#include "dozen_seven_smoke.h"

int wmain(int argc, wchar_t** argv)
{
   try {
      std::cout << std::unitbuf;
      if (argc>=5 && !std::wcscmp(argv[1], L"--debug")) {
         // Observe only this isolated probe's D3D12 debug messages. No app
         // installation, loader registration or unrelated process attachment.
         std::wstring command=L"\"" + std::wstring(argv[0]) + L"\"";
         for (int i=2; i<argc; ++i) command+=L" \"" + std::wstring(argv[i]) + L"\"";
         STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION process{};
         if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                             DEBUG_ONLY_THIS_PROCESS | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
            throw std::runtime_error("Debug probe launch Win32=" + std::to_string(GetLastError()));
         DEBUG_EVENT event{}; DWORD code=1;
         while (WaitForDebugEvent(&event, INFINITE)) {
            DWORD action=DBG_CONTINUE;
            if (event.dwDebugEventCode==OUTPUT_DEBUG_STRING_EVENT) {
               auto& info=event.u.DebugString;
               size_t size=info.nDebugStringLength * (info.fUnicode ? sizeof(wchar_t) : 1);
               std::vector<char> message(size+sizeof(wchar_t), 0);
               SIZE_T read{};
               if (ReadProcessMemory(process.hProcess, info.lpDebugStringData, message.data(), size, &read)) {
                  if (info.fUnicode) std::fwprintf(stderr, L"D3D12 debug: %ls\n", reinterpret_cast<const wchar_t*>(message.data()));
                  else std::fprintf(stderr, "D3D12 debug: %s\n", message.data());
               }
            } else if (event.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT && event.u.CreateProcessInfo.hFile) {
               CloseHandle(event.u.CreateProcessInfo.hFile);
            } else if (event.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile) {
               CloseHandle(event.u.LoadDll.hFile);
            } else if (event.dwDebugEventCode==EXCEPTION_DEBUG_EVENT && event.u.Exception.ExceptionRecord.ExceptionCode!=EXCEPTION_BREAKPOINT) {
               action=DBG_EXCEPTION_NOT_HANDLED;
            } else if (event.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT) {
               code=event.u.ExitProcess.dwExitCode;
               ContinueDebugEvent(event.dwProcessId, event.dwThreadId, action); break;
            }
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, action);
         }
         CloseHandle(process.hThread); CloseHandle(process.hProcess);
         return static_cast<int>(code);
      }
      bool bounded=argc==5 && !std::wcscmp(argv[1], L"--bounded");
      bool seven=argc==5 && !std::wcscmp(argv[1], L"--seven");
      if (bounded) {++argv; --argc;}
      if (seven) {++argv; --argc;}
      AddVectoredExceptionHandler(1, report_fault);
      if (argc != 4) throw std::runtime_error("Usage: dozen_port_smoke.exe <absolute vulkan_dzn.dll> <SDK dxil.dll> <compute.spv>");
      // This uninstalled PC probe has no package identity. Preload the trusted
      // SDK validator so UWP's existing-module path can use it unchanged.
      HMODULE validator = LoadLibraryExW(argv[2], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
      if (!validator) throw std::runtime_error("DXIL preload Win32=" + std::to_string(GetLastError()));
      HMODULE d3d = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!d3d) throw std::runtime_error("D3D12 preload Win32=" + std::to_string(GetLastError()));
      auto debug_get=reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(d3d, "D3D12GetDebugInterface"));
      ID3D12Debug* debug{};
      if (debug_get && SUCCEEDED(debug_get(__uuidof(ID3D12Debug), reinterpret_cast<void**>(&debug)))) {
         debug->EnableDebugLayer(); debug->Release();
      } else std::cout << "SKIPPED: native D3D12 debug layer unavailable\n";
      auto get_interface=reinterpret_cast<PFN_D3D12_GET_INTERFACE>(GetProcAddress(d3d, "D3D12GetInterface"));
      ID3D12DeviceFactory* factory{};
      if (get_interface && SUCCEEDED(get_interface(CLSID_D3D12DeviceFactory, __uuidof(ID3D12DeviceFactory), reinterpret_cast<void**>(&factory)))) {
         debug=nullptr;
         if (SUCCEEDED(factory->GetConfigurationInterface(CLSID_D3D12Debug, __uuidof(ID3D12Debug), reinterpret_cast<void**>(&debug)))) {
            debug->EnableDebugLayer(); debug->Release();
         }
         factory->Release();
      }
      HMODULE module = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
      if (!module) throw std::runtime_error("LoadLibraryExW Win32=" + std::to_string(GetLastError()));
      auto logger = reinterpret_cast<void (*)(void (*)(void*, const char*), void*)>(GetProcAddress(module, "mesa_uwp_set_log_callback"));
      std::cout << "UWP log callback available=" << !!logger << '\n';
      if (logger) logger([](void*, const char* message){std::fprintf(stderr, "%s", message);}, nullptr);
      auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vk_icdGetInstanceProcAddr"));
      if (!get) throw std::runtime_error("ICD entrypoint missing");
#define INSTANCE_FN(name) auto name = reinterpret_cast<PFN_##name>(get(instance, #name)); if (!name) throw std::runtime_error(#name " missing")
      VkInstance instance{};
      auto create = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
      if (!create) throw std::runtime_error("vkCreateInstance missing");
      VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
      app.pApplicationName = "Dozen port component probe"; app.apiVersion = VK_API_VERSION_1_2;
      VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo = &app;
      const char* surface_extensions[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
      if (seven) {ci.enabledExtensionCount=2; ci.ppEnabledExtensionNames=surface_extensions;}
      check(create(&ci, nullptr, &instance), "create instance");
      INSTANCE_FN(vkEnumeratePhysicalDevices);
      INSTANCE_FN(vkGetPhysicalDeviceProperties);
      INSTANCE_FN(vkEnumerateDeviceExtensionProperties);
      INSTANCE_FN(vkGetPhysicalDeviceFeatures2);
      INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties);
      INSTANCE_FN(vkGetPhysicalDeviceMemoryProperties);
      INSTANCE_FN(vkCreateDevice);
      INSTANCE_FN(vkGetDeviceProcAddr);
      INSTANCE_FN(vkDestroyInstance);
      uint32_t count{}; check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "adapter count");
      if (!count) throw std::runtime_error("No D3D12 adapter enumerated");
      std::vector<VkPhysicalDevice> adapters(count);
      check(vkEnumeratePhysicalDevices(instance, &count, adapters.data()), "adapters");
      auto physical = adapters.front();
      VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical, &properties);
      std::cout << "Adapter: " << properties.deviceName << '\n';
      count=0; check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr), "extension count");
      std::vector<VkExtensionProperties> extensions(count);
      check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()), "extensions");
      std::vector<const char*> names;
      bool calibrated=false;
      bool extended1=false, conservative=false;
      for (const auto& extension : extensions) {
         names.push_back(extension.extensionName);
         std::cout << extension.extensionName << '\n';
         calibrated |= !std::strcmp(extension.extensionName, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
         extended1 |= !std::strcmp(extension.extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
         conservative |= !std::strcmp(extension.extensionName, VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
      }
      VkPhysicalDeviceRobustness2FeaturesEXT robustness{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
      VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR bary{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
      VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      features.pNext=&robustness; robustness.pNext=&bary;
      vkGetPhysicalDeviceFeatures2(physical, &features);
      std::cout << "robustBufferAccess2=" << robustness.robustBufferAccess2
                << " robustImageAccess2=" << robustness.robustImageAccess2
                << " nullDescriptor=" << robustness.nullDescriptor
                << " barycentrics=" << bary.fragmentShaderBarycentric << '\n';
      if (robustness.robustImageAccess2 || robustness.nullDescriptor)
         throw std::runtime_error("Partial robustness2 incorrectly claims unsupported features");
      count=0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
      std::vector<VkQueueFamilyProperties> families(count);
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
      uint32_t family=0;
      while (family<count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
      if (family==count) throw std::runtime_error("No graphics queue");
      float priority=1;
      VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      qci.queueFamilyIndex=family; qci.queueCount=1; qci.pQueuePriorities=&priority;
      VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
      dci.enabledExtensionCount=static_cast<uint32_t>(names.size()); dci.ppEnabledExtensionNames=names.data();
      VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures zero{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES};
      zero.shaderZeroInitializeWorkgroupMemory=VK_TRUE; dci.pNext=&zero;
      VkPhysicalDeviceExtendedDynamicStateFeaturesEXT dynamic1{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
      VkPhysicalDeviceExtendedDynamicState2FeaturesEXT dynamic2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
      VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertex_input{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
      VkPhysicalDevice4444FormatsFeaturesEXT formats4444{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT};
      VkPhysicalDevicePresentIdFeaturesKHR present_id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
      VkPhysicalDeviceFeatures graphics_features{};
      if (seven) {
         VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; query.pNext=&formats4444;
         vkGetPhysicalDeviceFeatures2(physical,&query);
         graphics_features.fillModeNonSolid=VK_TRUE;
         graphics_features.geometryShader=VK_TRUE;
         dci.pEnabledFeatures=&graphics_features;
         dynamic1.extendedDynamicState=extended1;
         dynamic2.extendedDynamicState2=VK_TRUE; vertex_input.vertexInputDynamicState=VK_TRUE;
         zero.pNext=&dynamic1; dynamic1.pNext=&dynamic2; dynamic2.pNext=&vertex_input;
         vertex_input.pNext=&formats4444; formats4444.pNext=&present_id; present_id.presentId=VK_TRUE;
      }
      if (bounded) {
         if (!robustness.robustBufferAccess2) throw std::runtime_error("Bounded test requires robustBufferAccess2");
         robustness.pNext=nullptr; zero.pNext=&robustness;
      }
      std::cout << "Descriptor path: " << (bounded ? "bounded robustness2" : "bindless") << '\n';
      VkDevice device{}; check(vkCreateDevice(physical, &dci, nullptr, &device), "device/all advertised extensions");
#define DEVICE_FN(name) auto name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name)); if (!name) throw std::runtime_error(#name " missing")
      DEVICE_FN(vkGetDeviceQueue); DEVICE_FN(vkCreateBuffer); DEVICE_FN(vkGetBufferMemoryRequirements);
      DEVICE_FN(vkAllocateMemory); DEVICE_FN(vkBindBufferMemory); DEVICE_FN(vkCreateCommandPool);
      DEVICE_FN(vkAllocateCommandBuffers); DEVICE_FN(vkBeginCommandBuffer); DEVICE_FN(vkCmdFillBuffer);
      DEVICE_FN(vkCmdPipelineBarrier);
      DEVICE_FN(vkEndCommandBuffer); DEVICE_FN(vkQueueSubmit); DEVICE_FN(vkQueueWaitIdle);
      DEVICE_FN(vkMapMemory); DEVICE_FN(vkInvalidateMappedMemoryRanges); DEVICE_FN(vkUnmapMemory);
      DEVICE_FN(vkDestroyBuffer); DEVICE_FN(vkFreeMemory); DEVICE_FN(vkDestroyCommandPool); DEVICE_FN(vkDestroyDevice);
      DEVICE_FN(vkCmdPushDescriptorSetKHR); DEVICE_FN(vkCmdPushDescriptorSetWithTemplateKHR);
      DEVICE_FN(vkCmdDrawMultiEXT); DEVICE_FN(vkResetQueryPoolEXT);
      DEVICE_FN(vkCreateShaderModule); DEVICE_FN(vkCreateDescriptorSetLayout); DEVICE_FN(vkCreatePipelineLayout);
      DEVICE_FN(vkCreateComputePipelines); DEVICE_FN(vkCmdBindPipeline); DEVICE_FN(vkCmdDispatch);
      DEVICE_FN(vkDestroyPipeline); DEVICE_FN(vkDestroyPipelineLayout); DEVICE_FN(vkDestroyDescriptorSetLayout); DEVICE_FN(vkDestroyShaderModule);
      VkQueue queue{}; vkGetDeviceQueue(device, family, 0, &queue);
      if (seven)
         dozen_seven_smoke(instance, physical, device, queue, family, get, vkGetDeviceProcAddr,
                           std::filesystem::path(argv[3]).parent_path(), extended1, conservative, formats4444.formatA4B4G4R4);
      if (seven)
         dozen_seven_wsi_smoke(instance, physical, device, queue, family, get, vkGetDeviceProcAddr, module);
      VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      bci.size=4096; bci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      VkBuffer buffer{}; check(vkCreateBuffer(device, &bci, nullptr, &buffer), "buffer");
      VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device, buffer, &requirements);
      VkPhysicalDeviceMemoryProperties memory{}; vkGetPhysicalDeviceMemoryProperties(physical, &memory);
      uint32_t type=0;
      while (type<memory.memoryTypeCount && (!(requirements.memoryTypeBits & (1u<<type)) ||
             !(memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) ++type;
      if (type==memory.memoryTypeCount) throw std::runtime_error("No host-visible buffer memory");
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      ai.allocationSize=requirements.size; ai.memoryTypeIndex=type;
      VkDeviceMemory allocation{}; check(vkAllocateMemory(device, &ai, nullptr, &allocation), "allocate");
      check(vkBindBufferMemory(device, buffer, allocation, 0), "bind");
      std::ifstream input(std::filesystem::path(argv[3]), std::ios::binary | std::ios::ate);
      auto bytes=input.tellg();
      if (!input || bytes<=0 || bytes%4) throw std::runtime_error("Invalid compute SPIR-V file");
      std::vector<uint32_t> code(static_cast<size_t>(bytes)/4); input.seekg(0);
      if (!input.read(reinterpret_cast<char*>(code.data()), bytes)) throw std::runtime_error("SPIR-V read failed");
      VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sci.codeSize=static_cast<size_t>(bytes); sci.pCode=code.data();
      VkShaderModule shader{}; check(vkCreateShaderModule(device, &sci, nullptr, &shader), "shader");
      VkDescriptorSetLayoutBinding binding{}; binding.binding=0; binding.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      binding.descriptorCount=1; binding.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
      VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      lci.flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR; lci.bindingCount=1; lci.pBindings=&binding;
      VkDescriptorSetLayout layout{}; check(vkCreateDescriptorSetLayout(device, &lci, nullptr, &layout), "push layout");
      VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; plci.setLayoutCount=1; plci.pSetLayouts=&layout;
      VkPipelineLayout pipeline_layout{}; check(vkCreatePipelineLayout(device, &plci, nullptr, &pipeline_layout), "pipeline layout");
      VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      cpi.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cpi.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
      cpi.stage.module=shader; cpi.stage.pName="main"; cpi.layout=pipeline_layout;
      VkPipeline pipeline{}; check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline), "SPIR-V to DXIL/compute pipeline");
      VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex=family;
      VkCommandPool pool{}; check(vkCreateCommandPool(device, &pci, nullptr, &pool), "pool");
      VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      cai.commandPool=pool; cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount=1;
      VkCommandBuffer commands{}; check(vkAllocateCommandBuffers(device, &cai, &commands), "command buffer");
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      check(vkBeginCommandBuffer(commands, &begin), "begin");
      vkCmdFillBuffer(commands, buffer, 0, 4096, 0x137fabcd);
      VkBufferMemoryBarrier compute{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      compute.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; compute.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
      compute.srcQueueFamilyIndex=compute.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      compute.buffer=buffer; compute.size=VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 0, nullptr, 1, &compute, 0, nullptr);
      vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      // In bounded mode, half of the invocations write beyond the descriptor
      // range but remain inside the allocation. They must preserve the sentinel.
      VkDescriptorBufferInfo dbi{buffer, 0, bounded ? 2048u : 4096u};
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstBinding=0;
      write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.descriptorCount=1; write.pBufferInfo=&dbi;
      vkCmdPushDescriptorSetKHR(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &write);
      std::cout << "push descriptor recorded\n";
      vkCmdDispatch(commands, 32, 1, 1);
      std::cout << "dispatch recorded\n";
      VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
      host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      host.buffer=buffer; host.size=VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                           0, 0, nullptr, 1, &host, 0, nullptr);
      check(vkEndCommandBuffer(commands), "end");
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&commands;
      check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "submit"); check(vkQueueWaitIdle(queue), "wait");
      void* mapped{}; check(vkMapMemory(device, allocation, 0, VK_WHOLE_SIZE, 0, &mapped), "map");
      VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory=allocation; range.size=VK_WHOLE_SIZE;
      check(vkInvalidateMappedMemoryRanges(device, 1, &range), "invalidate");
      for (unsigned i=0; i<1024; ++i)
         if (static_cast<const uint32_t*>(mapped)[i]!=(bounded && i>=512 ? 0x137fabcdu : 0x24680000u+i))
            throw std::runtime_error("Compute/push descriptor/workgroup/bounds mismatch at " + std::to_string(i));
      vkUnmapMemory(device, allocation);
      if (calibrated) {
         DEVICE_FN(vkGetCalibratedTimestampsKHR);
         VkCalibratedTimestampInfoKHR clocks[2]{{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR}, {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR}};
         clocks[0].timeDomain=VK_TIME_DOMAIN_DEVICE_KHR; clocks[1].timeDomain=VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
         uint64_t ticks[2]{}, deviation{};
         check(vkGetCalibratedTimestampsKHR(device, 2, clocks, ticks, &deviation), "calibration");
         if (!ticks[0] || !ticks[1] || !deviation) throw std::runtime_error("Invalid calibration payload");
         std::cout << "Calibration maxDeviation(ns)=" << deviation << '\n';
      } else std::cout << "SKIPPED: calibrated timestamps not supported on this adapter\n";
      vkDestroyCommandPool(device, pool, nullptr); vkDestroyBuffer(device, buffer, nullptr);
      vkDestroyPipeline(device, pipeline, nullptr); vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
      vkDestroyDescriptorSetLayout(device, layout, nullptr); vkDestroyShaderModule(device, shader, nullptr);
      vkFreeMemory(device, allocation, nullptr); vkDestroyDevice(device, nullptr);
      vkDestroyInstance(instance, nullptr); FreeLibrary(module); FreeLibrary(d3d); FreeLibrary(validator);
      std::cout << "PASS: discovery, partial-feature guard, device/all extensions, entrypoints, SPIR-V/DXIL compute, push descriptors, zeroed workgroup/readback; not Vulkan CTS\n";
      return 0;
   } catch (const std::exception& error) {std::cerr << "FAIL: " << error.what() << '\n'; return 1;}
}
