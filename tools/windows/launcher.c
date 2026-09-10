// SPDX-License-Identifier: Apache-2.0
// Forward the original command line without invoking cmd.exe or PowerShell.
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <wchar.h>
#include <windows.h>

#include "bootstrap_config.h"

// Process-owned buffers are bounded by the Windows command-line/path limit.
static wchar_t module_path[32768], python_path[32768], script_path[32768], command[32768];

static BOOL WINAPI control_handler(DWORD event) { return event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT; }

int wmain(int argc, wchar_t** argv) {
    int json_mode = 0;
    for (int i = 1; i < argc && wcscmp(argv[i], L"--") != 0; ++i)
        if (wcscmp(argv[i], L"--json") == 0) json_mode = 1;
    DWORD length = GetModuleFileNameW(NULL, module_path, 32768);
    wchar_t* separator = wcsrchr(module_path, L'\\');
    if (length == 0 || length >= 32768 || separator == NULL) goto failed;
    *separator = L'\0';
    if (_snwprintf_s(python_path, 32768, _TRUNCATE, L"%ls\\..\\versions\\%ls\\python\\python.exe", module_path,
                     BOOTSTRAP_ID) < 0 ||
        _snwprintf_s(script_path, 32768, _TRUNCATE, L"%ls\\..\\versions\\%ls\\launch.py", module_path, BOOTSTRAP_ID) <
            0)
        goto failed;
    const wchar_t* rest = GetCommandLineW();
    if (*rest == L'"') {
        ++rest;
        while (*rest && *rest != L'"') ++rest;
        if (*rest) ++rest;
    } else {
        while (*rest && *rest != L' ' && *rest != L'\t') ++rest;
    }
    while (*rest == L' ' || *rest == L'\t') ++rest;
    if (_snwprintf_s(command, 32768, _TRUNCATE, L"\"%ls\" -I -X utf8 \"%ls\" %ls", python_path, script_path, rest) < 0)
        goto failed;
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    SetConsoleCtrlHandler(control_handler, TRUE);
    if (!CreateProcessW(python_path, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) goto failed;
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    return exit_code <= 4 ? (int)exit_code : 1;
failed:
    fprintf(stderr,
            "MicroPixel launcher could not start the installed runtime (Windows error %lu). Reinstall MicroPixel.\n",
            GetLastError());
    if (json_mode)
        puts(
            "{\"schema_version\":1,\"ok\":false,\"code\":\"bootstrap_failed\",\"result\":{},\"error\":{\"code\":"
            "\"bootstrap_failed\",\"message\":\"Unable to start the installed runtime; reinstall "
            "MicroPixel\"},\"warnings\":[]}");
    return 4;
}
