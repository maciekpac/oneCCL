/*
 Copyright 2016-2020 Intel Corporation
 
 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at
 
     http://www.apache.org/licenses/LICENSE-2.0
 
 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
*/
#include <iomanip>
#include <numeric>
#include <sstream>
#include <unordered_map>

#include "common/global/global.hpp"
#include "common/log/log.hpp"
#include "sched_timer.hpp"

#ifdef CCL_ENABLE_ITT
#include "ittnotify.h"
#include <map>
#include <stack>
#endif // CCL_ENABLE_ITT

namespace ccl {

void sched_timer::start() {
    start_time = std::chrono::high_resolution_clock::now();
    started = true;
}

void sched_timer::update() {
    CCL_THROW_IF_NOT(started, "timer is not started, but update is requested");
    auto current_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::micro> time_span = current_time - start_time;
    time_usec += time_span.count();
    start_time = current_time;
}

void sched_timer::reset() {
    time_usec = 0;
    started = false;
}

bool sched_timer::is_started() const {
    return started;
}

long double sched_timer::get_elapsed_usec() const {
    return time_usec;
}

std::string to_string(const sched_timer& timer) {
    std::stringstream ss;
    ss.precision(2);
    ss << std::fixed << timer.get_elapsed_usec();
    return ss.str();
}

#if defined(CCL_ENABLE_ITT)

namespace profile {
namespace itt {

static constexpr unsigned max_entry_name_length = 64;
// Map of vectors of events that allows us to avoid multiple
// expensive calls to `__itt_event_create`.
thread_local std::unordered_map<const char*, std::vector<__itt_event>> event_cache;
// Inflight events are events fetched from cache that were not returned yet.
// This structure allows us to easily return finished event to cache vector
// it belongs to.
thread_local std::unordered_map<__itt_event, std::vector<__itt_event>*> inflight_event_cache;
thread_local std::unordered_map<__itt_event, unsigned> inflight_event_ref_counts;

void set_thread_name(const std::string& name) {
    __itt_thread_set_name(name.c_str());
}

__itt_event event_get(const char* name) {
    if (ccl::global_data::env().itt_level == 0) {
        return invalid_event;
    }

    __itt_event event;

    auto cache_entry = event_cache.find(name);

    if (cache_entry == event_cache.end()) {
        // Initialize vector of __itt_events
        event_cache[name];
        cache_entry = event_cache.find(name);
    }

    // Entry in event_cache is initialized, we
    // can fetch vector with specific event type
    auto cached_vector = &cache_entry->second;

    if (!cached_vector->empty()) {
        // There is cached __itt_event handle
        // that can be used once again
        event = cached_vector->back();
        cached_vector->pop_back();
    }
    else {
        // No cached events
        char prefix_name[max_entry_name_length] = "oneCCL::";
        strncat(prefix_name, name, max_entry_name_length - strlen(prefix_name));
        event = __itt_event_create(prefix_name, strlen(prefix_name));
    }

    // Record cache vector to which the event should be
    // returned on event_end
    inflight_event_cache[event] = cached_vector;

    auto event_ref_count = inflight_event_ref_counts.find(event);

    if (event_ref_count == inflight_event_ref_counts.end()) {
        // The event handle is not in use by any entry
        inflight_event_ref_counts[event] = 1;
    }
    else {
        event_ref_count->second++;
    }

    return event;
}

void event_start(__itt_event event) {
    if (ccl::global_data::env().itt_level == 0) {
        return;
    }

    __itt_event_start(event);
}

void event_end(__itt_event event) {
    if (ccl::global_data::env().itt_level == 0) {
        return;
    }

    __itt_event_end(event);
    inflight_event_cache[event]->push_back(event);

    auto event_ref_count = inflight_event_ref_counts.find(event);

    event_ref_count->second--;
    if (event_ref_count->second == 0) {
        // No more references to the event are currently used
        // which means that we can remove the event from
        // 'inflight' cache.
        inflight_event_cache.erase(event);
        inflight_event_ref_counts.erase(event);
    }
}

} // namespace itt
} // namespace profile

#endif // CCL_ENABLE_ITT

} // namespace ccl
