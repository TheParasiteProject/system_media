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

#pragma once

#include <android-base/thread_annotations.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <utils/Timers.h> // For nsecs_t

namespace android::audio_utils {

/**
 * A timed execution queue for scheduling functions to run at specific times.
 *
 * TimerQueue allows scheduling of `std::function<void()>` objects to be
 * executed at a future time, based on the `CLOCK_BOOTTIME` clock. It uses a
 * dedicated thread and `timerfd` for efficient and accurate scheduling.
 *
 * Callbacks are executed on the internal TimerQueue thread. If a callback
 * blocks, it will delay the execution of subsequent events.
 *
 * This class is thread-safe.
 */
class TimerQueue {
public:
    /**
     * TimerQueue ctor
     *
     * @param alarm set to true to allow a wake from suspend.
     */
    explicit TimerQueue(bool alarm = false);
    ~TimerQueue();

    /**
     * Opaque handle for a pending event.
     *
     * This handle is returned by `add()` and can be used with `remove()` to
     * cancel a pending event before it has executed.
     */
    using handle_t = int64_t;
    static constexpr handle_t INVALID_HANDLE = -1;

    /**
     * Schedules a function to be executed at a specific time.
     *
     * @param function The `std::function<void()>` to execute. The function
     *                 should not be null.
     * @param executionTime The absolute time in nanoseconds, based on the
     *                      `CLOCK_BOOTTIME` monotonic clock, at which the
     *                      function should be executed.
     *
     * @return A unique `handle_t` that can be used to cancel the event with
     *         `remove()`. Returns `INVALID_HANDLE` if the provided function
     *         is null.
     */
    handle_t add(std::function<void()> function, nsecs_t executionTime);

    /**
     * Removes a pending function from the execution queue.
     *
     * If the event corresponding to the handle has not yet been executed, it
     * will be removed and will not be called. If the event has already started
     * executing or has finished, this method does nothing.
     *
     * @param handle The handle of the event to remove, as returned by `add()`.
     *
     * @return `true` if the event was successfully found and removed before
     *         execution, `false` otherwise.
     */
    bool remove(handle_t handle);

    /**
     * Returns true if the timer was successfully initialized.
     */
    bool ready() const { return mTimerFd >= 0; }

    /**
     * Returns true if the timer can wake from suspend.
     */
    bool alarm() const { return mAlarm; }

private:
    void threadLoop();
    void armTimerForNextEvent_l() REQUIRES(mMutex);

    struct Event {
        std::function<void()> function;
        nsecs_t executionTime;
    };

    const bool mAlarm;  // if true, in ALARM mode and will wake from suspend.
    const int mTimerFd;
    std::mutex mMutex;
    std::thread mThread;  // effectively const
    bool mRunning GUARDED_BY(mMutex) = true;
    handle_t mNextHandle GUARDED_BY(mMutex) = 1;

    // Main storage for event data, keyed by handle.
    std::map<handle_t, Event> mEvents GUARDED_BY(mMutex);

    // Index to keep events sorted by time for efficient lookup of the next event.
    std::multimap<nsecs_t, handle_t> mTimeIndex GUARDED_BY(mMutex);
};

} // namespace android::audio_utils
