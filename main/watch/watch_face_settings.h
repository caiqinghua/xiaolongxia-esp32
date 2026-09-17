#pragma once

#include "watch_view_state.h"

WatchFaceStyle ReadWatchFaceStyle();
bool SaveWatchFaceStyle(WatchFaceStyle style);
bool IsValidWatchFaceStyle(WatchFaceStyle style);
