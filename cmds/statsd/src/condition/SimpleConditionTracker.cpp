/*
 * Copyright (C) 2017 The Android Open Source Project
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

#define DEBUG false  // STOPSHIP if true
#include "Log.h"

#include "SimpleConditionTracker.h"
#include "guardrail/StatsdStats.h"

#include <log/logprint.h>

namespace android {
namespace os {
namespace statsd {

using std::map;
using std::string;
using std::unique_ptr;
using std::unordered_map;
using std::vector;

SimpleConditionTracker::SimpleConditionTracker(
        const ConfigKey& key, const string& name, const int index,
        const SimplePredicate& simplePredicate,
        const unordered_map<string, int>& trackerNameIndexMap)
    : ConditionTracker(name, index), mConfigKey(key) {
    VLOG("creating SimpleConditionTracker %s", mName.c_str());
    mCountNesting = simplePredicate.count_nesting();

    if (simplePredicate.has_start()) {
        auto pair = trackerNameIndexMap.find(simplePredicate.start());
        if (pair == trackerNameIndexMap.end()) {
            ALOGW("Start matcher %s not found in the config", simplePredicate.start().c_str());
            return;
        }
        mStartLogMatcherIndex = pair->second;
        mTrackerIndex.insert(mStartLogMatcherIndex);
    } else {
        mStartLogMatcherIndex = -1;
    }

    if (simplePredicate.has_stop()) {
        auto pair = trackerNameIndexMap.find(simplePredicate.stop());
        if (pair == trackerNameIndexMap.end()) {
            ALOGW("Stop matcher %s not found in the config", simplePredicate.stop().c_str());
            return;
        }
        mStopLogMatcherIndex = pair->second;
        mTrackerIndex.insert(mStopLogMatcherIndex);
    } else {
        mStopLogMatcherIndex = -1;
    }

    if (simplePredicate.has_stop_all()) {
        auto pair = trackerNameIndexMap.find(simplePredicate.stop_all());
        if (pair == trackerNameIndexMap.end()) {
            ALOGW("Stop all matcher %s not found in the config", simplePredicate.stop().c_str());
            return;
        }
        mStopAllLogMatcherIndex = pair->second;
        mTrackerIndex.insert(mStopAllLogMatcherIndex);
    } else {
        mStopAllLogMatcherIndex = -1;
    }

    mOutputDimension.insert(mOutputDimension.begin(), simplePredicate.dimension().begin(),
                            simplePredicate.dimension().end());

    if (mOutputDimension.size() > 0) {
        mSliced = true;
    }

    if (simplePredicate.initial_value() == SimplePredicate_InitialValue_FALSE) {
        mInitialValue = ConditionState::kFalse;
    } else {
        mInitialValue = ConditionState::kUnknown;
    }

    mNonSlicedConditionState = mInitialValue;

    mInitialized = true;
}

SimpleConditionTracker::~SimpleConditionTracker() {
    VLOG("~SimpleConditionTracker()");
}

bool SimpleConditionTracker::init(const vector<Predicate>& allConditionConfig,
                                  const vector<sp<ConditionTracker>>& allConditionTrackers,
                                  const unordered_map<string, int>& conditionNameIndexMap,
                                  vector<bool>& stack) {
    // SimpleConditionTracker does not have dependency on other conditions, thus we just return
    // if the initialization was successful.
    return mInitialized;
}

void print(map<HashableDimensionKey, int>& conditions, const string& name) {
    VLOG("%s DUMP:", name.c_str());
    for (const auto& pair : conditions) {
        VLOG("\t%s : %d", pair.first.c_str(), pair.second);
    }
}

void SimpleConditionTracker::handleStopAll(std::vector<ConditionState>& conditionCache,
                                           std::vector<bool>& conditionChangedCache) {
    // Unless the default condition is false, and there was nothing started, otherwise we have
    // triggered a condition change.
    conditionChangedCache[mIndex] =
            (mInitialValue == ConditionState::kFalse && mSlicedConditionState.empty()) ? false
                                                                                           : true;

    // After StopAll, we know everything has stopped. From now on, default condition is false.
    mInitialValue = ConditionState::kFalse;
    mSlicedConditionState.clear();
    conditionCache[mIndex] = ConditionState::kFalse;
}

bool SimpleConditionTracker::hitGuardRail(const HashableDimensionKey& newKey) {
    if (!mSliced || mSlicedConditionState.find(newKey) != mSlicedConditionState.end()) {
        // if the condition is not sliced or the key is not new, we are good!
        return false;
    }
    // 1. Report the tuple count if the tuple count > soft limit
    if (mSlicedConditionState.size() > StatsdStats::kDimensionKeySizeSoftLimit - 1) {
        size_t newTupleCount = mSlicedConditionState.size() + 1;
        StatsdStats::getInstance().noteConditionDimensionSize(mConfigKey, mName, newTupleCount);
        // 2. Don't add more tuples, we are above the allowed threshold. Drop the data.
        if (newTupleCount > StatsdStats::kDimensionKeySizeHardLimit) {
            ALOGE("Predicate %s dropping data for dimension key %s", mName.c_str(), newKey.c_str());
            return true;
        }
    }
    return false;
}

void SimpleConditionTracker::handleConditionEvent(const HashableDimensionKey& outputKey,
                                                  bool matchStart,
                                                  std::vector<ConditionState>& conditionCache,
                                                  std::vector<bool>& conditionChangedCache) {
    bool changed = false;
    auto outputIt = mSlicedConditionState.find(outputKey);
    ConditionState newCondition;
    if (hitGuardRail(outputKey)) {
        conditionChangedCache[mIndex] = false;
        // Tells the caller it's evaluated.
        conditionCache[mIndex] = ConditionState::kUnknown;
        return;
    }
    if (outputIt == mSlicedConditionState.end()) {
        // We get a new output key.
        newCondition = matchStart ? ConditionState::kTrue : ConditionState::kFalse;
        if (matchStart && mInitialValue != ConditionState::kTrue) {
            mSlicedConditionState[outputKey] = 1;
            changed = true;
        } else if (mInitialValue != ConditionState::kFalse) {
            // it's a stop and we don't have history about it.
            // If the default condition is not false, it means this stop is valuable to us.
            mSlicedConditionState[outputKey] = 0;
            changed = true;
        }
    } else {
        // we have history about this output key.
        auto& startedCount = outputIt->second;
        // assign the old value first.
        newCondition = startedCount > 0 ? ConditionState::kTrue : ConditionState::kFalse;
        if (matchStart) {
            if (startedCount == 0) {
                // This condition for this output key will change from false -> true
                changed = true;
            }

            // it's ok to do ++ here, even if we don't count nesting. The >1 counts will be treated
            // as 1 if not counting nesting.
            startedCount++;
            newCondition = ConditionState::kTrue;
        } else {
            // This is a stop event.
            if (startedCount > 0) {
                if (mCountNesting) {
                    startedCount--;
                    if (startedCount == 0) {
                        newCondition = ConditionState::kFalse;
                    }
                } else {
                    // not counting nesting, so ignore the number of starts, stop now.
                    startedCount = 0;
                    newCondition = ConditionState::kFalse;
                }
                // if everything has stopped for this output key, condition true -> false;
                if (startedCount == 0) {
                    changed = true;
                }
            }

            // if default condition is false, it means we don't need to keep the false values.
            if (mInitialValue == ConditionState::kFalse && startedCount == 0) {
                mSlicedConditionState.erase(outputIt);
                VLOG("erase key %s", outputKey.c_str());
            }
        }
    }

    // dump all dimensions for debugging
    if (DEBUG) {
        print(mSlicedConditionState, mName);
    }

    conditionChangedCache[mIndex] = changed;
    conditionCache[mIndex] = newCondition;

    VLOG("SimplePredicate %s nonSlicedChange? %d", mName.c_str(),
         conditionChangedCache[mIndex] == true);
}

void SimpleConditionTracker::evaluateCondition(const LogEvent& event,
                                               const vector<MatchingState>& eventMatcherValues,
                                               const vector<sp<ConditionTracker>>& mAllConditions,
                                               vector<ConditionState>& conditionCache,
                                               vector<bool>& conditionChangedCache) {
    if (conditionCache[mIndex] != ConditionState::kNotEvaluated) {
        // it has been evaluated.
        VLOG("Yes, already evaluated, %s %d", mName.c_str(), conditionCache[mIndex]);
        return;
    }

    if (mStopAllLogMatcherIndex >= 0 && mStopAllLogMatcherIndex < int(eventMatcherValues.size()) &&
        eventMatcherValues[mStopAllLogMatcherIndex] == MatchingState::kMatched) {
        handleStopAll(conditionCache, conditionChangedCache);
        return;
    }

    int matchedState = -1;
    // Note: The order to evaluate the following start, stop, stop_all matters.
    // The priority of overwrite is stop_all > stop > start.
    if (mStartLogMatcherIndex >= 0 &&
        eventMatcherValues[mStartLogMatcherIndex] == MatchingState::kMatched) {
        matchedState = 1;
    }

    if (mStopLogMatcherIndex >= 0 &&
        eventMatcherValues[mStopLogMatcherIndex] == MatchingState::kMatched) {
        matchedState = 0;
    }

    if (matchedState < 0) {
        // The event doesn't match this condition. So we just report existing condition values.
        conditionChangedCache[mIndex] = false;
        if (mSliced) {
            // if the condition result is sliced. metrics won't directly get value from the
            // cache, so just set any value other than kNotEvaluated.
            conditionCache[mIndex] = ConditionState::kUnknown;
        } else {
            const auto& itr = mSlicedConditionState.find(DEFAULT_DIMENSION_KEY);
            if (itr == mSlicedConditionState.end()) {
                // condition not sliced, but we haven't seen the matched start or stop yet. so
                // return initial value.
                conditionCache[mIndex] = mInitialValue;
            } else {
                // return the cached condition.
                conditionCache[mIndex] =
                        itr->second > 0 ? ConditionState::kTrue : ConditionState::kFalse;
            }
        }

        return;
    }

    // outputKey is the output key values. e.g, uid:1234
    const HashableDimensionKey outputKey(getDimensionKey(event, mOutputDimension));
    handleConditionEvent(outputKey, matchedState == 1, conditionCache, conditionChangedCache);
}

void SimpleConditionTracker::isConditionMet(
        const map<string, HashableDimensionKey>& conditionParameters,
        const vector<sp<ConditionTracker>>& allConditions,
        vector<ConditionState>& conditionCache) const {
    const auto pair = conditionParameters.find(mName);
    HashableDimensionKey key =
            (pair == conditionParameters.end()) ? DEFAULT_DIMENSION_KEY : pair->second;

    if (pair == conditionParameters.end() && mOutputDimension.size() > 0) {
        ALOGE("Predicate %s output has dimension, but it's not specified in the query!",
              mName.c_str());
        conditionCache[mIndex] = mInitialValue;
        return;
    }

    VLOG("simplePredicate %s query key: %s", mName.c_str(), key.c_str());

    auto startedCountIt = mSlicedConditionState.find(key);
    if (startedCountIt == mSlicedConditionState.end()) {
        conditionCache[mIndex] = mInitialValue;
    } else {
        conditionCache[mIndex] =
                startedCountIt->second > 0 ? ConditionState::kTrue : ConditionState::kFalse;
    }

    VLOG("Predicate %s return %d", mName.c_str(), conditionCache[mIndex]);
}

}  // namespace statsd
}  // namespace os
}  // namespace android
