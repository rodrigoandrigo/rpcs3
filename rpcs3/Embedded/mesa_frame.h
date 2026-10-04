#pragma once
#include "Emu/RSX/GSFrameBase.h"
#include <memory>
std::unique_ptr<GSFrameBase> make_mesa_frame();
