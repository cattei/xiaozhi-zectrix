#pragma once

#include <cstdint>
#include <functional>

enum class SleepBusySrc : uint32_t {
    Net     = 1u << 0,
    Audio   = 1u << 1,
    Display = 1u << 2,
    Ui      = 1u << 3,
    Nvs     = 1u << 4,
    Todo    = 1u << 5,
    Protocol = 1u << 6,
};

class SleepManager {
public:
    static SleepManager& GetInstance();

    void SetBusy(SleepBusySrc src, bool busy);
    void Kick(uint32_t delay_ms, const char* reason = nullptr);
    void Hold(const char* reason = nullptr);
    void Release(const char* reason = nullptr);
    bool CanSleepNow() const;
    bool PrepareForLightSleep();
    void SetPreSleepHook(std::function<bool()> hook);

    uint32_t GetBusyMask() const;
    int GetHoldCount() const;
    int64_t GetDeadlineMs() const;

private:
    SleepManager() = default;
};

inline void sm_set_busy(SleepBusySrc src, bool busy) {
    SleepManager::GetInstance().SetBusy(src, busy);
}

inline void sm_kick(uint32_t delay_ms, const char* reason = nullptr) {
    SleepManager::GetInstance().Kick(delay_ms, reason);
}

inline void sm_hold(const char* reason = nullptr) {
    SleepManager::GetInstance().Hold(reason);
}

inline void sm_release(const char* reason = nullptr) {
    SleepManager::GetInstance().Release(reason);
}

inline void sg_release(const char* reason = nullptr) {
    sm_release(reason);
}

inline bool sm_can_sleep_now() {
    return SleepManager::GetInstance().CanSleepNow();
}

inline bool sm_prepare_for_light_sleep() {
    return SleepManager::GetInstance().PrepareForLightSleep();
}
