#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "MinHook.h"
#include <psapi.h>
#include <fstream>
#include <functional>
#include <string>
#include <sstream>
#include <vector>
#include <cstdint>
#include <iostream>
#include <fcntl.h>
#include <io.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

constexpr uint16_t TCP_PORT = 28771;
const char* PCALL_SIG = "? ? ? ? ? 57 48 83 EC 40 33 C0 41 8B F8 44 8B D2 48 8B D9 45 85 C9 74 0C 41 8B D1";
const char* LOADBUFFER_SIG = "? ? ? ? ? ? ? ? ? ? 55 48 8B EC 48 81 EC 80 00 00 00 48 8B F9 ? ? ? ? 33 C9 ? ? ? ? 4D 85 C9 ? ? ? ? 48 8D 45 D8 ? ? ? ? ? ? ? ? 4C 8D 45";

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;
};

Pattern CreatePattern(const char* signature) {
    Pattern pattern;
    std::string sig(signature);

    for (size_t i = 0; i < sig.length(); i += 2) {
        if (sig[i] == ' ') { i--; continue; }
        if (sig[i] == '?') {
            pattern.bytes.push_back(0);
            pattern.mask.push_back(false);
            if (sig[i + 1] == '?') i++;
        }
        else {
            char byte[3] = { sig[i], sig[i + 1], 0 };
            pattern.bytes.push_back(static_cast<uint8_t>(strtol(byte, nullptr, 16)));
            pattern.mask.push_back(true);
        }
    }
    return pattern;
}

uintptr_t FindPattern(const uint8_t* data, const size_t dataSize, const Pattern& pattern) {
    for (size_t i = 0; i < dataSize - pattern.bytes.size(); i++) {
        bool found = true;
        for (size_t j = 0; j < pattern.bytes.size(); j++) {
            if (pattern.mask[j] && data[i + j] != pattern.bytes[j]) {
                found = false;
                break;
            }
        }
        if (found) return i;
    }
    return 0;
}

HANDLE g_ConsoleHandle = NULL;
bool g_ConsoleEnabled = false;

bool CheckForConsoleArg() {
    LPWSTR cmdLine = GetCommandLineW();
    int argc;
    LPWSTR* argv = CommandLineToArgvW(cmdLine, &argc);

    if (argv != NULL) {
        for (int i = 1; i < argc; i++) {
            if (_wcsicmp(argv[i], L"-console") == 0) {
                LocalFree(argv);
                return true;
            }
        }
        LocalFree(argv);
    }
    return false;
}

void InitConsole() {
    if (!CheckForConsoleArg()) {
        g_ConsoleEnabled = false;
        return;
    }

    g_ConsoleEnabled = true;
    AllocConsole();
    g_ConsoleHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTitleA("Mod Debug Console");

    COORD bufferSize = { 120, 9000 };
    SetConsoleScreenBufferSize(g_ConsoleHandle, bufferSize);

    SMALL_RECT windowSize = { 0, 0, 119, 30 };
    SetConsoleWindowInfo(g_ConsoleHandle, TRUE, &windowSize);
}

template<typename T>
std::string formatHex(T value) {
    std::stringstream ss;
    ss << "0x" << std::hex << std::uppercase << value;
    return ss.str();
}

template<typename T>
std::string formatPtr(T* ptr) {
    return formatHex(reinterpret_cast<uintptr_t>(ptr));
}

void Log(const std::string& message)
{
    // Write to console if enabled
    if (g_ConsoleEnabled && g_ConsoleHandle != NULL) {
        std::string consoleMsg = message + "\n";
        DWORD written;
        WriteConsoleA(g_ConsoleHandle, consoleMsg.c_str(), consoleMsg.length(), &written, NULL);
    }

    // Always write to log file
    std::ofstream logFile("./kcd2lua.log", std::ios_base::app);
    if (logFile.is_open())
    {
        logFile << message << std::endl;
        logFile.close();
    }
    else if (g_ConsoleEnabled && g_ConsoleHandle != NULL)
    {
        const char* error = "[ERROR] Could not open log file.\n";
        DWORD written;
        WriteConsoleA(g_ConsoleHandle, error, strlen(error), &written, NULL);
    }
}

