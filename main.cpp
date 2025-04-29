#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>
#include <immintrin.h> // For AVX intrinsics
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "robin_hood.h" // Single header robin hood hash map
#include "concurrentqueue/concurrentqueue.h" // Lock-free queue for logging

#include <atomic>
#include <thread>
#include <chrono>
#include <array>
#include <algorithm>
#include <cmath>
#include <unordered_set> // Added missing include

// ===================================================
// Function prototypes
// ===================================================
typedef HRESULT(__stdcall* Present)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
typedef HRESULT(__stdcall* ResizeBuffers)(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
typedef void(__fastcall* CEntitySystem_Update)(void* thisPtr, void* edx);

// ===================================================
// Global DirectX State
// ===================================================
Present oPresent = nullptr;
ResizeBuffers oResizeBuffers = nullptr;
CEntitySystem_Update oCEntitySystem_Update = nullptr;
ID3D11Device* pDevice = nullptr;
ID3D11DeviceContext* pContext = nullptr;
ID3D11RenderTargetView* mainRenderTargetView = nullptr;
HWND gameWindow = nullptr;
bool initialized = false;

// ===================================================
// ESP Configuration
// ===================================================
struct ESPConfig {
    bool showNPCs = true;
    bool showDistance = true;
    bool showBoxes = false;
    bool showConfigWindow = true;
    int textScale = 100;
    float fieldOfViewDegrees = 90.0f;
};
ESPConfig config;

// ===================================================
// Math Structures
// ===================================================
struct Vec3 {
    double x, y, z;

    Vec3() : x(0), y(0), z(0) {}
    Vec3(double _x, double _y, double _z) : x(_x), y(_y), z(_z) {}

    double DistanceTo(const Vec3& other) const {
        double dx = x - other.x;
        double dy = y - other.y;
        double dz = z - other.z;
        return sqrt(dx*dx + dy*dy + dz*dz);
    }

    Vec3 operator-(const Vec3& other) const {
        return Vec3(x - other.x, y - other.y, z - other.z);
    }

    double Length() const {
        return sqrt(x*x + y*y + z*z);
    }

    Vec3 Normalized() const {
        double len = Length();
        if (len > 0.0) {
            return Vec3(x/len, y/len, z/len);
        }
        return *this;
    }
};

struct Vec2 {
    float x, y, z; // z depth value for visibility test
    bool success;  // Whether projection was successful

    Vec2() : x(0), y(0), z(0), success(false) {}
    Vec2(float _x, float _y, float _z = 0, bool _success = true) : x(_x), y(_y), z(_z), success(_success) {}

    bool IsValid(float maxWidth, float maxHeight) const {
        if (!success) return false;
        if (std::isnan(x) || std::isnan(y)) return false;
        if (x <= 0 || y <= 0) return false;
        if (x >= maxWidth || y >= maxHeight) return false;
        return true;
    }
};

struct Mat3 {
    double m[3][3];

    Mat3() {
        memset(m, 0, sizeof(m));
    }

    Vec3 MultiplyVec(const Vec3& v) const {
        Vec3 result;
        result.x = m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z;
        result.y = m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z;
        result.z = m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z;
        return result;
    }
};

struct Quaternion {
    float x, y, z, w;

    Quaternion() : x(0), y(0), z(0), w(1) {}

    Quaternion(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}

    // Rotate vector using quaternion (no matrix needed)
    Vec3 RotateVector(const Vec3& v) const {
        // Create vector part of quaternion
        Vec3 u(x, y, z);
        float s = w;

        // 2 * dot(u, v)
        float uvDot2 = 2.0f * (u.x * v.x + u.y * v.y + u.z * v.z);

        // 2 * s
        float s2 = 2.0f * s;

        // result = v + 2 * (s * cross(u, v) + dot(u, v) * u)
        return Vec3(
            v.x + s2 * (u.y * v.z - u.z * v.y) + uvDot2 * u.x,
            v.y + s2 * (u.z * v.x - u.x * v.z) + uvDot2 * u.y,
            v.z + s2 * (u.x * v.y - u.y * v.x) + uvDot2 * u.z
        );
    }
};

// ===================================================
// Optimized Logging System
// ===================================================
enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

class Logger {
private:
    moodycamel::ConcurrentQueue<std::pair<LogLevel, std::string>> logQueue;
    std::atomic<bool> loggingEnabled{false};
    std::atomic<bool> logThreadRunning{false};
    HANDLE logThread;
    thread_local static char formatBuffer[4096];

public:
    Logger() : logThread(NULL) {}

    void Initialize() {
        loggingEnabled.store(true, std::memory_order_release);
        logThreadRunning.store(true, std::memory_order_release);

        logThread = CreateThread(NULL, 0, [](LPVOID param) -> DWORD {
            Logger* logger = static_cast<Logger*>(param);

            HANDLE logFile = CreateFileA(
                "esp_overlay.log",
                GENERIC_WRITE,
                FILE_SHARE_READ,
                NULL,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                NULL
            );

            if (logFile == INVALID_HANDLE_VALUE) {
                return 1;
            }

            // Mark start of log
            const char* startMsg = "--- ESP Overlay Log Started ---\r\n";
            DWORD bytesWritten;
            WriteFile(logFile, startMsg, static_cast<DWORD>(strlen(startMsg)), &bytesWritten, NULL);

            const size_t BATCH_SIZE = 64;
            std::pair<LogLevel, std::string> logEntries[BATCH_SIZE];
            std::string buffer;
            buffer.reserve(8192);

            while (logger->logThreadRunning.load(std::memory_order_acquire)) {
                size_t count = logger->logQueue.try_dequeue_bulk(logEntries, BATCH_SIZE);

                if (count > 0 && logger->loggingEnabled.load(std::memory_order_acquire)) {
                    buffer.clear();

                    // Format all entries in batch
                    for (size_t i = 0; i < count; i++) {
                        // Get timestamp
                        SYSTEMTIME st;
                        GetLocalTime(&st);

                        // Format log entry
                        char timeStamp[32];
                        snprintf(timeStamp, sizeof(timeStamp), "[%02d:%02d:%02d.%03d] ",
                                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

                        buffer.append(timeStamp);

                        // Add level prefix
                        switch (logEntries[i].first) {
                            case LogLevel::Debug:   buffer.append("[DBG] "); break;
                            case LogLevel::Info:    buffer.append("[INF] "); break;
                            case LogLevel::Warning: buffer.append("[WRN] "); break;
                            case LogLevel::Error:   buffer.append("[ERR] "); break;
                        }

                        // Add message and newline
                        buffer.append(logEntries[i].second);
                        buffer.append("\r\n");
                    }

                    // Write entire batch at once
                    WriteFile(
                        logFile,
                        buffer.c_str(),
                        static_cast<DWORD>(buffer.size()),
                        &bytesWritten,
                        NULL
                    );

                    // Flush to disk periodically
                    FlushFileBuffers(logFile);
                }

                // Wait a bit if no messages or after processing
                Sleep(count > 0 ? 1 : 200);
            }

            // Mark end of log
            const char* endMsg = "--- ESP Overlay Log Ended ---\r\n";
            WriteFile(logFile, endMsg, static_cast<DWORD>(strlen(endMsg)), &bytesWritten, NULL);

            CloseHandle(logFile);
            return 0;
        }, this, 0, NULL);

        // Set thread priority to low
        SetThreadPriority(logThread, THREAD_PRIORITY_BELOW_NORMAL);
    }

    void Shutdown() {
        logThreadRunning.store(false, std::memory_order_release);

        if (logThread != NULL) {
            // Wait for thread termination with timeout
            if (WaitForSingleObject(logThread, 1000) == WAIT_TIMEOUT) {
                TerminateThread(logThread, 1);
            }

            CloseHandle(logThread);
            logThread = NULL;
        }
    }

    void Log(LogLevel level, const std::string& message) {
        // Skip if logging disabled
        if (!loggingEnabled.load(std::memory_order_acquire)) return;

        // Enqueue message (non-blocking)
        logQueue.enqueue(std::make_pair(level, message));
    }

    // Format string and log
    template<typename... Args>
    void LogFormat(LogLevel level, const char* format, Args&&... args) {
        // Skip if logging disabled
        if (!loggingEnabled.load(std::memory_order_acquire)) return;

        // Format string using snprintf
        int len = snprintf(formatBuffer, sizeof(formatBuffer), format, std::forward<Args>(args)...);

        // Enqueue if formatting succeeded
        if (len > 0) {
            logQueue.enqueue(std::make_pair(level, std::string(formatBuffer, len)));
        }
    }

    void Debug(const std::string& message) { Log(LogLevel::Debug, message); }
    void Info(const std::string& message) { Log(LogLevel::Info, message); }
    void Warning(const std::string& message) { Log(LogLevel::Warning, message); }
    void Error(const std::string& message) { Log(LogLevel::Error, message); }

    template<typename... Args>
    void DebugFormat(const char* format, Args&&... args) {
        LogFormat(LogLevel::Debug, format, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void InfoFormat(const char* format, Args&&... args) {
        LogFormat(LogLevel::Info, format, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void WarningFormat(const char* format, Args&&... args) {
        LogFormat(LogLevel::Warning, format, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void ErrorFormat(const char* format, Args&&... args) {
        LogFormat(LogLevel::Error, format, std::forward<Args>(args)...);
    }
};

// Initialize static member
thread_local char Logger::formatBuffer[4096];
Logger gLogger;

// ===================================================
// Game Structures Forward Declarations
// ===================================================
class CEntityClass;
class CEntity;
class CEntityArray;
class CEntitySystem;
class CRenderer;
class CSystem;
class GEnv;

// ===================================================
// Utility Functions
// ===================================================
namespace Utils {
    constexpr uintptr_t MASK_LOWER_48 = 0xFFFFFFFFFFFFULL;

    inline uintptr_t MaskPointer(uintptr_t ptr) noexcept {
        return ptr & MASK_LOWER_48;
    }

    bool IsValidPtr(const void* ptr, bool checkRead = true) {
        if (!ptr) return false;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (checkRead && (mbi.Protect & (
            PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) return false;
        if (mbi.Protect & PAGE_GUARD || mbi.Protect & PAGE_NOACCESS) return false;
        return true;
    }

    bool ReadCString(uintptr_t address, char* outBuffer, size_t bufferSize) {
        if (bufferSize == 0) return false;
        if (!IsValidPtr(reinterpret_cast<const void*>(address))) return false;

        // Initialize first byte to ensure null termination
        outBuffer[0] = '\0';

        try {
            for (size_t i = 0; i < bufferSize - 1; ++i) {
                uintptr_t charAddr = address + i;
                if (!IsValidPtr(reinterpret_cast<const void*>(charAddr))) {
                    outBuffer[0] = '\0';
                    return false;
                }

                char c = *reinterpret_cast<const char*>(charAddr);
                outBuffer[i] = c;

                if (c == '\0') return true;
            }

            // Ensure null termination if string exceeds buffer
            outBuffer[bufferSize - 1] = '\0';
            return true;
        }
        catch (...) {
            outBuffer[0] = '\0';
            return false;
        }
    }

    uintptr_t GetGameModuleBase(const char* moduleName, int maxRetries = 10, int retryDelayMs = 1000) {
        uintptr_t base = 0;
        for (int i = 0; i < maxRetries && base == 0; ++i) {
            base = (uintptr_t)GetModuleHandleA(moduleName);
            if (base == 0) {
                gLogger.WarningFormat("Module '%s' not found, retrying (%d/%d)...",
                                     moduleName, (i + 1), maxRetries);
                Sleep(retryDelayMs);
            }
        }

        if (base != 0) {
            gLogger.InfoFormat("Found module %s at 0x%llX", moduleName, base);
        } else {
            gLogger.ErrorFormat("Failed to find module %s after %d attempts", moduleName, maxRetries);
        }

        return base;
    }

    template<typename FuncType, typename InstanceType, typename... Args>
    auto CallVFunc(InstanceType* instance, int index, Args... args)
        -> decltype(std::declval<FuncType>()(instance, args...)) {
        void* voidInstance = reinterpret_cast<void*>(instance);
        if (!IsValidPtr(voidInstance, false)) {
            throw std::runtime_error("CallVFunc: Invalid instance pointer");
        }

        uintptr_t* vtable = *reinterpret_cast<uintptr_t**>(voidInstance);
        if (!IsValidPtr(vtable, true)) {
            throw std::runtime_error("CallVFunc: Invalid VTable pointer");
        }

        if (index < 0 || index > 500) {
            throw std::runtime_error("CallVFunc: VTable index out of reasonable bounds");
        }

        uintptr_t funcAddr = vtable[index];
        if (!IsValidPtr(reinterpret_cast<void*>(funcAddr), false)) {
            throw std::runtime_error("CallVFunc: Invalid function address");
        }

        FuncType func = reinterpret_cast<FuncType>(funcAddr);
        return func(instance, args...);
    }

    // FNV-1a hash implementation
    constexpr uint32_t FNV1A_BASIS = 0x811c9dc5;
    constexpr uint32_t FNV1A_PRIME = 0x01000193;

    uint32_t fnv1a(const char* str) {
        uint32_t hash = FNV1A_BASIS;
        while (*str) {
            hash ^= static_cast<uint32_t>(*str++);
            hash *= FNV1A_PRIME;
        }
        return hash;
    }

    uint32_t fnv1a(uintptr_t ptr) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&ptr);
        uint32_t hash = FNV1A_BASIS;
        for (size_t i = 0; i < sizeof(uintptr_t); i++) {
            hash ^= static_cast<uint32_t>(bytes[i]);
            hash *= FNV1A_PRIME;
        }
        return hash;
    }
}

// ===================================================
// Game Offsets
// ===================================================
namespace GameOffsets {
    // Base Offsets
    const uintptr_t GEnvOffset = 0x981D200; // Offset from module base to GEnv* pointer
    const uintptr_t CEntitySystem_UpdateOffset = 0x69060B0; // Offset for CEntitySystem::Update

    // Offsets relative to GEnv*
    const uintptr_t GEnv_EntitySystemOffset = 0x00A0;
    const uintptr_t GEnv_RendererOffset = 0x00F8; // Offset to CRenderer in GEnv
    const uintptr_t GEnv_SystemOffset = 0x00C0;

    // Offsets relative to CEntitySystem*
    const uintptr_t EntitySystem_ClassRegistryOffset = 0x06D8;
    const uintptr_t EntitySystem_EntityArrayOffset = 0x0118;

    const int EntitySystem_VTableIndex_GetEntityClass = 4;
    const int EntitySystem_VTableIndex_GetClassRegistry = 24;

    // Offsets relative to CEntityArray*
    const uintptr_t EntityArray_MaxSizeOffset = 0x0000;
    const uintptr_t EntityArray_CurrSizeOffset = 0x0008;
    const uintptr_t EntityArray_DataOffset = 0x0018;

    // Offsets relative to CEntityClass*
    const uintptr_t EntityClass_FlagsOffset = 0x0008; // int64_t flags
    const uintptr_t EntityClass_NameOffset = 0x0010;  // char* name

    // Offsets relative to CEntity*
    const uintptr_t Entity_FlagsOffset = 0x0008; // int64_t flags
    const uintptr_t Entity_IdOffset = 0x0010; // uint64_t entityId
    const uintptr_t Entity_ClassTypePtrOffset = 0x0020; // void* classTypePtr
    const uintptr_t Entity_PositionOffset = 0x00F0; // double x,y,z (3 consecutive doubles)
    const uintptr_t Entity_NameOffset = 0x0290; // char* entityName

    // VTable indices
    const int Entity_VTableIndex_GetWorldPos = 88;
    const int Renderer_VTableIndex_ProjectToScreen = 66;
}

// ===================================================
// Game Structure Implementations
// ===================================================
// Wrapper for CEntityClass
class CEntityClass {
private:
    uintptr_t ptr;

public:
    CEntityClass(uintptr_t address) : ptr(address) {}

    uintptr_t GetPtr() const { return ptr; }

    int64_t GetFlags() const {
        return *reinterpret_cast<int64_t*>(ptr + GameOffsets::EntityClass_FlagsOffset);
    }

    const char* GetNamePtr() const {
        return *reinterpret_cast<const char**>(ptr + GameOffsets::EntityClass_NameOffset);
    }

    bool GetName(char* outBuffer, size_t bufferSize) const {
        uintptr_t namePtr = *reinterpret_cast<uintptr_t*>(ptr + GameOffsets::EntityClass_NameOffset);
        return Utils::ReadCString(namePtr, outBuffer, bufferSize);
    }
};

// Wrapper for CEntity
class CEntity {
private:
    uintptr_t ptr;

public:
    CEntity(uintptr_t address) : ptr(address) {}

    uintptr_t GetPtr() const { return ptr; }

    int64_t GetFlags() const {
        return *reinterpret_cast<int64_t*>(ptr + GameOffsets::Entity_FlagsOffset);
    }

    int64_t GetId() const {
        return *reinterpret_cast<int64_t*>(ptr + GameOffsets::Entity_IdOffset);
    }

    uintptr_t GetEntityClassPtr() const {
        uintptr_t raw = *reinterpret_cast<uintptr_t*>(ptr + GameOffsets::Entity_ClassTypePtrOffset);
        return Utils::MaskPointer(raw);
    }

    CEntityClass GetEntityClass() const {
        return CEntityClass(GetEntityClassPtr());
    }

    Vec3 GetWorldPos() const {
        Vec3 outPos = {0, 0, 0};
        try {
            typedef Vec3* (*GetWorldPosFunc)(void* pEntity, Vec3* outPos);
            Utils::CallVFunc<GetWorldPosFunc>(
                reinterpret_cast<void*>(ptr),
                GameOffsets::Entity_VTableIndex_GetWorldPos,
                &outPos
            );
        } catch (const std::exception& e) {
            gLogger.ErrorFormat("Exception in GetWorldPos: %s", e.what());
        }
        return outPos;
    }

    const char* GetNamePtr() const {
        return *reinterpret_cast<const char**>(ptr + GameOffsets::Entity_NameOffset);
    }

    bool GetName(char* outBuffer, size_t bufferSize) const {
        uintptr_t namePtr = *reinterpret_cast<uintptr_t*>(ptr + GameOffsets::Entity_NameOffset);
        return Utils::ReadCString(namePtr, outBuffer, bufferSize);
    }

    bool IsValid() const {
        return Utils::IsValidPtr(reinterpret_cast<const void*>(ptr)) &&
               Utils::IsValidPtr(reinterpret_cast<const void*>(GetEntityClassPtr()));
    }
};

// Wrapper for CEntityArray<T> where T = CEntity*
class CEntityArray {
private:
    uintptr_t ptr;

public:
    CEntityArray(uintptr_t address) : ptr(address) {}

    uintptr_t GetPtr() const { return ptr; }

    int64_t GetMaxSize() const {
        return *reinterpret_cast<int64_t*>(ptr + GameOffsets::EntityArray_MaxSizeOffset);
    }

    int64_t GetCurrSize() const {
        return *reinterpret_cast<int64_t*>(ptr + GameOffsets::EntityArray_CurrSizeOffset);
    }

    uintptr_t GetDataPtr() const {
        return *reinterpret_cast<uintptr_t*>(ptr + GameOffsets::EntityArray_DataOffset);
    }

    uintptr_t GetEntityPtr(int64_t i) const {
        if (i < 0 || i >= GetMaxSize()) return 0;

        uintptr_t* data = reinterpret_cast<uintptr_t*>(GetDataPtr());
        if (!Utils::IsValidPtr(data)) return 0;

        uintptr_t maskedIndex = i & (GetMaxSize() - 1);
        uintptr_t elementPtr = data[maskedIndex];
        if (!Utils::IsValidPtr(reinterpret_cast<void*>(elementPtr))) return 0;

        return Utils::MaskPointer(elementPtr);
    }

    CEntity At(int64_t i) const {
        return CEntity(GetEntityPtr(i));
    }
};

// Wrapper for CEntitySystem
class CEntitySystem {
private:
    uintptr_t ptr;

public:
    CEntitySystem(uintptr_t address) : ptr(address) {}

    uintptr_t GetPtr() const { return ptr; }

    CEntityArray GetEntityArray() const {
        return CEntityArray(ptr + GameOffsets::EntitySystem_EntityArrayOffset);
    }
};

// Wrapper for CRenderer
class CRenderer {
private:
    uintptr_t ptr;

public:
    CRenderer(uintptr_t address) : ptr(address) {}

    uintptr_t GetPtr() const { return ptr; }

    Vec2 ProjectToScreen(const Vec3& pos, const Vec2& resolution = Vec2(1920.0f, 1080.0f)) const {
        if (!Utils::IsValidPtr(reinterpret_cast<const void*>(ptr))) {
            return Vec2(0, 0, 0, false);
        }

        try {
            float outX = 0.0f, outY = 0.0f, outZ = 0.0f;

            typedef bool (*ProjectToScreenFunc)(void* renderer, double x, double y, double z,
                                               float* outX, float* outY, float* outZ,
                                               bool isPlayerViewportRelative, int64_t unused);

            bool result = Utils::CallVFunc<ProjectToScreenFunc>(
                reinterpret_cast<void*>(ptr),
                GameOffsets::Renderer_VTableIndex_ProjectToScreen,
                pos.x, pos.y, pos.z,
                &outX, &outY, &outZ,
                true,
                0
            );

            if (result && outZ > 0.0f) {
                float screenX = outX * (resolution.x * 0.01f);
                float screenY = outY * (resolution.y * 0.01f);

                return Vec2(screenX, screenY, outZ, true);
            }

            return Vec2(0, 0, 0, false);
        } catch (const std::exception& e) {
            gLogger.ErrorFormat("Exception in ProjectToScreen: %s", e.what());
            return Vec2(0, 0, 0, false);
        }
    }
};

class CSystem {
private:
    uintptr_t ptr;

public:
    CSystem(uintptr_t address) : ptr(address) {}

    uintptr_t GetPtr() const { return ptr; }

    // Camera forward vector
    Vec3 GetCameraForward() const {
        return Vec3(
            *reinterpret_cast<double*>(ptr + 0x30),
            *reinterpret_cast<double*>(ptr + 0x50),
            *reinterpret_cast<double*>(ptr + 0x70)
        );
    }

    // Camera up vector
    Vec3 GetCameraUp() const {
        return Vec3(
            *reinterpret_cast<double*>(ptr + 0x38),
            *reinterpret_cast<double*>(ptr + 0x58),
            *reinterpret_cast<double*>(ptr + 0x78)
        );
    }

    // Camera world position
    Vec3 GetCameraWorldPos() const {
        return Vec3(
            *reinterpret_cast<double*>(ptr + 0x40),
            *reinterpret_cast<double*>(ptr + 0x60),
            *reinterpret_cast<double*>(ptr + 0x80)
        );
    }

    // Internal FOV (X-axis)
    float GetInternalXFOV() const {
        return *reinterpret_cast<float*>(ptr + 0x118);
    }

    // Calculate view matrix from forward and up vectors
    Mat3 CalculateViewMatrix() const {
        Vec3 forward = GetCameraForward();
        Vec3 up = GetCameraUp();

        // Normalize vectors
        double forwardMag = sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
        forward.x /= forwardMag;
        forward.y /= forwardMag;
        forward.z /= forwardMag;

        double upMag = sqrt(up.x * up.x + up.y * up.y + up.z * up.z);
        up.x /= upMag;
        up.y /= upMag;
        up.z /= upMag;

        // Calculate right vector (cross product of up and forward)
        Vec3 right;
        right.x = up.y * forward.z - up.z * forward.y;
        right.y = up.z * forward.x - up.x * forward.z;
        right.z = up.x * forward.y - up.y * forward.x;

        // Construct view matrix
        Mat3 viewMatrix;

        // Right vector
        viewMatrix.m[0][0] = right.x;
        viewMatrix.m[0][1] = right.y;
        viewMatrix.m[0][2] = right.z;

        // Up vector
        viewMatrix.m[1][0] = up.x;
        viewMatrix.m[1][1] = up.y;
        viewMatrix.m[1][2] = up.z;

        // Forward vector (negated for view direction)
        viewMatrix.m[2][0] = -forward.x;
        viewMatrix.m[2][1] = -forward.y;
        viewMatrix.m[2][2] = -forward.z;

        return viewMatrix;
    }

    // Get quaternion from forward/up vectors
    Quaternion GetCameraQuaternion() const {
        Vec3 forward = GetCameraForward().Normalized();
        Vec3 up = GetCameraUp().Normalized();

        // Calculate right vector (cross product of up and forward)
        Vec3 right(
            up.y * forward.z - up.z * forward.y,
            up.z * forward.x - up.x * forward.z,
            up.x * forward.y - up.y * forward.x
        );

        // Normalize right vector
        double rightLen = right.Length();
        right.x /= rightLen;
        right.y /= rightLen;
        right.z /= rightLen;

        // Recalculate up vector to ensure orthogonality
        Vec3 normUp(
            forward.y * right.z - forward.z * right.y,
            forward.z * right.x - forward.x * right.z,
            forward.x * right.y - forward.y * right.x
        );

        // Build rotation matrix from orthogonal basis
        float m00 = static_cast<float>(right.x);
        float m01 = static_cast<float>(right.y);
        float m02 = static_cast<float>(right.z);
        float m10 = static_cast<float>(normUp.x);
        float m11 = static_cast<float>(normUp.y);
        float m12 = static_cast<float>(normUp.z);
        float m20 = static_cast<float>(-forward.x);
        float m21 = static_cast<float>(-forward.y);
        float m22 = static_cast<float>(-forward.z);

        // Convert rotation matrix to quaternion
        float tr = m00 + m11 + m22;

        Quaternion q;

        if (tr > 0) {
            float S = sqrt(tr + 1.0f) * 2.0f;
            q.w = 0.25f * S;
            q.x = (m12 - m21) / S;
            q.y = (m20 - m02) / S;
            q.z = (m01 - m10) / S;
        } else if ((m00 > m11) && (m00 > m22)) {
            float S = sqrt(1.0f + m00 - m11 - m22) * 2.0f;
            q.w = (m12 - m21) / S;
            q.x = 0.25f * S;
            q.y = (m01 + m10) / S;
            q.z = (m20 + m02) / S;
        } else if (m11 > m22) {
            float S = sqrt(1.0f + m11 - m00 - m22) * 2.0f;
            q.w = (m20 - m02) / S;
            q.x = (m01 + m10) / S;
            q.y = 0.25f * S;
            q.z = (m12 + m21) / S;
        } else {
            float S = sqrt(1.0f + m22 - m00 - m11) * 2.0f;
            q.w = (m01 - m10) / S;
            q.x = (m20 + m02) / S;
            q.y = (m12 + m21) / S;
            q.z = 0.25f * S;
        }

        return q;
    }
};

// Wrapper for GEnv
class GEnv {
private:
    uintptr_t ptr;

public:
    GEnv(uintptr_t address) : ptr(address) {}

    uintptr_t GetPtr() const { return ptr; }

    CEntitySystem GetEntitySystem() const {
        uintptr_t sysPtr = *reinterpret_cast<uintptr_t*>(ptr + GameOffsets::GEnv_EntitySystemOffset);
        if (!Utils::IsValidPtr(reinterpret_cast<void*>(sysPtr)))
            return CEntitySystem(0);
        return CEntitySystem(sysPtr);
    }

    CRenderer GetRenderer() const {
        uintptr_t rendererPtr = *reinterpret_cast<uintptr_t*>(ptr + GameOffsets::GEnv_RendererOffset);
        if (!Utils::IsValidPtr(reinterpret_cast<void*>(rendererPtr)))
            return CRenderer(0);
        return CRenderer(rendererPtr);
    }

    CSystem GetSystem() const {
        uintptr_t systemPtr = *reinterpret_cast<uintptr_t*>(ptr + GameOffsets::GEnv_SystemOffset);
        if (!Utils::IsValidPtr(reinterpret_cast<void*>(systemPtr)))
            return CSystem(0);
        return CSystem(systemPtr);
    }
};

// ===================================================
// Entity Registry - Optimized Lock-Free Implementation
// ===================================================
// Static and dynamic entity data structures
struct EntityStatic {
    uintptr_t entityPtr;          // 48-bit masked pointer
    uintptr_t classPtr;           // pointer to entity class
    uint32_t  classHash;          // FNV-1a hash of class name
    bool      isActor;            // Player / NPC_* flag
    char      name[64];           // cached name (fixed-length buffer)
};

struct EntityDynamic {
    Vec3  position;               // World position
    Vec2  screenPos;              // Projected screen position (valid if onscreen)
    float distance;               // Distance from camera
    uint32_t lastSeenGeneration;  // Generation counter for invalidation
};

// Complete entity record
struct EntityRecord {
    EntityStatic st;
    EntityDynamic dyn;
};

// Double-buffered registry using robin_hood hash map
class DoubleBufferedRegistry {
private:
    robin_hood::unordered_flat_map<uintptr_t, EntityRecord> buffers[2];
    std::atomic<uint32_t> active{0};

public:
    DoubleBufferedRegistry() {
        // Pre-reserve capacity for better performance
        buffers[0].reserve(1000);
        buffers[1].reserve(1000);
    }

    // Get read buffer - thread safe, lock-free
    const robin_hood::unordered_flat_map<uintptr_t, EntityRecord>& read() const noexcept {
        return buffers[active.load(std::memory_order_acquire)];
    }

    // Get write buffer - only used by scanner thread
    robin_hood::unordered_flat_map<uintptr_t, EntityRecord>& write() noexcept {
        return buffers[1 ^ active.load(std::memory_order_acquire)];
    }

    // Publish updates - atomic swap of buffers
    void publish() noexcept {
        active.store(1 ^ active.load(std::memory_order_acquire), std::memory_order_release);
        write().clear(); // Reset the new write buffer
    }
};

// ===================================================
// Global State
// ===================================================
namespace Globals {
    HMODULE hModule = nullptr;
    uintptr_t moduleBase = 0;

    // Screen resolution cache
    struct ScreenResolution {
        float width = 1920.0f;
        float height = 1080.0f;
    };
    ScreenResolution screenResolution;

    // Actor class management
    std::unordered_set<std::string> actorClassNames;
    std::mutex actorClassNamesMutex;

    // Scanner thread state
    std::atomic<uintptr_t> latestEntitySys{0};
    std::atomic<uint64_t> lastTick{0};
    std::atomic<bool> scannerRunning{false};
    HANDLE scannerWakeEvent = NULL;
    HANDLE scannerThread = NULL;

    // Entity registry
    DoubleBufferedRegistry entityRegistry;
    std::atomic<uint32_t> currentGeneration{1};

    // Camera state cache
    Vec3 cameraPosition{0, 0, 0};
    Quaternion cameraRotation;
    float cameraFOV = 90.0f;

    // Statistics
    std::atomic<size_t> entityCount{0};
    std::atomic<size_t> actorCount{0};
    std::atomic<size_t> frameCount{0};

    // Add known actor class
    void AddActorClass(const std::string& className) {
        std::lock_guard<std::mutex> lock(actorClassNamesMutex);
        actorClassNames.insert(className);
    }

    // Check if class is an actor type
    bool IsActorClass(const std::string& className) {
        std::lock_guard<std::mutex> lock(actorClassNamesMutex);
        return actorClassNames.count(className) > 0 ||
               className == "Player" ||
               className.find("NPC_") == 0;
    }

    // Clear actor class list
    void ClearActorClasses() {
        std::lock_guard<std::mutex> lock(actorClassNamesMutex);
        actorClassNames.clear();
    }

    // Get GEnv pointer
    uintptr_t GetGEnvPtr() {
        if (moduleBase == 0) return 0;
        uintptr_t gEnvPtrAddr = moduleBase + GameOffsets::GEnvOffset;
        if (!Utils::IsValidPtr(reinterpret_cast<void*>(gEnvPtrAddr))) {
            return 0;
        }
        return gEnvPtrAddr;
    }

    // Update camera info from system
    void UpdateCameraInfo() {
        uintptr_t gEnvPtr = GetGEnvPtr();
        if (gEnvPtr != 0) {
            GEnv gEnv(gEnvPtr);
            CSystem system = gEnv.GetSystem();

            if (system.GetPtr() != 0) {
                cameraPosition = system.GetCameraWorldPos();
                cameraRotation = system.GetCameraQuaternion();
                cameraFOV = system.GetInternalXFOV();
            }
        }
    }
}

// ===================================================
// Projection Functions
// ===================================================
// Basic world-to-screen projection using matrix
Vec2 WorldToScreen(const Vec3& worldPos, const CSystem& system, float screenW, float screenH) {
    Vec2 result(0, 0, 0, false);

    // Get camera position
    Vec3 cameraPos = system.GetCameraWorldPos();

    // Vector from camera to target
    Vec3 toTarget;
    toTarget.x = worldPos.x - cameraPos.x;
    toTarget.y = worldPos.y - cameraPos.y;
    toTarget.z = worldPos.z - cameraPos.z;

    // Get view matrix from camera
    Mat3 viewMatrix = system.CalculateViewMatrix();

    // Transform to camera space
    Vec3 cam = viewMatrix.MultiplyVec(toTarget);

    // Check if behind camera
    if (cam.z >= 0.0) {
        return result;
    }

    // Calculate FOV
    float fovX = system.GetInternalXFOV();
    if (fovX <= 0.0f) fovX = config.fieldOfViewDegrees * (3.14159f / 180.0f);

    // Calculate projection parameters
    float aspect = screenW / screenH;
    float fx = 1.0f / tanf(fovX * 0.5f);
    float fy = fx / aspect;

    // Project to NDC (normalized device coordinates)
    float xn = (fx * (float)cam.x) / -(float)cam.z;
    float yn = (fy * (float)cam.y) / -(float)cam.z;

    // Convert to screen coordinates
    result.x = (xn + 1.0f) * 0.5f * screenW;
    result.y = (1.0f - (yn + 1.0f) * 0.5f) * screenH;
    result.z = -(float)cam.z;

    // Set visibility
    result.success = (xn >= -1.0f && xn <= 1.0f && yn >= -1.0f && yn <= 1.0f);

    return result;
}

// SIMD-optimized world-to-screen transform (x64 only)
Vec2 WorldToScreenSIMD(const Vec3& worldPos, const Vec3& cameraPos, const Quaternion& cameraRotation,
                      float fovX, float screenW, float screenH) {
    Vec2 result(0, 0, 0, false);

    // Load camera position into AVX register
    __m256d camPosVec = _mm256_setr_pd(cameraPos.x, cameraPos.y, cameraPos.z, 0.0);

    // Load world position into AVX register
    __m256d worldPosVec = _mm256_setr_pd(worldPos.x, worldPos.y, worldPos.z, 0.0);

    // Calculate vector from camera to target (4 coordinates at once)
    __m256d toTargetVec = _mm256_sub_pd(worldPosVec, camPosVec);

    // Extract vector components
    double toTarget[4];
    _mm256_storeu_pd(toTarget, toTargetVec);

    // Create vector for rotation
    Vec3 toTargetVec3(toTarget[0], toTarget[1], toTarget[2]);

    // Apply quaternion rotation to get view-space position
    Vec3 viewSpace = cameraRotation.RotateVector(toTargetVec3);

    // Check if behind camera (-Z is forward in view space)
    if (viewSpace.z >= 0.0) {
        return result;
    }

    // Calculate projection parameters
    float aspect = screenW / screenH;
    float fx = 1.0f / tanf(fovX * 0.5f);
    float fy = fx / aspect;

    // Project to screen
    float xn = (fx * (float)viewSpace.x) / -(float)viewSpace.z;
    float yn = (fy * (float)viewSpace.y) / -(float)viewSpace.z;

    // Convert to screen coordinates
    result.x = (xn + 1.0f) * 0.5f * screenW;
    result.y = (1.0f - (yn + 1.0f) * 0.5f) * screenH;
    result.z = -(float)viewSpace.z;

    // Set visibility flag
    result.success = (xn >= -1.0f && xn <= 1.0f && yn >= -1.0f && yn <= 1.0f);

    return result;
}

// Quaternion-based world to screen projection
Vec2 WorldToScreenQuaternion(const Vec3& worldPos, const Vec3& cameraPos,
                            const Quaternion& cameraRotation, float fovX,
                            float screenW, float screenH) {
    Vec2 result(0, 0, 0, false);

    // Vector from camera to target
    Vec3 toTarget(
        worldPos.x - cameraPos.x,
        worldPos.y - cameraPos.y,
        worldPos.z - cameraPos.z
    );

    // Transform vector using quaternion
    Vec3 viewVec = cameraRotation.RotateVector(toTarget);

    // Inverted Z (looking down -Z axis)
    double camX = viewVec.x;
    double camY = viewVec.y;
    double camZ = -viewVec.z;

    // Check if behind camera
    if (camZ <= 0.0) {
        return result;
    }

    // Calculate projection parameters
    float aspect = screenW / screenH;
    float fx = 1.0f / tanf(fovX * 0.5f);
    float fy = fx / aspect;

    // Project to screen
    float xn = (fx * (float)camX) / (float)camZ;
    float yn = (fy * (float)camY) / (float)camZ;

    // Convert to screen coordinates
    result.x = (xn + 1.0f) * 0.5f * screenW;
    result.y = (1.0f - (yn + 1.0f) * 0.5f) * screenH;
    result.z = (float)camZ;

    // Set visibility flag
    result.success = (xn >= -1.0f && xn <= 1.0f && yn >= -1.0f && yn <= 1.0f);

    return result;
}

// ===================================================
// Entity Scanner Implementation
// ===================================================
// Scan entity array and update registry
void ScanEntityArray(const CEntitySystem& es, uint32_t generation) {
    // Measure scan start time
    auto scanStart = std::chrono::high_resolution_clock::now();

    // Get write buffer for updates
    auto& regWrite = Globals::entityRegistry.write();

    // Get entity array once
    CEntityArray arr = es.GetEntityArray();
    const int64_t maxSize = arr.GetMaxSize();

    // Local counters for statistics
    size_t totalEntities = 0;
    size_t actorEntities = 0;

    // Cache camera position for distance calculations
    Vec3 camPos = Globals::cameraPosition;

    // Process all entities
    for (int64_t i = 0; i < maxSize; ++i) {
        // Get entity pointer directly
        uintptr_t entityPtr = arr.GetEntityPtr(i);
        if (entityPtr == 0) continue;

        CEntity entity(entityPtr);
        if (!entity.IsValid()) continue;

        totalEntities++;

        // Generate entity key
        uintptr_t key = entity.GetPtr();
        auto it = regWrite.find(key);

        EntityRecord* rec;
        if (it == regWrite.end()) {
            // First time seeing this entity - slow path
            EntityStatic st;
            st.entityPtr = key;
            st.classPtr = entity.GetEntityClassPtr();

            // Get class name for hashing
            CEntityClass entityClass = entity.GetEntityClass();
            char className[64] = {0};
            if (entityClass.GetName(className, sizeof(className))) {
                st.classHash = Utils::fnv1a(className);
                st.isActor = Globals::IsActorClass(className);
            } else {
                st.classHash = 0;
                st.isActor = false;
            }

            // Get entity name
            entity.GetName(st.name, sizeof(st.name));

            // Insert into registry
            auto result = regWrite.emplace(key, EntityRecord{st, {}});
            rec = &result.first->second;

            if (st.isActor) {
                actorEntities++;
            }
        } else {
            // Entity already in registry - fast path
            rec = &it->second;

            if (rec->st.isActor) {
                actorEntities++;
            }
        }

        // Always update dynamic fields (cheap)
        Vec3 pos = entity.GetWorldPos();
        rec->dyn.position = pos;
        rec->dyn.distance = static_cast<float>(pos.DistanceTo(camPos));
        rec->dyn.lastSeenGeneration = generation;

        // Project to screen using cached camera data
        rec->dyn.screenPos = WorldToScreenQuaternion(
            pos,
            camPos,
            Globals::cameraRotation,
            Globals::cameraFOV,
            Globals::screenResolution.width,
            Globals::screenResolution.height
        );
    }

    // Prune entities that weren't seen in this scan
    for (auto it = regWrite.begin(); it != regWrite.end();) {
        if (it->second.dyn.lastSeenGeneration != generation) {
            it = regWrite.erase(it);
        } else {
            ++it;
        }
    }

    // Publish updates - atomic buffer swap
    Globals::entityRegistry.publish();

    // Update global statistics
    Globals::entityCount.store(totalEntities, std::memory_order_release);
    Globals::actorCount.store(actorEntities, std::memory_order_release);

    // Log periodic scan statistics
    static uint32_t scanCount = 0;
    if (++scanCount % 300 == 0) {
        auto scanEnd = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(scanEnd - scanStart).count();

        gLogger.InfoFormat("Scan #%u: %zu entities (%zu actors) in %.3f ms",
                          scanCount, totalEntities, actorEntities, duration / 1000.0f);
    }
}

// Scanner thread procedure
DWORD WINAPI ScannerThreadProc(LPVOID /*lpParameter*/) {
    // Set thread name for debugging
    SetThreadDescription(GetCurrentThread(), L"ESP Scanner Thread");

    // Set high thread priority for responsive scanning
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    gLogger.Info("Scanner thread started");

    // Scanning configuration
    uint32_t generation = 1;

    while (Globals::scannerRunning.load(std::memory_order_acquire)) {
        // Wait for event signal or 16ms timeout (approx. 60Hz max)
        WaitForSingleObject(Globals::scannerWakeEvent, 16);

        if (!Globals::scannerRunning.load(std::memory_order_acquire))
            break;

        // Get latest entity system pointer
        uintptr_t esPtr = Globals::latestEntitySys.load(std::memory_order_acquire);
        if (esPtr == 0) continue;

        // Update camera information
        Globals::UpdateCameraInfo();

        // Scan entities outside the render thread
        ScanEntityArray(CEntitySystem(esPtr), generation++);
    }

    gLogger.Info("Scanner thread stopped");
    return 0;
}

// Setup scanner thread
void InitializeScannerThread() {
    // Initialize scanner state
    Globals::scannerRunning.store(true, std::memory_order_release);

    // Create auto-reset event for signaling
    Globals::scannerWakeEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    // Create scanner thread
    Globals::scannerThread = CreateThread(NULL, 0, ScannerThreadProc, NULL, 0, NULL);

    gLogger.Info("Scanner thread initialized");
}

// Signal scanner from main thread
void SignalScanner() {
    static int frameCounter = 0;
    if (++frameCounter % 3 != 0) return; // Signal every 3 frames

    SetEvent(Globals::scannerWakeEvent);
}

// Cleanup scanner thread
void CleanupScannerThread() {
    gLogger.Info("Stopping scanner thread...");

    Globals::scannerRunning.store(false, std::memory_order_release);
    SetEvent(Globals::scannerWakeEvent); // Wake thread for shutdown

    // Wait for thread termination with timeout
    if (Globals::scannerThread != NULL) {
        if (WaitForSingleObject(Globals::scannerThread, 1000) == WAIT_TIMEOUT) {
            gLogger.Warning("Scanner thread did not exit gracefully, terminating");
            TerminateThread(Globals::scannerThread, 1);
        }

        CloseHandle(Globals::scannerThread);
        Globals::scannerThread = NULL;
    }

    if (Globals::scannerWakeEvent != NULL) {
        CloseHandle(Globals::scannerWakeEvent);
        Globals::scannerWakeEvent = NULL;
    }

    gLogger.Info("Scanner thread stopped");
}

// ===================================================
// Hook Implementations
// ===================================================
// CEntitySystem::Update hook - reduced to minimum work
void __fastcall hkCEntitySystem_Update(void* thisPtr, void* /*edx*/) {
    // Store entity system pointer and timestamp atomically - nothing else
    Globals::latestEntitySys.store(reinterpret_cast<uintptr_t>(thisPtr), std::memory_order_release);
    Globals::lastTick.store(__rdtsc(), std::memory_order_release);

    // Call original function
    return oCEntitySystem_Update(thisPtr, nullptr);
}

// Install CEntitySystem::Update hook
bool InstallEntitySystemHook() {
    gLogger.Info("Installing CEntitySystem::Update hook...");

    uintptr_t funcAddress = Globals::moduleBase + GameOffsets::CEntitySystem_UpdateOffset;

    if (!Utils::IsValidPtr(reinterpret_cast<void*>(funcAddress))) {
        gLogger.Error("CEntitySystem::Update address is invalid!");
        return false;
    }

    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<void*>(funcAddress),
        reinterpret_cast<void*>(&hkCEntitySystem_Update),
        reinterpret_cast<void**>(&oCEntitySystem_Update)
    );

    if (status != MH_OK) {
        gLogger.ErrorFormat("Failed to create CEntitySystem::Update hook. Error code: %d", status);
        return false;
    }

    status = MH_EnableHook(reinterpret_cast<void*>(funcAddress));

    if (status != MH_OK) {
        gLogger.ErrorFormat("Failed to enable CEntitySystem::Update hook. Error code: %d", status);
        return false;
    }

    gLogger.Info("CEntitySystem::Update hook installed successfully");
    return true;
}

// ===================================================
// ImGui Rendering Implementation
// ===================================================
// Forward declare ImGui WndProc handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Windows procedure hook
WNDPROC oWndProc = nullptr;
LRESULT __stdcall hkWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Pass events to ImGui
    if (initialized && ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
        return true;

    // Call original window procedure
    return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}

// Create DirectX render target
void CreateRenderTarget(IDXGISwapChain* pSwapChain) {
    ID3D11Texture2D* pBackBuffer;
    HRESULT hr = pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
    if (FAILED(hr)) {
        gLogger.ErrorFormat("Failed to get back buffer. HRESULT: 0x%X", hr);
        return;
    }

    hr = pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &mainRenderTargetView);
    pBackBuffer->Release();
    if (FAILED(hr)) {
        gLogger.ErrorFormat("Failed to create render target view. HRESULT: 0x%X", hr);
        return;
    }
}

// Clean up render target
void CleanupRenderTarget() {
    if (mainRenderTargetView) {
        mainRenderTargetView->Release();
        mainRenderTargetView = nullptr;
    }
}

// Initialize ImGui
bool InitImGui(IDXGISwapChain* pSwapChain) {
    // Get device and context
    HRESULT hr = pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&pDevice);
    if (FAILED(hr)) {
        gLogger.ErrorFormat("Failed to get device from swap chain. HRESULT: 0x%X", hr);
        return false;
    }

    pDevice->GetImmediateContext(&pContext);

    // Create render target
    CreateRenderTarget(pSwapChain);

    // Get window handle
    DXGI_SWAP_CHAIN_DESC sd;
    pSwapChain->GetDesc(&sd);
    gameWindow = sd.OutputWindow;

    // Hook window procedure
    oWndProc = (WNDPROC)SetWindowLongPtr(gameWindow, GWLP_WNDPROC, (LONG_PTR)hkWndProc);

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable keyboard controls

    // Setup ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(gameWindow);
    ImGui_ImplDX11_Init(pDevice, pContext);

    // Update global screen resolution
    Globals::screenResolution.width = io.DisplaySize.x;
    Globals::screenResolution.height = io.DisplaySize.y;

    // Load default font
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 1;
    fontConfig.PixelSnapH = true;
    io.Fonts->AddFontDefault(&fontConfig);

    // Build font atlas
    io.Fonts->Build();

    initialized = true;
    gLogger.Info("ImGui initialized successfully");
    return true;
}

// Clean up ImGui
void ShutdownImGui() {
    if (!initialized) return;

    // Clean up resources
    CleanupRenderTarget();

    // Shutdown ImGui
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    // Restore original window procedure
    SetWindowLongPtr(gameWindow, GWLP_WNDPROC, (LONG_PTR)oWndProc);

    // Release D3D resources
    if (pContext) pContext->Release();
    if (pDevice) pDevice->Release();

    initialized = false;
    gLogger.Info("ImGui shutdown complete");
}

// Render ESP overlay using background draw list
void RenderESPOverlay(ImDrawList* drawList, const ImVec2& displaySize) {
    // Access read-only registry - no locking needed
    const auto& registry = Globals::entityRegistry.read();

    // Skip if empty
    if (registry.empty()) return;

    // Process all entities in registry
    for (const auto& [key, record] : registry) {
        // Skip non-actors if setting disabled
        if (!record.st.isActor && !config.showNPCs) continue;

        // Skip if not on screen
        if (!record.dyn.screenPos.IsValid(displaySize.x, displaySize.y)) continue;

        // Get position and distance
        float x = record.dyn.screenPos.x;
        float y = record.dyn.screenPos.y;
        float distance = record.dyn.distance;

        // Create display text
        char textBuffer[128];
        int textLen;

        if (config.showDistance) {
            textLen = snprintf(textBuffer, sizeof(textBuffer), "%s [%.1fm]",
                record.st.name, distance);
        } else {
            textLen = snprintf(textBuffer, sizeof(textBuffer), "%s",
                record.st.name);
        }

        // Determine color based on entity type
        ImU32 textColor;
        if (strstr(record.st.name, "Player") != nullptr) {
            textColor = IM_COL32(255, 255, 0, 255); // Yellow for players
        } else {
            textColor = IM_COL32(0, 255, 255, 255); // Cyan for NPCs
        }

        // Scale text based on distance and settings
        float fontScale = std::max(0.5f, std::min(2.0f, config.textScale/100.0f * (1.0f - distance/1000.0f)));

        // Calculate text size for centering
        ImVec2 textSize = ImGui::CalcTextSize(textBuffer);
        textSize.x *= fontScale;
        textSize.y *= fontScale;

        // Draw text centered on entity
        drawList->AddText(
            NULL, // Use default font
            ImGui::GetFontSize() * fontScale,
            ImVec2(x - textSize.x * 0.5f, y - textSize.y * 0.5f),
            textColor,
            textBuffer,
            textBuffer + textLen
        );

        // Draw box if enabled
        if (config.showBoxes) {
            float boxWidth = textSize.x * 1.2f;
            float boxHeight = textSize.y * 1.5f;

            drawList->AddRect(
                ImVec2(x - boxWidth * 0.5f, y - boxHeight * 0.5f),
                ImVec2(x + boxWidth * 0.5f, y + boxHeight * 0.5f),
                textColor,
                0.0f, 0, 1.0f
            );
        }
    }
}

// Hooked Present function with optimized rendering
HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    // Initialize ImGui on first call
    if (!initialized) {
        if (!InitImGui(pSwapChain)) {
            return oPresent(pSwapChain, SyncInterval, Flags);
        }
    }

    // Start ImGui frame
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Get display size
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 displaySize = io.DisplaySize;

    // Update global screen resolution if changed
    if (Globals::screenResolution.width != displaySize.x ||
        Globals::screenResolution.height != displaySize.y) {
        Globals::screenResolution.width = displaySize.x;
        Globals::screenResolution.height = displaySize.y;
    }

    // Signal scanner thread
    SignalScanner();

    // Increment frame counter
    Globals::frameCount.fetch_add(1, std::memory_order_relaxed);

    // Get background draw list for all ESP rendering
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();

    // Render ESP overlay in a single draw list
    RenderESPOverlay(drawList, displaySize);

    // Create config window separately
    if (config.showConfigWindow) {
        ImGui::Begin("ESP Config", &config.showConfigWindow, ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
        ImGui::Separator();

        // ESP Configuration Options
        ImGui::Checkbox("Show NPCs", &config.showNPCs);
        ImGui::Checkbox("Show Distance", &config.showDistance);
        ImGui::Checkbox("Show Boxes", &config.showBoxes);
        ImGui::SliderInt("Text Size", &config.textScale, 50, 200, "%d%%");
        ImGui::SliderFloat("Field of View", &config.fieldOfViewDegrees, 60.0f, 120.0f, "%.1f°");

        ImGui::Separator();

        // Camera info
        if (Globals::cameraPosition.x != 0 ||
            Globals::cameraPosition.y != 0 ||
            Globals::cameraPosition.z != 0) {
            ImGui::Text("Camera: (%.1f, %.1f, %.1f)",
                        Globals::cameraPosition.x,
                        Globals::cameraPosition.y,
                        Globals::cameraPosition.z);
            ImGui::Text("FOV: %.1f°", Globals::cameraFOV * 180.0f / 3.14159f);
        } else {
            ImGui::Text("Camera: Not Available");
        }

        ImGui::Separator();

        // Entity stats
        size_t entityCount = Globals::entityCount.load(std::memory_order_acquire);
        size_t actorCount = Globals::actorCount.load(std::memory_order_acquire);

        ImGui::Text("Entities: %zu (Actors: %zu)", entityCount, actorCount);
        ImGui::Text("Frame: %zu", Globals::frameCount.load(std::memory_order_acquire));

        ImGui::End();
    }

    // Render ImGui
    ImGui::Render();

    // Set render target
    pContext->OMSetRenderTargets(1, &mainRenderTargetView, nullptr);

    // Render ImGui draw data
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    // Call original Present function
    return oPresent(pSwapChain, SyncInterval, Flags);
}

// Hooked ResizeBuffers function
HRESULT __stdcall hkResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    // Clean up resources before resize
    if (initialized) {
        CleanupRenderTarget();
    }

    // Call original ResizeBuffers function
    HRESULT result = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    // Recreate resources after resize
    if (SUCCEEDED(result) && initialized) {
        CreateRenderTarget(pSwapChain);

        // Update global screen resolution
        Globals::screenResolution.width = (float)Width;
        Globals::screenResolution.height = (float)Height;

        gLogger.InfoFormat("Screen resized to %dx%d", Width, Height);
    }

    return result;
}

// ===================================================
// DirectX Hook Initialization
// ===================================================
bool InitializeDirectXHooks() {
    gLogger.Info("Initializing DirectX hooks...");

    // Create a temporary window to get DirectX function addresses
    HWND tmpHwnd = CreateWindowA(
        "STATIC", "TempWindow", WS_OVERLAPPED,
        0, 0, 100, 100, nullptr, nullptr,
        GetModuleHandleA(nullptr), nullptr
    );

    if (!tmpHwnd) {
        gLogger.Error("Failed to create temporary window");
        return false;
    }

    // Create temporary D3D11 device and swapchain
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.Width = 100;
    sd.BufferDesc.Height = 100;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = tmpHwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* pTmpSwapChain = nullptr;
    ID3D11Device* pTmpDevice = nullptr;
    ID3D11DeviceContext* pTmpContext = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        &featureLevel, 1, D3D11_SDK_VERSION, &sd,
        &pTmpSwapChain, &pTmpDevice, nullptr, &pTmpContext
    );

    if (FAILED(hr)) {
        DestroyWindow(tmpHwnd);
        gLogger.ErrorFormat("Failed to create temporary DirectX device. HRESULT: 0x%X", hr);
        return false;
    }

    // Get vtable addresses
    void** pSwapChainVTable = *reinterpret_cast<void***>(pTmpSwapChain);

    // Clean up temporary objects
    pTmpSwapChain->Release();
    pTmpDevice->Release();
    pTmpContext->Release();
    DestroyWindow(tmpHwnd);

    // Create hooks for DirectX functions
    if (MH_CreateHook(pSwapChainVTable[8], reinterpret_cast<LPVOID>(&hkPresent), reinterpret_cast<LPVOID*>(&oPresent)) != MH_OK) {
        gLogger.Error("Failed to create Present hook");
        return false;
    }

    if (MH_CreateHook(pSwapChainVTable[13], reinterpret_cast<LPVOID>(&hkResizeBuffers), reinterpret_cast<LPVOID*>(&oResizeBuffers)) != MH_OK) {
        gLogger.Error("Failed to create ResizeBuffers hook");
        return false;
    }

    // Enable the hooks
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        gLogger.Error("Failed to enable DirectX hooks");
        return false;
    }

    gLogger.Info("DirectX hooks initialized successfully");
    return true;
}

// ===================================================
// DLL Main Entry Point
// ===================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        // Store module handle
        Globals::hModule = hModule;
        DisableThreadLibraryCalls(hModule);

        // Initialize logging
        gLogger.Initialize();
        gLogger.Info("ESP Overlay DLL attached to process");

        // Create a detached thread for initialization to avoid deadlocks
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            // Initialize MinHook
            MH_STATUS status = MH_Initialize();
            if (status != MH_OK) {
                gLogger.ErrorFormat("Failed to initialize MinHook. Error code: %d", status);
                return 1;
            }

            // Initialize DirectX hooks
            if (!InitializeDirectXHooks()) {
                gLogger.Error("Failed to initialize DirectX hooks");
                return 1;
            }

            // Find game module and initialize global base address
            Globals::moduleBase = Utils::GetGameModuleBase("StarCitizen.exe");
            if (Globals::moduleBase != 0) {
                // Install optimized CEntitySystem::Update hook
                if (InstallEntitySystemHook()) {
                    // Initialize scanner thread
                    InitializeScannerThread();

                    gLogger.Info("ESP system fully initialized");
                } else {
                    gLogger.Error("Failed to install entity system hook");
                }
            } else {
                gLogger.Error("Failed to find game module");
            }

            return 0;
        }, nullptr, 0, nullptr);
    }
    else if (dwReason == DLL_PROCESS_DETACH) {
        gLogger.Info("ESP Overlay DLL detaching from process");

        // Clean up scanner thread
        CleanupScannerThread();

        // Clean up ImGui
        ShutdownImGui();

        // Disable and uninitialize MinHook
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();

        // Close logging
        gLogger.Shutdown();
    }

    return TRUE;
}
