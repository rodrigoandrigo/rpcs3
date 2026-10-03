#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <mutex>
#include <cstdint>
#include <utility>
#include <stdexcept>

namespace d3d12
{
// One embedding context per DLL. No HWND, CoreWindow or UI objects are retained
// here. The frontend owns presentation and the DIRECT queue; completed output
// textures are immutable snapshots and remain alive through host GPU use.
class presentation_context
{
    template<class T> using ptr = Microsoft::WRL::ComPtr<T>;
    std::mutex m_mutex;
    ptr<ID3D12Device> m_device;
    ptr<ID3D12CommandQueue> m_queue;
    ptr<ID3D12Resource> m_latest;
    std::uint64_t m_serial = 0;
public:
    void attach(ID3D12Device* device, ID3D12CommandQueue* queue)
    {
        if (!device || !queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
            throw std::invalid_argument("D3D12 presentation requires a device and DIRECT queue");
        ptr<ID3D12Device> owner;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(owner.GetAddressOf()))))
            throw std::invalid_argument("Cannot query presentation queue device");
        ptr<IUnknown> first, second;
        device->QueryInterface(IID_PPV_ARGS(first.GetAddressOf()));
        owner->QueryInterface(IID_PPV_ARGS(second.GetAddressOf()));
        if (!first || first.Get() != second.Get())
            throw std::invalid_argument("Presentation device and queue do not match");
        std::lock_guard lock(m_mutex);
        if (m_device) throw std::logic_error("D3D12 presentation is already attached");
        m_device = device;
        m_queue = queue;
    }
    auto device_and_queue()
    {
        std::lock_guard lock(m_mutex);
        if (!m_device) throw std::logic_error("Host D3D12 presentation is not attached");
        return std::make_pair(m_device, m_queue);
    }
    // Called after ExecuteCommandLists on the SAME queue used by the frontend.
    // Queue ordering makes subsequent host sampling safe without CPU waiting.
    void publish(ID3D12Resource* immutable_output)
    {
        std::lock_guard lock(m_mutex);
        m_latest = immutable_output;
        ++m_serial;
    }
    ptr<ID3D12Resource> acquire(std::uint64_t& serial)
    {
        std::lock_guard lock(m_mutex);
        serial = m_serial;
        return m_latest;
    }
    void clear_frame()
    {
        std::lock_guard lock(m_mutex);
        m_latest.Reset();
    }
    void detach()
    {
        std::lock_guard lock(m_mutex);
        m_latest.Reset();
        m_queue.Reset();
        m_device.Reset();
    }
};
inline presentation_context& presentation()
{
    static presentation_context context;
    return context;
}
}
