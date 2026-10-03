#include "stdafx.h"
#include "Emu/NP/upnp_handler.h"
LOG_CHANNEL(upnp_log, "UPNP");
upnp_handler::~upnp_handler() = default;
void upnp_handler::upnp_enable()
{ upnp_log.warning("UPnP is unavailable in the UWP profile; no port mapping attempted"); }
bool upnp_handler::is_active() const { return false; }
void upnp_handler::add_port_redir(const std::string&, u16, std::string_view)
{ upnp_log.warning("UPnP port mapping unsupported in UWP"); }
void upnp_handler::remove_port_redir(u16, std::string_view) {}
void upnp_handler::remove_port_redir_external(u16, std::string_view, bool) {}