template<typename... Args>
void LogFormat(const std::string& format, Args... args) {
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), format.c_str(), args...);
    Log(buffer);
}

lua_State* L = nullptr;
std::function<void()> on_tick = nullptr;
std::function<void()> on_init = nullptr;

std::string executeLuaCode(const std::string& luaCode)
{
    if (!L) return "Lua state not initialized.";

    if (luaL_dostring(L, luaCode.c_str()) != 0)
    {
        const char* errorMsg = lua_tostring(L, -1);
        std::string error = errorMsg ? errorMsg : "Unknown Lua error";
        Log("[ERROR] Lua execution failed: " + error);
        lua_pop(L, 1);
        return "Lua Error: " + error;
    }
    return "Lua executed successfully.";
}

SOCKET listenSocket = INVALID_SOCKET;

DWORD WINAPI TCPServerThread(LPVOID)
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        Log("[ERROR] Failed to initialize WinSock.");
        return 1;
    }

    listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET)
    {
        Log("[ERROR] Failed to create socket.");
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(TCP_PORT);
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

    if (bind(listenSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        Log("[ERROR] Failed to bind socket.");
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        Log("[ERROR] Failed to listen on socket.");
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    LogFormat("[INFO] TCP Server listening on 127.0.0.1:%d", TCP_PORT);

    while (true)
    {
        SOCKET clientSocket = accept(listenSocket, NULL, NULL);
        if (clientSocket == INVALID_SOCKET)
        {
            Log("[ERROR] Failed to accept connection.");
            continue;
        }

        Log("[INFO] Client connected!");

        while (true)
        {
            Log("[INFO] Waiting for message...");

            // Read message length (4 bytes)
            uint32_t messageLength = 0;
            int bytesReceived = recv(clientSocket, (char*)&messageLength, sizeof(messageLength), 0);

            if (bytesReceived <= 0) {
                LogFormat("[WARN] Client disconnected %d bytes received", bytesReceived);
                break;  // Connection closed or error
            }

            if (bytesReceived != sizeof(messageLength)) {
                Log("[ERROR] Failed to read message length");
                break;
            }

            LogFormat("[INFO] Expecting message of length %d", messageLength);

            // Read the complete message (already null-terminated)
            std::vector<char> buffer(messageLength);
            size_t totalReceived = 0;

            while (totalReceived < static_cast<size_t>(messageLength)) {
                bytesReceived = recv(clientSocket, buffer.data() + totalReceived,
                                   messageLength - totalReceived, 0);

                if (bytesReceived <= 0) {
                    Log("[ERROR] Connection error while reading message");
                    break;
                }

                totalReceived += bytesReceived;
            }

            if (totalReceived == static_cast<size_t>(messageLength)) {
                std::string result = executeLuaCode(buffer.data());
                result += "\n";
                send(clientSocket, result.c_str(), result.length(), 0);
            }

            Log("[INFO] Code executed");
        }

        closesocket(clientSocket);
        Log("[INFO] Client disconnected");
    }

    return 0;
}

namespace hooks
{
    typedef int32_t(__cdecl* luaL_loadbuffer_t)(lua_State* L, char* buff, size_t size, char* name);
    luaL_loadbuffer_t pluaL_loadbuffer = nullptr;

    int32_t luaL_loadbuffer_hook(lua_State* L, char* buff, size_t size, char* name)
    {
        return pluaL_loadbuffer(L, buff, size, name);
    }

    typedef int32_t(__cdecl* lua_pcall_t)(lua_State* L, int32_t nargs, int32_t nresults, int32_t errfunc);
    lua_pcall_t plua_pcall = nullptr;

    int32_t lua_pcall_hook(lua_State* L_, int32_t nargs, int32_t nresults, int32_t errfunc)
    {
        int32_t return_val = plua_pcall(L_, nargs, nresults, errfunc);
        if (L == nullptr)
        {
            Log("[LUA] Hooked!");
            L = L_;

            if (on_init != nullptr)
                on_init();
        }

        return return_val;
    }
}

bool VerifyAddress(void* addr, const char* name) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) {
        LogFormat("[ERROR] Failed to query memory for %s", name);
        return false;
    }

    if (mbi.State != MEM_COMMIT) {
        LogFormat("[ERROR] %s address is not committed memory", name);
        return false;
    }

    if (!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
        LogFormat("[ERROR] %s address is not executable", name);
        return false;
    }

    return true;
}

