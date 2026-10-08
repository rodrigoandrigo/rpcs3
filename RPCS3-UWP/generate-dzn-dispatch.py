"""Generate a single-instance embedding dispatch shim from Mesa's Vulkan registry.

No system Vulkan loader, registry ICD discovery, or desktop DLL search is used.
The embedded RPCS3 core owns one Vulkan instance and one logical device.
"""
import pathlib
import sys
import xml.etree.ElementTree as ET

registry = ET.parse(sys.argv[1]).getroot()
commands = {c.findtext('proto/name') or c.get('name'): c for c in registry.find('commands')}
names = set()
for feature in registry.findall('feature'):
    if 'vulkan' in feature.get('api', '').split(','):
        names.update(c.get('name') for c in feature.findall('require/command'))
for extension in registry.findall('extensions/extension'):
    if 'vulkan' in extension.get('supported', '').split(',') and not extension.get('platform') and not extension.get('provisional'):
        names.update(c.get('name') for c in extension.findall('require/command'))
out = ['#include <vulkan/vulkan.h>', '#include <windows.h>', '#include <atomic>', '#include <stdexcept>', '#include <string>',
       '#ifndef RPCS3_DZN_NATIVE_TEST',
       'extern "C" void rpcs3_embedded_dzn_log(void*, const char*);',
       '#endif',
       'namespace { std::atomic<VkInstance> embedded_instance{}; std::atomic<VkDevice> embedded_device{};',
       'PFN_vkGetInstanceProcAddr icd() { static auto proc = [] {',
       '#ifdef RPCS3_DZN_NATIVE_TEST',
       'auto module = LoadLibraryExW(L"vulkan_dzn.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);',
       '#else',
       'auto module = LoadPackagedLibrary(L"vulkan_dzn.dll", 0);',
       '#endif',
       'if (!module) throw std::runtime_error("Cannot load packaged Mesa DZN: Win32 " + std::to_string(GetLastError()));',
       '#ifndef RPCS3_DZN_NATIVE_TEST',
       'using log_hook = void (*)(void (*)(void*, const char*), void*);',
       'auto hook = reinterpret_cast<log_hook>(GetProcAddress(module, "mesa_uwp_set_log_callback"));',
       'if (!hook) throw std::runtime_error("Mesa DZN has no UWP logging bridge");',
       'hook(rpcs3_embedded_dzn_log, nullptr);',
       '#endif',
       'auto p = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vk_icdGetInstanceProcAddr"));',
       'if (!p) throw std::runtime_error("Mesa DZN has no ICD entry point"); return p; }(); return proc; }',
       'PFN_vkVoidFunction resolve(const char* name, bool local) {',
       'auto p = local ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(icd()(embedded_instance.load(), "vkGetDeviceProcAddr"))(embedded_device.load(), name) : icd()(embedded_instance.load(), name);',
       'if (!p) throw std::runtime_error(name); return p; } }']
for name in sorted(names):
    command = commands[name]
    while command.get('alias'):
        command = commands[command.get('alias')]
    result = command.findtext('proto/type')
    params = [p for p in command.findall('param') if 'vulkan' in p.get('api', 'vulkan').split(',')]
    declaration = ', '.join(''.join(p.itertext()) for p in params)
    args = ', '.join(p.findtext('name') for p in params)
    first = params[0].findtext('type') if params else ''
    local = first in ('VkDevice', 'VkQueue', 'VkCommandBuffer')
    out.append(f'extern "C" VKAPI_ATTR {result} VKAPI_CALL {name}({declaration}) {{')
    if name == 'vkCreateInstance':
        out.append('if (embedded_instance.load()) return VK_ERROR_TOO_MANY_OBJECTS;')
    if name == 'vkCreateDevice':
        out.append('if (embedded_device.load()) return VK_ERROR_TOO_MANY_OBJECTS;')
    if name == 'vkGetInstanceProcAddr':
        out.append(f'return icd()({args});')
    else:
        out.append(f'static auto fn = reinterpret_cast<PFN_{name}>(resolve("{name}", {str(local).lower()}));')
        prefix = 'auto status = ' if name in ('vkCreateInstance', 'vkCreateDevice') else ('return ' if result != 'void' else '')
        out.append(f'{prefix}fn({args});')
        if name == 'vkCreateInstance':
            out.append('if (status == VK_SUCCESS) embedded_instance.store(*pInstance); return status;')
        elif name == 'vkCreateDevice':
            out.append('if (status == VK_SUCCESS) embedded_device.store(*pDevice); return status;')
        elif name == 'vkDestroyInstance':
            out.append('embedded_instance.store(VK_NULL_HANDLE);')
        elif name == 'vkDestroyDevice':
            out.append('embedded_device.store(VK_NULL_HANDLE);')
    out.append('}')
pathlib.Path(sys.argv[2]).write_text('\n'.join(out), encoding='utf-8')
