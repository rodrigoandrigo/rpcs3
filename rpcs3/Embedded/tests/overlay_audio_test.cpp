#include "stdafx.h"
#include "Emu/emu_callbacks.h"
#include "Emu/RSX/Overlays/overlay_audio.h"
#include <iostream>

emu_callbacks g_emu_callbacks;
// This isolated test exercises the real audio_player, not the emulator logger.
logs::registerer::registerer(logs::channel&) {}

class test_source final : public video_source
{
public:
    static inline bool configured = false, active = false;
    void set_iso_path(const std::string&) override {}
    void set_video_path(const std::string&, bool) override {}
    void set_audio_path(const std::string& path, bool archive) override { configured = path == "SND0.AT3" && archive; }
    void set_active(bool value) override { active = value; }
    bool get_active() const override { return active; }
    bool has_new() const override { return false; }
    void get_image(std::vector<u8>&, int&, int&, int&, int&) override {}
};

int main()
{
    using rsx::overlays::audio_player;
    { audio_player player("SND0.AT3", false, ""); player.set_active(true); player.set_active(false); }
    g_emu_callbacks.make_video_source = [] { return std::unique_ptr<video_source>{}; };
    for (int i = 0; i < 10; ++i) { audio_player player("SND0.AT3", false, ""); player.set_active(true); player.set_active(false); }
    g_emu_callbacks.make_video_source = [] { return std::make_unique<test_source>(); };
    audio_player player("SND0.AT3", true, "game.iso");
    player.set_active(true);
    if (!test_source::configured || !test_source::active) return 1;
    player.set_active(false);
    if (test_source::active) return 1;
    std::cout << "PASS missing callback, null decoder, repeated title audio, and supported decoder\n";
}
