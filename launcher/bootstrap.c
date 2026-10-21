/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The root entry needs only Windows system DLLs. The real GUI and shared DLLs live in out/. */
// clang-format off
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>
// clang-format on
static int append(wchar_t *dst, size_t cap, const wchar_t *s) {
    size_t n = wcslen(dst), m = wcslen(s);
    if (n + m >= cap)
        return 0;
    memcpy(dst + n, s, (m + 1) * sizeof(wchar_t));
    return 1;
}
static int quote(wchar_t *dst, size_t cap, const wchar_t *s) {
    if (!append(dst, cap, L"\""))
        return 0;
    unsigned slash = 0;
    for (; *s; ++s) {
        if (*s == L'\\') {
            ++slash;
            continue;
        }
        for (unsigned i = 0; i < slash * (*s == L'"' ? 2u : 1u); ++i)
            if (!append(dst, cap, L"\\"))
                return 0;
        slash = 0;
        if (*s == L'"' && !append(dst, cap, L"\\"))
            return 0;
        wchar_t c[] = {*s, 0};
        if (!append(dst, cap, c))
            return 0;
    }
    for (unsigned i = 0; i < slash * 2; ++i)
        if (!append(dst, cap, L"\\"))
            return 0;
    return append(dst, cap, L"\"");
}
int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR args, int show) {
    (void)instance;
    (void)previous;
    (void)args;
    (void)show;
    wchar_t root[32768], exe[32768], command[32768] = {0};
    DWORD n = GetModuleFileNameW(NULL, root, 32768);
    if (!n || n >= 32768)
        return 1;
    wchar_t *last = wcsrchr(root, L'\\');
    if (!last)
        return 1;
    *last = 0;
    wcscpy(exe, root);
    if (!append(exe, 32768, L"\\out\\AegisLauncher.exe") || !quote(command, 32768, exe))
        return 1;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i)
        if (!append(command, 32768, L" ") || !quote(command, 32768, argv[i])) {
            LocalFree(argv);
            return 1;
        }
    LocalFree(argv);
    STARTUPINFOW si = {0};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, root, &si, &pi)) {
        MessageBoxW(NULL, L"无法启动 Aegis。请完整解压发布包，保留 out、assets 和 python 目录。",
                    L"Aegis", MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
