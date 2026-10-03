#include "stdafx.h"
#include "GSFrameBase.h"
#include "Emu/system_config.h"

atomic_t<bool> g_game_window_focused = false;

bool is_input_allowed()
{
#ifdef RPCS3_UWP
	extern bool rpcs3_embedded_is_input_allowed();
	return rpcs3_embedded_is_input_allowed();
#else
	return g_game_window_focused || g_cfg.io.background_input_enabled;
#endif
}
