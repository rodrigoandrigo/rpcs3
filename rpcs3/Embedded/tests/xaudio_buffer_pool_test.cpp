#include "../../Emu/Audio/XAudio2/XAudio2BufferPool.h"
#include <algorithm>
#include <cstdio>

int main()
{
    // A delayed callback loses a quantum with just-in-time submission. One
    // real PCM quantum of lookahead must survive the same schedule intact.
    for (bool lookahead : {false, true}) {
        unsigned queued = 0, lost = 0;
        for (unsigned pass = 0; pass < 100; ++pass) {
            const unsigned need = queued < 480 ? 480 - queued : 0;
            if (need && pass % 10 != 5) {
                queued += lookahead ? unsigned(XAudio2BufferPool::request_bytes(need * 8, 9600, 8, 48000) / 8) : need;
            }
            if (queued < 480) lost += 480 - queued;
            queued -= std::min(queued, 480u);
        }
        if (lookahead ? lost != 0 : lost == 0) return 8;
    }
    if (XAudio2BufferPool::request_bytes(9000, 9600, 8, 48000) != 9600 ||
        XAudio2BufferPool::request_bytes(3840, 9600, 8, 48000) != 7680 ||
        XAudio2BufferPool::request_bytes(3, 15, 4, 44100) != 12) return 9;
    XAudio2BufferPool pool;
    pool.initialize(256);
    auto* playing = pool.acquire();
    if (!playing) return 1;
    std::fill(playing->data.begin(), playing->data.end(), 0x55);
    // A partially consumed buffer must not prevent filling the next quantum.
    auto* next = pool.acquire();
    if (!next || next == playing) return 2;
    std::fill(next->data.begin(), next->data.end(), 0xaa);
    if (!std::all_of(playing->data.begin(), playing->data.end(), [](auto v) { return v == 0x55; })) return 3;
    auto* third = pool.acquire();
    if (!third || third == playing || third == next || pool.acquire()) return 4;
    XAudio2BufferPool::release(next);
    if (pool.acquire() != next) return 5;
    XAudio2BufferPool::release(playing); XAudio2BufferPool::release(next); XAudio2BufferPool::release(third);
    XAudio2BufferPool::release(nullptr);
    for (unsigned i = 0; i < 10000; ++i) {
        auto* buffer = pool.acquire();
        if (!buffer || buffer->data.size() != 256) return 6;
        XAudio2BufferPool::release(buffer);
    }
    pool.initialize(512); // Simulates destroy/reopen with a new PCM format.
    if (pool.acquire()->data.size() != 512) return 7;
    std::puts("PASS: delayed quantum regression, aligned lookahead, ownership and reopen");
}
