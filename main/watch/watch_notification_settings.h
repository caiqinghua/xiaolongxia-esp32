#pragma once

// Missing settings are disabled. Storage errors return -1 and fail closed.
int ReadWatchNotificationsEnabled();
bool SaveWatchNotificationsEnabled(bool enabled);
