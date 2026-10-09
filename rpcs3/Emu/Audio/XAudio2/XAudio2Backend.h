#pragma once

#ifndef _WIN32
#error "XAudio2 can only be built on Windows."
#endif

#include "Emu/Audio/AudioBackend.h"

#include <initguid.h>
#include <xaudio2.h>
#include <wrl/client.h>
#include <atomic>
#ifdef RPCS3_UWP
#include "XAudio2BufferPool.h"
#endif
#ifndef RPCS3_UWP
#include <MMDeviceAPI.h>
#endif

class XAudio2Backend final : public AudioBackend, public IXAudio2VoiceCallback, public IXAudio2EngineCallback
#ifndef RPCS3_UWP
    , public IMMNotificationClient
#endif
{
public:
	XAudio2Backend();
	~XAudio2Backend() override;

	XAudio2Backend(const XAudio2Backend&) = delete;
	XAudio2Backend& operator=(const XAudio2Backend&) = delete;

	std::string_view GetName() const override { return "XAudio2"sv; }

	bool Initialized() override;
	bool Operational() override;
	bool DefaultDeviceChanged() override;

	bool Open(std::string_view dev_id, AudioFreq freq, AudioSampleSize sample_size, AudioChannelCnt ch_cnt, audio_channel_layout layout) override;
	void Close() override;

	f64 GetCallbackFrameLen() override;

	void Play() override;
	void Pause() override;

private:
	static constexpr u32 INTERNAL_BUF_SIZE_MS = 25;

	Microsoft::WRL::ComPtr<IXAudio2> m_xaudio2_instance{};
	IXAudio2MasteringVoice* m_master_voice{};
	IXAudio2SourceVoice* m_source_voice{};
	bool m_com_init_success = false;

#ifndef RPCS3_UWP
	Microsoft::WRL::ComPtr<IMMDeviceEnumerator> m_device_enumerator{};
#endif

	// Protected by state callback mutex
	std::string m_current_device{};
	bool m_default_dev_changed = false;

	std::vector<u8> m_data_buf{};
#ifdef RPCS3_UWP
	XAudio2BufferPool m_submitted_buffers;
#endif
	std::array<u8, sizeof(float) * static_cast<u32>(AudioChannelCnt::SURROUND_7_1)> m_last_sample{};

	atomic_t<bool> m_reset_req = false;

	// XAudio voice callbacks
	void OnVoiceProcessingPassStart(UINT32 BytesRequired) noexcept override;
	void OnVoiceProcessingPassEnd() noexcept override {}
	void OnStreamEnd() noexcept override {}
	void OnBufferStart(void* /* pBufferContext */) noexcept override {}
	void OnBufferEnd(void* context) noexcept override
	{
#ifdef RPCS3_UWP
		XAudio2BufferPool::release(context);
#endif
	}
	void OnLoopEnd(void* /* pBufferContext */) noexcept override {}
	void OnVoiceError(void* /* pBufferContext */, HRESULT Error) noexcept override { OnCriticalError(Error); }

	// XAudio engine callbacks
	void OnProcessingPassStart() noexcept override {};
	void OnProcessingPassEnd() noexcept override {};
	void OnCriticalError(HRESULT Error) noexcept override;

#ifndef RPCS3_UWP
	// Desktop endpoint notifications are not available in Xbox AppContainer.
	IFACEMETHODIMP_(ULONG) AddRef() override { return 1; };
	IFACEMETHODIMP_(ULONG) Release() override { return 1; };
	IFACEMETHODIMP QueryInterface(REFIID /*iid*/, void** /*object*/) override { return E_NOINTERFACE; };
	IFACEMETHODIMP OnPropertyValueChanged(LPCWSTR /*device_id*/, const PROPERTYKEY /*key*/) override { return S_OK; };
	IFACEMETHODIMP OnDeviceAdded(LPCWSTR /*device_id*/) override { return S_OK; };
	IFACEMETHODIMP OnDeviceRemoved(LPCWSTR /*device_id*/) override { return S_OK; };
	IFACEMETHODIMP OnDeviceStateChanged(LPCWSTR /*device_id*/, DWORD /*new_state*/) override { return S_OK; };
	IFACEMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR new_default_device_id) override;
#endif

	void CloseUnlocked();
};
