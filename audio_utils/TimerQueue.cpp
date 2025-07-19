/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "TimerQueue"

#include <audio_utils/TimerQueue.h>

#include <log/log.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <utils/SystemClock.h>

namespace android::audio_utils {

TimerQueue::TimerQueue(bool alarm)
    : mAlarm(alarm),
      mTimerFd{timerfd_create((alarm ? CLOCK_BOOTTIME_ALARM : CLOCK_BOOTTIME),
            TFD_CLOEXEC)} {
    if (mTimerFd < 0) {
        ALOGE("Failed to create timerfd: %s", strerror(errno));
        return;
    }
    mThread = std::thread(&TimerQueue::threadLoop, this);
}

TimerQueue::~TimerQueue() {
    if (mTimerFd < 0) return;

    {
        std::lock_guard lock(mMutex);
        mRunning = false;
        armTimerForNextEvent_l();
    }
    if (mThread.joinable()) {
        mThread.join();
    }
    close(mTimerFd);
}

TimerQueue::handle_t TimerQueue::add(std::function<void()> function, nsecs_t executionTime) {
    if (!function) {
        return INVALID_HANDLE;
    }

    std::lock_guard lock(mMutex);

    handle_t handle = mNextHandle;
    if (handle == std::numeric_limits<handle_t>::max()) { // Handle wrap-around
        mNextHandle = 1;
    } else {
        ++mNextHandle;
    }

    const bool needsReschedule = mTimeIndex.empty() || executionTime < mTimeIndex.begin()->first;

    mEvents.emplace(handle, Event{std::move(function), executionTime});
    mTimeIndex.emplace(executionTime, handle);

    if (needsReschedule) {
        armTimerForNextEvent_l();
    }

    return handle;
}

bool TimerQueue::remove(handle_t handle) {
    if (handle == INVALID_HANDLE) {
        return false;
    }

    std::lock_guard lock(mMutex);

    const auto eventIt = mEvents.find(handle);
    if (eventIt == mEvents.end()) {
        return false;
    }

    const nsecs_t executionTime = eventIt->second.executionTime;
    const bool wasNext = (mTimeIndex.begin()->second == handle);

    mEvents.erase(eventIt);

    const auto [first, last] = mTimeIndex.equal_range(executionTime);
    for (auto it = first; it != last; ++it) {
        if (it->second == handle) {
            mTimeIndex.erase(it);
            break;
        }
    }

    if (wasNext) {
        armTimerForNextEvent_l();
    }

    return true;
}

void TimerQueue::armTimerForNextEvent_l() {
    struct itimerspec spec{};
    if (!mRunning) {
        // Set a timer for 1 nanosecond to ensure it fires immediately and unblocks the read.
        spec.it_value.tv_nsec = 1;
        timerfd_settime(mTimerFd, 0 /* flags */, &spec, /* old_value */ nullptr);
    } else if (!mTimeIndex.empty()) {
        nsecs_t nextTime = mTimeIndex.begin()->first;
        spec.it_value.tv_sec = nextTime / 1'000'000'000;
        spec.it_value.tv_nsec = nextTime % 1'000'000'000;
        timerfd_settime(mTimerFd, TFD_TIMER_ABSTIME, &spec, /* old_value */ nullptr);
    } else {
        // Set timer to 0 disables it
        timerfd_settime(mTimerFd, TFD_TIMER_ABSTIME, &spec, /* old_value */ nullptr);
    }
}

void TimerQueue::threadLoop() {
    while (true) {
        uint64_t expirations;
        ssize_t n = read(mTimerFd, &expirations, sizeof(expirations));
        std::vector<std::function<void()>> functionsToRun;
        {
            ALOGV("%s: read %zd",__func__, n);
            std::lock_guard lock(mMutex);
            if (!mRunning) break;
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) {
                    continue;
                }
                ALOGE("read(timerfd) failed: %s", strerror(errno));
             }

            const nsecs_t now = elapsedRealtimeNano();

            while (!mTimeIndex.empty() && mTimeIndex.begin()->first <= now) {
                const auto it = mTimeIndex.begin();
                const handle_t handle = it->second;

                const auto eventIt = mEvents.find(handle);
                if (eventIt != mEvents.end()) {
                    functionsToRun.push_back(std::move(eventIt->second.function));
                    mEvents.erase(eventIt);
                }
                mTimeIndex.erase(it);
            }

            armTimerForNextEvent_l();
        }

        for (const auto& func : functionsToRun) {
            func();
        }
    }
}

} // namespace android::audio_utils
