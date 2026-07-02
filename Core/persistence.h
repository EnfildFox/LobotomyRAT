// Core/persistence.h
#pragma once

// Install persistence via Task Scheduler and Registry
void install_persistence(const char* exe_path);

// Remove persistence entries
void uninstall_persistence();

// Check and restore persistence if missing
void check_persistence();