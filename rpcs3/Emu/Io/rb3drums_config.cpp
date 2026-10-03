#include "stdafx.h"
#include "rb3drums_config.h"

LOG_CHANNEL(cfg_log, "CFG");

cfg_rb3drums g_cfg_rb3drums;

cfg_rb3drums::cfg_rb3drums()
	: cfg::node()
{
}

std::string cfg_rb3drums::path() const
{
	return fs::get_config_dir(true) + "rb3drums.yml";
}

bool cfg_rb3drums::load()
{
	const std::string config_path = path();
	cfg_log.notice("Loading rb3drums config from '%s'", config_path);

	if (fs::file cfg_file{config_path, fs::read})
	{
		return from_string(cfg_file.to_string());
	}

	cfg_log.notice("No rb3drums config found. Using default settings. Path: %s", config_path);
	from_default();
	save();
	return false;
}

void cfg_rb3drums::save()
{
	const std::string config_path = path();
	cfg_log.notice("Saving rb3drums config to '%s'", config_path);

	if (!cfg::node::save(config_path))
	{
		cfg_log.error("Failed to save rb3drums config to '%s' (error=%s)", config_path, fs::g_tls_error);
	}

	reload_requested = true;
}
