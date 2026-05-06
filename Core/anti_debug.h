// Core/anti_debug.h
#pragma once

// Check for debugger/sandbox and activate sleep mode if detected
void check_debugger();

// Global flag: true = agent is in sleep mode (heartbeat only, no command execution)
extern volatile bool g_sleepMode;