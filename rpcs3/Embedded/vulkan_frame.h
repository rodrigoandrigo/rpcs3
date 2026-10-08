#pragma once
#include "Emu/RSX/GSFrameBase.h"
#include <memory>
std::unique_ptr<GSFrameBase> make_vulkan_frame();
void embedded_publish_bgra(const void* pixels, u32 width, u32 height);
