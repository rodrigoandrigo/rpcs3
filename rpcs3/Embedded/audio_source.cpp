#include "stdafx.h"
#include "audio_source.h"
#include "util/media_utils.h"
#include "Emu/Audio/XAudio2/XAudio2Backend.h"
#include "Emu/system_config.h"
#include "Loader/ISO.h"
#include <atomic>
#include <shared_mutex>

namespace
{
class audio_source final : public video_source
{
    utils::audio_decoder m_decoder;
    std::unique_ptr<XAudio2Backend> m_output;
    std::string m_audio_path, m_iso_path, m_mount;
    bool m_archive = false;
    std::atomic<bool> m_active{false};
    usz m_cursor = 0;
public:
    ~audio_source() override { set_active(false); }
    void set_iso_path(const std::string& path) override { m_iso_path = path; }
    void set_video_path(const std::string&, bool) override {}
    void set_audio_path(const std::string& path, bool archive) override { m_audio_path = path; m_archive = archive; }
    bool get_active() const override { return m_active.load(); }
    bool has_new() const override { return false; }
    void get_image(std::vector<u8>&, int&, int&, int&, int&) override {}
    void set_active(bool active) override
    {
        if (!active)
        {
            m_active = false;
            if (m_output) { m_output->Pause(); m_output->SetWriteCallback({}); m_output.reset(); }
            m_decoder.stop();
            if (!m_mount.empty()) { fs::set_virtual_device(m_mount, {}); m_mount.clear(); }
            m_cursor = 0;
            return;
        }
        if (m_active || m_audio_path.empty()) return;
        auto path = m_audio_path;
        if (m_archive)
        {
            if (m_iso_path.empty()) { rsx_log.error("Title audio requires its ISO path"); return; }
            static std::atomic<u64> next{0};
            const auto id = std::to_string(++next);
            m_mount = "uwp_title_audio_" + id;
            // Virtual device paths require a 22-character ID followed by '_'.
            const auto prefix = "/vfsv0_" + std::string(22 - id.size(), '0') + id + "_" + m_mount;
            fs::set_virtual_device(m_mount, stx::make_shared<iso_device>(m_iso_path, prefix));
            path = prefix + (path.starts_with('/') ? "" : "/") + path;
        }
        m_output = std::make_unique<XAudio2Backend>();
        if (!m_output->Initialized() || !m_output->Open({}, AudioFreq::FREQ_48K,
            AudioSampleSize::FLOAT, AudioChannelCnt::STEREO, audio_channel_layout::stereo))
        {
            rsx_log.error("Unable to open XAudio2 output for title audio");
            set_active(false);
            return;
        }
        music_selection_context context;
        context.valid = true;
        context.playlist = {path};
        m_decoder.set_context(std::move(context));
        m_decoder.decode(); // Decoding runs on its own worker, not the UI/RSX thread.
        m_output->SetWriteCallback([this](u32 requested, void* buffer) -> u32
        {
            std::shared_lock lock(m_decoder.m_mtx, std::try_to_lock);
            if (!lock.owns_lock() || m_decoder.has_error) return 0;
            const usz available = static_cast<usz>(m_decoder.m_size.load());
            const usz frame_bytes = sizeof(float) * 2;
            requested -= requested % frame_bytes;
            usz written = 0;
            auto destination = static_cast<u8*>(buffer);
            while (written < requested && available)
            {
                if (m_cursor >= available)
                {
                    if (!m_decoder.track_fully_decoded) break;
                    m_cursor = 0; // SND0 title previews repeat until deactivated.
                }
                usz count = std::min<usz>(requested - written, available - m_cursor);
                count -= count % frame_bytes;
                if (!count) break;
                AudioBackend::apply_volume_static(g_cfg.audio.volume.get() / 100.f,
                    static_cast<u32>(count / sizeof(float)),
                    reinterpret_cast<const float*>(m_decoder.data.data() + m_cursor),
                    reinterpret_cast<float*>(destination + written));
                m_cursor += count;
                written += count;
            }
            return static_cast<u32>(written);
        });
        m_active = true;
        m_output->Play();
    }
};
}
std::unique_ptr<video_source> make_uwp_audio_source() { return std::make_unique<audio_source>(); }
