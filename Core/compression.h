// Core/compression.h
#pragma once
#include <windows.h>
#include <string>
#include <vector>

// Сжатие буфера с помощью RtlCompressBuffer (XPRESS)
// Возвращает true и заполняет compressed_data, false при ошибке
bool compress_xpress(const std::vector<uint8_t>& input, std::vector<uint8_t>& compressed);

// Распаковка (для обратной стороны, если агент будет принимать сжатые команды)
bool decompress_xpress(const std::vector<uint8_t>& compressed, std::vector<uint8_t>& output);