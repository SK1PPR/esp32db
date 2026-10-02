// state_name.cpp — shared by the WiFi build (net.cpp) and the stubs (net_off.cpp).

#include "net/net.hpp"

namespace net {

const char *state_name(State s)
{
    switch (s) {
    case State::Off: return "off";
    case State::NoCredentials: return "no_credentials";
    case State::Connecting: return "connecting";
    case State::Connected: return "connected";
    case State::Failing: return "failing";
    }
    return "?";
}

}  // namespace net
