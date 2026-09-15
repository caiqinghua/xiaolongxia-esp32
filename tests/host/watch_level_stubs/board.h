#pragma once
#include "boards/common/board_runtime_profile.h"
class Board {
public:
    inline static BoardRuntimeProfile profile = BoardRuntimeProfile::kWatchAssistantRecorder;
    static Board& GetInstance() { static Board board; return board; }
    BoardRuntimeProfile GetRuntimeProfile() const { return profile; }
};
