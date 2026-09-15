#pragma once
#include "tcp.h"
#include <memory>
class NetworkInterface {
public:
    virtual ~NetworkInterface() = default;
    virtual std::unique_ptr<Tcp> CreateTcp(int) = 0;
    virtual std::unique_ptr<Tcp> CreateSsl(int) = 0;
};
