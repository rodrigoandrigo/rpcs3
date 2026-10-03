#include "stdafx.h"
#include "ps_move_config.h"

LOG_CHANNEL(ps_move);

cfg_ps_moves g_cfg_move;

cfg_ps_moves::cfg_ps_moves()
	: cfg::node()
{
}

std::string cfg_ps_moves::path() const
{
	return fs::get_config_dir(true) + "ps_move.yml";
}

bool cfg_ps_moves::load()
{
	const std::string config_path = path();
	ps_move.notice("Loading PS Move config from '%s'", config_path);

	if (fs::file cfg_file{ config_path, fs::read })
	{
		return from_string(cfg_file.to_string());
	}

	ps_move.notice("PS Move config missing. Using default settings. Path: %s", config_path);
	from_default();
	return false;
}

void cfg_ps_moves::save() const
{
	const std::string config_path = path();
	ps_move.notice("Saving PS Move config to '%s'", config_path);

	if (!cfg::node::save(config_path))
	{
		ps_move.error("Failed to save PS Move config to '%s' (error=%s)", config_path, fs::g_tls_error);
	}
}
