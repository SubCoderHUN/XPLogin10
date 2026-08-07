#include "xplogin/HealthMonitor.h"

namespace xplogin {

HealthMonitor::HealthMonitor(std::shared_ptr<IStateStore> store,
                             std::shared_ptr<IClock> clock,
                             HealthPolicy policy)
    : store_(std::move(store)), clock_(std::move(clock)), policy_(policy) {}

HealthAction HealthMonitor::Decide(BootMode bootMode,
                                   bool disabledFlag,
                                   uint32_t startAttempts,
                                   const HealthPolicy& policy) {
    // Safe Mode is how an administrator gets back in. Never load there.
    if (bootMode != BootMode::Normal) {
        return HealthAction::Bypass;
    }
    if (!policy.enabled || disabledFlag) {
        return HealthAction::Bypass;
    }
    if (policy.bypassThreshold > 0 && startAttempts >= policy.bypassThreshold) {
        return HealthAction::Bypass;
    }
    if (policy.unfilterThreshold > 0 && startAttempts >= policy.unfilterThreshold) {
        return HealthAction::RunUnfiltered;
    }
    return HealthAction::RunNormally;
}

HealthAction HealthMonitor::BeginProviderStart(BootMode bootMode) {
    uint64_t attempts = 0;
    bool disabled = false;

    if (store_) {
        store_->ReadU64(healthkeys::kStartAttempts, &attempts);
        uint64_t disabledValue = 0;
        if (store_->ReadU64(healthkeys::kDisabledFlag, &disabledValue)) {
            disabled = disabledValue != 0;
        }
    }

    const HealthAction action = Decide(bootMode, disabled,
                                       static_cast<uint32_t>(attempts), policy_);

    // Record the attempt *before* we touch a single pixel, so a hard hang or a
    // bugcheck during painting still counts as a strike on the next boot.
    if (store_ && action != HealthAction::Bypass) {
        store_->WriteU64(healthkeys::kStartAttempts, attempts + 1);
        if (clock_) {
            store_->WriteU64(healthkeys::kLastStartMs, clock_->NowMs());
        }
    }
    return action;
}

void HealthMonitor::ReportSuccessfulLogon() {
    if (!store_) {
        return;
    }
    store_->WriteU64(healthkeys::kStartAttempts, 0);
    if (clock_) {
        store_->WriteU64(healthkeys::kLastSuccessMs, clock_->NowMs());
    }
    uint64_t total = 0;
    store_->ReadU64(healthkeys::kTotalLogons, &total);
    store_->WriteU64(healthkeys::kTotalLogons, total + 1);
    store_->Remove(healthkeys::kLastCrashReason);
}

void HealthMonitor::ReportUiReady() {
    if (!store_) {
        return;
    }
    // Only the strike counter: this is not a logon, so the success timestamp
    // and the logon tally are left alone.
    store_->WriteU64(healthkeys::kStartAttempts, 0);
    store_->Remove(healthkeys::kLastCrashReason);
}

void HealthMonitor::ReportCrash(const std::wstring& reason) {
    if (!store_) {
        return;
    }
    uint64_t attempts = 0;
    store_->ReadU64(healthkeys::kStartAttempts, &attempts);
    store_->WriteU64(healthkeys::kStartAttempts, attempts + 1);
    store_->WriteString(healthkeys::kLastCrashReason, reason);
}

void HealthMonitor::ForceDisable(const std::wstring& reason) {
    if (!store_) {
        return;
    }
    store_->WriteU64(healthkeys::kDisabledFlag, 1);
    store_->WriteString(healthkeys::kLastCrashReason, reason);
}

void HealthMonitor::ForceEnable() {
    if (!store_) {
        return;
    }
    store_->WriteU64(healthkeys::kDisabledFlag, 0);
    store_->WriteU64(healthkeys::kStartAttempts, 0);
    store_->Remove(healthkeys::kLastCrashReason);
}

uint32_t HealthMonitor::StartAttempts() const {
    uint64_t attempts = 0;
    if (store_) {
        store_->ReadU64(healthkeys::kStartAttempts, &attempts);
    }
    return static_cast<uint32_t>(attempts);
}

bool HealthMonitor::IsDisabled() const {
    uint64_t disabled = 0;
    if (store_) {
        store_->ReadU64(healthkeys::kDisabledFlag, &disabled);
    }
    return disabled != 0;
}

std::wstring HealthMonitor::LastCrashReason() const {
    std::wstring reason;
    if (store_) {
        store_->ReadString(healthkeys::kLastCrashReason, &reason);
    }
    return reason;
}

} // namespace xplogin
