#pragma once
#include "util/video_source.h"
#include <memory>

// Title preview audio: RPCS3 brokered AVIO -> FFmpeg -> XAudio2, without Qt.
std::unique_ptr<video_source> make_uwp_audio_source();
