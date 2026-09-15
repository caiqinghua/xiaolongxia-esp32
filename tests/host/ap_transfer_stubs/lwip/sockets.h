#pragma once
#include "ap_test_sdk.h"
#define shutdown ap_test_shutdown
constexpr int SHUT_RDWR = 2;
int ap_test_shutdown(int socket, int mode);
