#include "stdafx.h"
#include "recording_config.h"

LOG_CHANNEL(cfg_log, "CFG");

cfg_recording g_cfg_recording;

cfg_recording::cfg_recording()
	: cfg::node()
{
}

std::string cfg_recording::path() const
{
	return fs::get_config_dir(true) + "recording.yml";
}

bool cfg_recording::load()
{
	const std::string config_path = path();
	cfg_log.notice("Loading recording config from '%s'", config_path);

	if (fs::file cfg_file{config_path, fs::read})
	{
		return from_string(cfg_file.to_string());
	}

	cfg_log.notice("Recording config missing. Using default settings. Path: %s", config_path);
	from_default();
	save();
	return false;
}

void cfg_recording::save() const
{
	const std::string config_path = path();
	cfg_log.notice("Saving recording config to '%s'", config_path);

	if (!cfg::node::save(config_path))
	{
		cfg_log.error("Failed to save recording config to '%s' (error=%s)", config_path, fs::g_tls_error);
	}
}
