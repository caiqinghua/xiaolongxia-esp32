#pragma once

#include "watch_motion_task.h"
#include "watch_wrist_control.h"

// exclusive_sleep requires the power manager's retained sleep transition.
WatchMotionLoad LoadWatchActivityRecord(WatchActivityRecord& record, bool exclusive_sleep);
bool SaveWatchActivityRecord(const WatchActivityRecord& record, bool exclusive_sleep);
WatchWristLoad LoadWatchWristPreferences(WatchWristPreferences& preferences);
bool SaveWatchWristPreferences(const WatchWristPreferences& preferences);
