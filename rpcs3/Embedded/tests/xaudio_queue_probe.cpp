// Native silent XAudio2 regression; not an AppContainer, guest or Xbox test.
#include "../../Emu/Audio/XAudio2/XAudio2BufferPool.h"
#include <windows.h>
#include <xaudio2.h>
#include <wrl/client.h>
#include <cstdio>
#include <algorithm>

class callback final : public IXAudio2VoiceCallback
{
public:
    XAudio2BufferPool pool;
    IXAudio2SourceVoice* voice{};
    std::atomic<unsigned> passes{}, partial{}, exhausted{}, errors{}, completed{};
    callback() { pool.initialize(9600); }
    void OnVoiceProcessingPassStart(UINT32 needed) noexcept override
    {
        if (!needed || !voice) return;
        ++passes;
        XAUDIO2_VOICE_STATE state{}; voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued) ++partial;
        auto* slot = pool.acquire();
        if (!slot) { ++exhausted; return; }
        needed = static_cast<unsigned>(XAudio2BufferPool::request_bytes(needed, slot->data.size(), 8, 44100));
        needed -= needed % 8;
        std::fill(slot->data.begin(), slot->data.begin() + needed, 0);
        XAUDIO2_BUFFER buffer{}; buffer.AudioBytes = needed;
        buffer.pAudioData = slot->data.data(); buffer.pContext = slot;
        if (FAILED(voice->SubmitSourceBuffer(&buffer))) { ++errors; pool.release(slot); }
    }
    void OnBufferEnd(void* context) noexcept override { pool.release(context); ++completed; }
    void OnVoiceError(void*, HRESULT) noexcept override { ++errors; }
    void OnVoiceProcessingPassEnd() noexcept override {}
    void OnStreamEnd() noexcept override {}
    void OnBufferStart(void*) noexcept override {}
    void OnLoopEnd(void*) noexcept override {}
};

int main()
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Microsoft::WRL::ComPtr<IXAudio2> engine;
    HRESULT hr = XAudio2Create(&engine);
    if (FAILED(hr)) { std::printf("XAudio2Create HRESULT=%08x\n", unsigned(hr)); return 1; }
    IXAudio2MasteringVoice* master{};
    hr = engine->CreateMasteringVoice(&master, 2, 48000, 0, nullptr, nullptr, AudioCategory_GameEffects);
    if (FAILED(hr)) { std::printf("Master HRESULT=%08x\n", unsigned(hr)); return 2; }
    master->SetVolume(0); // Never play sound while running this regression.
    callback cb;
    WAVEFORMATEX pcm{};
    pcm.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; pcm.nChannels = 2; pcm.nSamplesPerSec = 44100;
    pcm.wBitsPerSample = 32; pcm.nBlockAlign = 8; pcm.nAvgBytesPerSec = pcm.nSamplesPerSec * 8;
    hr = engine->CreateSourceVoice(&cb.voice, &pcm, 0, 2, &cb);
    if (FAILED(hr)) { master->DestroyVoice(); std::printf("Source HRESULT=%08x\n", unsigned(hr)); return 3; }
    cb.voice->SetFrequencyRatio(1.001f);
    // Seed a partial frame queue: the next processing pass must fill its
    // deficit even though an earlier buffer is still owned by XAudio2.
    auto* seed = cb.pool.acquire();
    XAUDIO2_BUFFER initial{};
    initial.AudioBytes = 8; initial.pAudioData = seed->data.data(); initial.pContext = seed;
    hr = cb.voice->SubmitSourceBuffer(&initial);
    if (FAILED(hr)) { cb.pool.release(seed); cb.voice->DestroyVoice(); master->DestroyVoice(); return 7; }
    hr = cb.voice->Start();
    if (FAILED(hr)) { cb.voice->DestroyVoice(); master->DestroyVoice(); return 4; }
    for (int restart = 0; restart < 3; ++restart) {
        Sleep(800);
        cb.voice->Stop();
        cb.voice->FlushSourceBuffers();
        XAUDIO2_VOICE_STATE stopped{};
        for (unsigned attempt = 0; attempt < 50; ++attempt) {
            cb.voice->GetState(&stopped);
            if (!stopped.BuffersQueued) break;
            Sleep(1);
        }
        if (stopped.BuffersQueued) { std::puts("FAIL: stopped voice retained PCM buffers"); return 8; }
        cb.voice->Start();
    }
    Sleep(200);
    XAUDIO2_VOICE_STATE state{}; cb.voice->GetState(&state);
    cb.voice->Stop(); cb.voice->DestroyVoice(); cb.voice = nullptr;
    master->DestroyVoice(); engine->StopEngine(); engine.Reset();
    if (SUCCEEDED(com)) CoUninitialize();
    std::printf("passes=%u deficit_while_queued=%u completed=%u exhausted=%u errors=%u samples=%llu\n",
        cb.passes.load(), cb.partial.load(), cb.completed.load(), cb.exhausted.load(), cb.errors.load(), state.SamplesPlayed);
    if (cb.errors || cb.exhausted || cb.passes < 30 || state.SamplesPlayed < 50000) return 5;
    if (!cb.partial) { std::puts("INCONCLUSIVE: endpoint did not exercise partial-buffer deficits"); return 6; }
    std::puts("PASS: XAudio2 partial-buffer deficits filled without starvation or premature reuse");
}
