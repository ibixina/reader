#pragma once
#include <atomic>
#include <memory>

namespace reader {

// Cooperative cancellation for every background stage (§7, §44).
// Closing a document detaches pending jobs via a shared generation counter.
class CancellationToken {
public:
    CancellationToken() : state_(std::make_shared<std::atomic<bool>>(false)) {}
    void cancel() const { state_->store(true, std::memory_order_relaxed); }
    bool cancelled() const { return state_->load(std::memory_order_relaxed); }
    CancellationToken child() const {
        CancellationToken c;
        if (cancelled()) c.cancel();
        return c;
    }

private:
    std::shared_ptr<std::atomic<bool>> state_;
};

} // namespace reader
