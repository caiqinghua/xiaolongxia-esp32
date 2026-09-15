#pragma once
#include "http.h"
#include "boards/common/board_runtime_profile.h"
#include <memory>
#include <vector>
class OtaTestNetwork {
public:
    std::unique_ptr<Http> CreateHttp(int);
};
class Board {
public:
    static Board& GetInstance() { static Board board; return board; }
    OtaTestNetwork* GetNetwork();
    BoardRuntimeProfile profile = BoardRuntimeProfile::kAssistant;
    BoardRuntimeProfile GetRuntimeProfile() const { return profile; }
};
