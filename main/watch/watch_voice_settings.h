#pragma once

// Missing settings are disabled. Storage errors return -1 and fail closed.
int ReadWatchVoiceEnabled();
bool SaveWatchVoiceEnabled(bool enabled);
