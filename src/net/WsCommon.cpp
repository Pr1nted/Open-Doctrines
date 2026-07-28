// The part of the WebSocket client that is the same on every backend.
// URL parsing lives in NetUrl.cpp, shared with the HTTPS client.

#include "WebSocket.h"

const char* wsStateName(WsState s) {
    switch (s) {
        case WsState::Idle:       return "Idle";
        case WsState::Connecting: return "Connecting";
        case WsState::Open:       return "Open";
        case WsState::Closing:    return "Closing";
        case WsState::Closed:     return "Closed";
    }
    return "Unknown";
}
