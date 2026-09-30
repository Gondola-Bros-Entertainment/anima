#include <anima/assets/staging.hpp>

namespace anima {
bool StopToken::stop_requested() const noexcept { return state_ && state_->load(std::memory_order_acquire); }
StopSource::StopSource() : state_(std::make_shared<std::atomic<bool>>(false)) {}
bool StopSource::request_stop() noexcept { return state_ && !state_->exchange(true, std::memory_order_acq_rel); }
bool StopSource::stop_requested() const noexcept { return state_ && state_->load(std::memory_order_acquire); }
const char *StagingCancelled::what() const noexcept { return "Staging was cancelled"; }
} // namespace anima
