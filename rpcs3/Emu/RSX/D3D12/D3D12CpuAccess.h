#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <memory>
#include <stdexcept>

namespace d3d12
{
// The renderer grants leases only after synchronizing pending GPU writes.
// pump() cannot return to drawing while a granted CPU lease is alive. It
// continues servicing requests, so nested/concurrent CPU accesses cannot
// deadlock behind the first lease. GPU execution is deliberately serialized.
class cpu_access_queue
{
    struct request
    {
        std::uint32_t address, length;
        bool write, lease;
        std::promise<bool> completed;
    };
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<request>> requests;
    std::size_t active = 0;
    bool closed = false;
public:
    template <typename Wake>
    bool submit(std::uint32_t address, std::uint32_t length, bool write, bool lease, Wake wake)
    {
        if (!length) return false;
        if (std::uint64_t(address) + length > (1ull << 32)) throw std::invalid_argument("Invalid CPU access range");
        auto job = std::make_shared<request>();
        job->address = address; job->length = length; job->write = write; job->lease = lease;
        auto done = job->completed.get_future();
        {
            std::lock_guard lock(mutex);
            if (closed) throw std::runtime_error("RSX CPU access queue is closed");
            requests.push_back(job);
        }
        changed.notify_all();
        wake();
        return done.get();
    }
    template <typename Synchronize>
    void pump(Synchronize synchronize)
    {
        for (;;)
        {
            std::deque<std::shared_ptr<request>> batch;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] {return closed || !requests.empty() || !active;});
                if (closed || (requests.empty() && !active)) return;
                batch.swap(requests);
            }
            for (const auto& job : batch)
            {
                try
                {
                    synchronize(job->address, job->length, job->write);
                    {
                        std::lock_guard lock(mutex);
                        if (closed) throw std::runtime_error("RSX stopped during CPU access synchronization");
                        if (job->lease) ++active;
                    }
                    job->completed.set_value(job->lease);
                }
                catch (...) {job->completed.set_exception(std::current_exception());}
            }
        }
    }
    void release() noexcept
    {
        {std::lock_guard lock(mutex); if (active) --active;}
        changed.notify_all();
    }
    void close()
    {
        {
            std::lock_guard lock(mutex);
            closed = true;
            for (const auto& job : requests)
                job->completed.set_exception(std::make_exception_ptr(std::runtime_error("RSX stopped before CPU synchronization")));
            requests.clear();
        }
        changed.notify_all();
    }
};
}