const Pattern PCALL_PATTERN = CreatePattern(PCALL_SIG);
const Pattern LOADBUFFER_PATTERN = CreatePattern(LOADBUFFER_SIG);
void hook()
{
    HMODULE hModule = GetModuleHandleW(L"WHGame.dll");
    if (!hModule) {
        Log("[ERROR] Failed to get WHGame.dll module handle");
        return;
    }

    MODULEINFO modInfo;
    if (!GetModuleInformation(GetCurrentProcess(), hModule, &modInfo, sizeof(modInfo))) {
        Log("[ERROR] Failed to get module information");
        return;
    }

    const uint8_t* moduleBase = static_cast<const uint8_t*>(modInfo.lpBaseOfDll);
    const size_t moduleSize = modInfo.SizeOfImage;

    LogFormat("[INFO] WHGame.dll base address: %s", formatPtr(moduleBase).c_str());

    uintptr_t pcall_offset = FindPattern(moduleBase, moduleSize, PCALL_PATTERN);
    uintptr_t loadbuffer_offset = FindPattern(moduleBase, moduleSize, LOADBUFFER_PATTERN);

    if (!pcall_offset || !loadbuffer_offset) {
        Log("[ERROR] Failed to find one or more patterns");
        if (!pcall_offset) Log("- Could not find lua_pcall pattern");
        if (!loadbuffer_offset) Log("- Could not find luaL_loadbuffer pattern");
        return;
    }

    LogFormat("[INFO] Found offsets - lua_pcall: %s, luaL_loadbuffer: %s",
        formatHex(pcall_offset).c_str(), formatHex(loadbuffer_offset).c_str());

    void* pcall_addr = (void*)(reinterpret_cast<uintptr_t>(moduleBase) + pcall_offset);
    void* loadbuffer_addr = (void*)(reinterpret_cast<uintptr_t>(moduleBase) + loadbuffer_offset);

    LogFormat("[INFO] Found lua_pcall at: %s", formatPtr(pcall_addr).c_str());
    LogFormat("[INFO] Found luaL_loadbuffer at: %s", formatPtr(loadbuffer_addr).c_str());

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        Log("[ERROR] Failed to initialize MinHook");
        return;
    }

    if (MH_CreateHook(pcall_addr,
                        reinterpret_cast<LPVOID>(&hooks::lua_pcall_hook),
                        reinterpret_cast<LPVOID*>(&hooks::plua_pcall)) != MH_OK) {
        Log("[ERROR] Failed to create lua_pcall hook");
        return;
    }

    if (MH_CreateHook(loadbuffer_addr,
                        reinterpret_cast<LPVOID>(&hooks::luaL_loadbuffer_hook),
                        reinterpret_cast<LPVOID*>(&hooks::pluaL_loadbuffer)) != MH_OK) {
        Log("[ERROR] Failed to create luaL_loadbuffer hook");
        return;
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        Log("[ERROR] Failed to enable hooks");
        return;
    }

    Log("[INFO] Hooks setup successfully");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
            Log("\n\n[INFO] Mod loaded");
            InitConsole();
            if (g_ConsoleEnabled) {
                Log("[INFO] Mod console initialized");
            }
            hook();
            CreateThread(nullptr, 0, TCPServerThread, nullptr, 0, nullptr);
            return 0;
        }, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (listenSocket != INVALID_SOCKET)
        {
            closesocket(listenSocket);
            WSACleanup();
        }
        if (g_ConsoleEnabled && g_ConsoleHandle != NULL) {
            FreeConsole();
            g_ConsoleHandle = NULL;
        }
    }
    return TRUE;
}
