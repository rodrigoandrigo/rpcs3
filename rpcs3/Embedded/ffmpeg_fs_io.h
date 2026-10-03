#pragma once
#include "Utilities/File.h"
#include <cerrno>
#include <limits>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

namespace rpcs3::embedded
{
// AVIO owns neither this object nor the RPCS3 file. Keep it alive until the
// format context has closed. All callbacks contain exceptions at the C boundary.
class ffmpeg_fs_io
{
    fs::file m_file;
    AVIOContext* m_io = nullptr;
    static int read(void* opaque, uint8_t* data, int count) noexcept try
    {
        if (count <= 0) return AVERROR(EINVAL);
        auto& file = static_cast<ffmpeg_fs_io*>(opaque)->m_file;
        const auto bytes = file.read(data, static_cast<u64>(count));
        if (bytes) return static_cast<int>(bytes);
        return file.pos() >= file.size() ? AVERROR_EOF : AVERROR(EIO);
    }
    catch (...) { return AVERROR(EIO); }
    static int write(void* opaque, const uint8_t* data, int count) noexcept try
    {
        if (count <= 0) return AVERROR(EINVAL);
        auto& file = static_cast<ffmpeg_fs_io*>(opaque)->m_file;
        const auto bytes = file.write(data, static_cast<u64>(count));
        return bytes == static_cast<u64>(count) ? count : AVERROR(EIO);
    }
    catch (...) { return AVERROR(EIO); }
    static int64_t seek(void* opaque, int64_t offset, int origin) noexcept try
    {
        auto& file = static_cast<ffmpeg_fs_io*>(opaque)->m_file;
        origin &= ~AVSEEK_FORCE;
        if (origin == AVSEEK_SIZE) {
            const auto size = file.size();
            return size <= static_cast<u64>(INT64_MAX) ? static_cast<int64_t>(size) : AVERROR(EOVERFLOW);
        }
        fs::seek_mode mode;
        switch (origin) {
        case SEEK_SET: mode = fs::seek_set; break;
        case SEEK_CUR: mode = fs::seek_cur; break;
        case SEEK_END: mode = fs::seek_end; break;
        default: return AVERROR(EINVAL);
        }
        const auto result = file.seek(offset, mode);
        return result <= static_cast<u64>(INT64_MAX) ? static_cast<int64_t>(result) : AVERROR(EIO);
    }
    catch (...) { return AVERROR(EIO); }
public:
    ffmpeg_fs_io() = default;
    ffmpeg_fs_io(const ffmpeg_fs_io&) = delete;
    ffmpeg_fs_io& operator=(const ffmpeg_fs_io&) = delete;
    ~ffmpeg_fs_io() {
        if (m_io) { av_freep(&m_io->buffer); avio_context_free(&m_io); }
    }
    bool open(const std::string& path, bool writing) try
    {
        if (m_io || !m_file.open(path, writing ? fs::rewrite : fs::read)) return false;
        auto* buffer = static_cast<uint8_t*>(av_malloc(64 * 1024));
        if (!buffer) return false;
        m_io = avio_alloc_context(buffer, 64 * 1024, writing, this,
            writing ? nullptr : read, writing ? write : nullptr, seek);
        if (!m_io) { av_free(buffer); return false; }
        return true;
    }
    catch (...) { return false; }
    AVIOContext* get() const noexcept { return m_io; }
    int open_input(AVFormatContext*& context, const std::string& path, AVDictionary** options)
    {
        if (!context) return AVERROR(ENOMEM);
        if (!open(path, false)) return AVERROR(EIO);
        context->pb = m_io;
        context->flags |= AVFMT_FLAG_CUSTOM_IO;
        return avformat_open_input(&context, path.c_str(), nullptr, options);
    }
    int flush() noexcept try {
        if (!m_io) return 0;
        avio_flush(m_io);
        if (m_io->error < 0) return m_io->error;
        m_file.sync();
        return 0;
    }
    catch (...) { return AVERROR(EIO); }
};
}
