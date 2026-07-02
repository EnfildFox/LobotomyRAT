#include "compression.h"
#include <ntstatus.h>
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)

typedef NTSTATUS (NTAPI* pRtlGetCompressionWorkSpaceSize)(USHORT, PULONG, PULONG);
typedef NTSTATUS (NTAPI* pRtlCompressBuffer)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, ULONG, PULONG, PVOID);
typedef NTSTATUS (NTAPI* pRtlDecompressBuffer)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, PULONG);

static pRtlCompressBuffer RtlCompressBuffer = nullptr;
static pRtlDecompressBuffer RtlDecompressBuffer = nullptr;
static pRtlGetCompressionWorkSpaceSize RtlGetCompressionWorkSpaceSize = nullptr;
static bool init_ntdll() {
    static bool once = false;
    static bool ok = false;
    if (!once) {
        once = true;
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll) {
            RtlCompressBuffer = (pRtlCompressBuffer)GetProcAddress(ntdll, "RtlCompressBuffer");
            RtlDecompressBuffer = (pRtlDecompressBuffer)GetProcAddress(ntdll, "RtlDecompressBuffer");
            RtlGetCompressionWorkSpaceSize = (pRtlGetCompressionWorkSpaceSize)GetProcAddress(ntdll, "RtlGetCompressionWorkSpaceSize");
            ok = (RtlCompressBuffer && RtlDecompressBuffer && RtlGetCompressionWorkSpaceSize);
        }
    }
    return ok;
}

bool compress_xpress(const std::vector<uint8_t>& input, std::vector<uint8_t>& compressed) {
    if (!init_ntdll() || input.empty()) return false;
    ULONG wsSize = 0, fsSize = 0;
    if (RtlGetCompressionWorkSpaceSize(COMPRESSION_FORMAT_XPRESS, &wsSize, &fsSize) != 0) return false;
    std::vector<uint8_t> workspace(wsSize + fsSize); // выделяем рабочий буфер суммарного размера
    // буфер для сжатыъх данных
    ULONG compressedSize = input.size() + 8; // минимальный запас
    compressed.resize(compressedSize);
    ULONG finalSize = 0;
    NTSTATUS status = RtlCompressBuffer(COMPRESSION_FORMAT_XPRESS, (PUCHAR)input.data(), (ULONG)input.size(),
        (PUCHAR)compressed.data(), (ULONG)compressed.size(), 4096, &finalSize, workspace.data());
    if (NT_SUCCESS(status)) {
        compressed.resize(finalSize);
        return true;
    }
    return false;
}

bool decompress_xpress(const std::vector<uint8_t>& compressed, std::vector<uint8_t>& output) {
    if (!init_ntdll() || compressed.empty()) return false;
    ULONG wsSize = 0, fsSize = 0;
    // Получаем размер рабочего буфера ивыделяем его 
    if (RtlGetCompressionWorkSpaceSize(COMPRESSION_FORMAT_XPRESS, &wsSize, &fsSize) != 0) return false;
    std::vector<uint8_t> workspace(wsSize + fsSize);
    // Нам нужно знать несжатый размер. Для упрощения выделим буфер 2 МБ.
    output.resize(2 * 1024 * 1024);
    ULONG decompressedSize = 0;
    NTSTATUS status = RtlDecompressBuffer(COMPRESSION_FORMAT_XPRESS, (PUCHAR)output.data(), (ULONG)output.size(),
        (PUCHAR)compressed.data(), (ULONG)compressed.size(), &decompressedSize);
    if (NT_SUCCESS(status)) {
        output.resize(decompressedSize);
        return true;
    }
    return false;
}