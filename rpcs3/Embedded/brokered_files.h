#pragma once
#include "core_api.h"
namespace rpcs3::embedded
{
int32_t mount_storage(uint32_t kind, void* abi, const char* name, uint32_t writable,
    char* root, uint32_t capacity, uint32_t* required);
int32_t unmount_storage(const char* name);
void clear_storage_mounts();
}
