#pragma once

#include <unknwn.h>

extern "C" void* rpcs3_frontend_create(IUnknown* swapChainPanel,
	uint32_t pixelWidth, uint32_t pixelHeight, float rasterizationScale);
extern "C" void rpcs3_frontend_run(void* runtime);
extern "C" void rpcs3_frontend_stop(void* runtime);
extern "C" void rpcs3_frontend_destroy(void* runtime);
