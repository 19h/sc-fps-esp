/**
 * @file EntityScanner.cpp
 * @brief Game entity scanner and ESP overlay implementation (Enhanced Resilience)
 *
 * This DLL can be injected into StarCitizen.exe to scan for entities and render
 * a real-time ESP overlay showing player positions. Uses DirectX11 hooking for
 * rendering and MinHook for function interception.
 *
 * Enhanced for resilience with comprehensive error checking and exception logging.
 * Includes context logging for memory readability checks.
 *
 * Compile as a DLL and inject into the target process.
 * Requirements: MinHook, D3D11, DXGI, ImGui
 */

// #define WIN32_LEAN_AND_MEAN // Defined by compiler options, remove from source
#include <windows.h>
#include <cstdint>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <optional>
#include <memory>
#include <algorithm>
#include <stdexcept>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <thread>
#include <functional>
#include <unordered_map>
#include <cstdio> // For snprintf, fopen, fwrite, fclose
#include <limits> // For numeric_limits
#include <cstddef> // For offsetof - needed for replacement calculation
#include <type_traits> // For std::conditional_t, std::is_const_v, std::remove_reference_t, std::is_pointer_v

// DirectX headers
#include <d3d11.h>
#include <dxgi.h>

// Third-party libraries
#include "MinHook.h"
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"

// Forward declarations (needed before Logger)
bool IsMemoryReadable(const void *ptr, const size_t size, const char *context);

//-----------------------------------------------------------------------------
// Constants, Types & Configuration
//-----------------------------------------------------------------------------

/// Target game module name
constexpr const char *TARGET_MODULE_NAME = "StarCitizen.exe";

/// Offset from module base to the GEnv* variable (Verify this offset!)
constexpr uintptr_t GENV_POINTER_OFFSET = 0x981D200; // WARNING: Offsets change with game updates!

/// Bit mask for extracting lower 48 bits of a pointer
constexpr uintptr_t MASK_LOWER_48 = 0xFFFFFFFFFFFFULL;

/// Logging tag for output
constexpr const char *LOG_TAG = "[EntityScanner] ";

/// Log file name
constexpr const char *LOG_FILE_NAME = "entitycore.log";

/// Maximum string length to read from possibly unsafe memory
constexpr size_t MAX_STRING_LENGTH = 256;

/// Fixed screen resolution values for ESP rendering (Fallback)
constexpr float DEFAULT_SCREEN_WIDTH = 2560.0f;
constexpr float DEFAULT_SCREEN_HEIGHT = 1440.0f;

/// Error result type for operations that can fail
template<typename T>
using Result = std::optional<T>;

/// Thread scanning interval in milliseconds
constexpr int SCAN_THREAD_INTERVAL_MS = 100;

/// Number of initial cycles to skip for warming up
constexpr int INITIAL_WARMUP_CYCLES = 10; // Increased for stability

//-----------------------------------------------------------------------------
// Improved Logger (Placed earlier for use in VFUNC/get_vfunc_ptr)
//-----------------------------------------------------------------------------
class Logger {
private:
    static inline std::mutex logMutex; // Ensure mutex is defined

public:
    /**
     * @brief Log a message to the log file (thread-safe, append)
     *
     * @param message Message to log
     */
    static void Log(const std::string &message) {
        try {
            std::lock_guard lock(logMutex);

            if (FILE *file = fopen(LOG_FILE_NAME, "a")) {
                // Add timestamp
                const auto now = std::chrono::system_clock::now();
                auto now_c = std::chrono::system_clock::to_time_t(now);
                std::tm now_tm{};
#ifdef _WIN32
                localtime_s(&now_tm, &now_c); // Windows specific
#else
                // Use thread-safe version if available, otherwise fallback
                std::tm* tm_ptr = localtime(&now_c);
                if (tm_ptr) {
                    now_tm = *tm_ptr;
                } else {
                    memset(&now_tm, 0, sizeof(now_tm));
                }
#endif

                std::ostringstream timestamp_ss;
                timestamp_ss << std::put_time(&now_tm, "%Y-%m-%d %H:%M:%S");
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
                timestamp_ss << '.' << std::setfill('0') << std::setw(3) << ms.count();

                const std::string fullMsg = "[" + timestamp_ss.str() + "] " + LOG_TAG + message + "\n";
                fwrite(fullMsg.c_str(), 1, fullMsg.size(), file);
                fclose(file);
            } else {
                const std::string errorMsg = LOG_TAG + std::string("ERROR: Failed to open log file '") + LOG_FILE_NAME +
                                             "'\n";
                OutputDebugStringA(errorMsg.c_str());
            }
            OutputDebugStringA((LOG_TAG + message + "\n").c_str());
        } catch (const std::exception &e) {
            const std::string errorMsg = LOG_TAG + std::string("Logger::Log exception: ") + e.what() + "\n";
            OutputDebugStringA(errorMsg.c_str());
        } catch (...) {
            const std::string errorMsg = LOG_TAG + std::string("Logger::Log unknown exception\n");
            OutputDebugStringA(errorMsg.c_str());
        }
    }

    /**
     * @brief Format and log a message to the log file using snprintf
     */
    template<typename... Args>
    static void LogFormat(const char *format, Args... args) {
        try {
            char buffer[2048];
            if (const int result = snprintf(buffer, sizeof(buffer), format, args...);
                result >= 0 && result < static_cast<int>(sizeof(buffer))) {
                Log(std::string(buffer));
            } else if (result >= static_cast<int>(sizeof(buffer))) {
                Log("LogFormat error: Buffer too small, message truncated.");
                buffer[sizeof(buffer) - 1] = '\0';
                Log(std::string(buffer));
            } else {
                Log("LogFormat error: Formatting error occurred.");
            }
        } catch (const std::exception &e) {
            Log(std::string("LogFormat exception: ") + e.what());
        } catch (...) {
            Log("LogFormat unknown exception");
        }
    }

    /** @brief Log an exception object */
    static void LogException(const std::exception &e, const std::string &context = "") {
        std::string msg = "Exception caught";
        if (!context.empty()) msg += " in " + context;
        msg += ": ";
        msg += e.what();
        Log(msg);
    }

    /** @brief Log an unknown exception */
    static void LogUnknownException(const std::string &context = "") {
        std::string msg = "Unknown exception caught";
        if (!context.empty()) msg += " in " + context;
        Log(msg);
    }
};

// Architecture-specific definitions
#if defined(_WIN64)
#define POINTER_SIZE 8
#define PTR_FORMAT "%llx"
#define IS_64BIT 1
#else
    #define POINTER_SIZE 4
    #define PTR_FORMAT "%x"
    #define IS_64BIT 0
#endif

// Forward declarations
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

//-----------------------------------------------------------------------------
// Memory & Pointer Utilities (Enhanced with Context Logging)
//-----------------------------------------------------------------------------

/**
 * @brief Checks if a memory range is readable using VirtualQuery. More reliable than IsBadReadPtr.
 * @param ptr Pointer to the start of the memory block.
 * @param size Size of the memory block to check.
 * @param context A string describing the context of the check (e.g., function name, variable being checked).
 * @return true if the entire range is readable, false otherwise.
 */
bool IsMemoryReadable(const void *ptr, const size_t size, const char *context) {
    // Added context parameter
    if (!ptr || size == 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi;
    size_t totalChecked = 0;
    const auto currentPtr = static_cast<const char *>(ptr);

    while (totalChecked < size) {
        if (VirtualQuery(currentPtr + totalChecked, &mbi, sizeof(mbi)) == 0) {
            const DWORD error = GetLastError();
            Logger::LogFormat(
                "IsMemoryReadable [%s]: VirtualQuery failed at %p, Error: %lu (0x%08X) - %zu/%zu bytes checked",
                context ? context : "Unknown",
                currentPtr + totalChecked, error, error, totalChecked, size);
            return false;
        }

        // Check if the page is committed and readable
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (
                                            PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) {
            // Log only if it's unexpected (avoid spamming for known non-readable regions if possible)
            // Logger::LogFormat(
            //     "IsMemoryReadable [%s]: Memory at %p not committed or not readable (State: 0x%X, Protect: 0x%X)",
            //     context ? context : "Unknown",
            //     currentPtr + totalChecked, mbi.State, mbi.Protect);
            return false;
        }

        // Check if the page protection includes guard page flag
        if ((mbi.Protect & PAGE_GUARD) != 0) {
            Logger::LogFormat("IsMemoryReadable [%s]: Memory at %p is a guard page (Protect: 0x%X)",
                              context ? context : "Unknown",
                              currentPtr + totalChecked, mbi.Protect);
            return false;
        }

        // Calculate how much of the remaining size is covered by this memory region
        // Check for potential overflow before adding RegionSize
        uintptr_t regionEnd;
        const uintptr_t baseAddr = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        if (baseAddr > UINTPTR_MAX - mbi.RegionSize) {
            regionEnd = UINTPTR_MAX; // Overflow occurred
            Logger::LogFormat("IsMemoryReadable [%s]: Potential overflow calculating region end (Base: %p, Size: %zu)",
                              context ? context : "Unknown", mbi.BaseAddress, mbi.RegionSize);
        } else {
            regionEnd = baseAddr + mbi.RegionSize;
        }

        const uintptr_t currentCheckAddr = reinterpret_cast<uintptr_t>(currentPtr + totalChecked);
        size_t bytesInRegion = (regionEnd > currentCheckAddr) ? (regionEnd - currentCheckAddr) : 0;


        size_t remainingToCheck = size - totalChecked;
        const size_t checkInThisRegion = std::min(bytesInRegion, remainingToCheck);

        // If checkInThisRegion is 0, it means VirtualQuery returned a region
        // that doesn't actually cover the address we queried, or the region calculation failed.
        if (checkInThisRegion == 0 && remainingToCheck > 0) {
            Logger::LogFormat(
                "IsMemoryReadable [%s]: Zero bytes checkable in region at %p (bytesInRegion: %zu, remaining: %zu)",
                context ? context : "Unknown", currentPtr + totalChecked, bytesInRegion, remainingToCheck);
            return false; // Cannot proceed
        }

        totalChecked += checkInThisRegion;
    }

    return true; // Entire range checked and found readable
}

/**
 * @brief Extracts the lower 48 bits of a pointer address
 */
inline uintptr_t ExtractLower48(uintptr_t ptr) noexcept {
    return ptr & MASK_LOWER_48;
}

/**
 * @brief Safely reads a C-string from potentially unsafe memory (Enhanced)
 */
std::string ReadCString(const char *ptr) {
    if (!ptr) {
        return "<null_ptr>";
    }
    if (!IsMemoryReadable(ptr, 1, "ReadCString - Initial Check")) {
        // Logger::LogFormat("ReadCString: Initial read check failed for ptr %p", static_cast<const void *>(ptr)); // Noisy
        return "<invalid_ptr_initial>";
    }

    std::string result;
    result.reserve(MAX_STRING_LENGTH);
    try {
        const char *current = ptr;
        for (size_t i = 0; i < MAX_STRING_LENGTH; ++i, ++current) {
            if (!IsMemoryReadable(current, 1, "ReadCString - Loop Check")) {
                // Logger::LogFormat("ReadCString: Bad read at index %zu for ptr %p", i, static_cast<const void *>(ptr)); // Noisy
                result += "<bad_read>";
                break;
            }
            const char c = *current;
            if (c == '\0') break;
            if (isprint(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80 || c == '\t' || c ==
                '\n' || c == '\r') {
                result += c;
            } else {
                result += '?';
            }
        }
        if (result.length() == MAX_STRING_LENGTH) {
            if (IsMemoryReadable(current, 1, "ReadCString - Truncation Check")) {
                if (*current != '\0') {
                    result += "...<truncated>";
                }
            }
        }
    } catch (const std::exception &e) {
        Logger::LogException(e, "ReadCString");
        return "<exception_reading_string>";
    } catch (...) {
        Logger::LogUnknownException("ReadCString");
        return "<unknown_exception_reading_string>";
    }
    if (result.empty() && ptr) {
        if (IsMemoryReadable(ptr, 1, "ReadCString - Final Empty Check")) {
            if (*ptr == '\0') return "";
        }
        if (result.find("<bad_read>") != std::string::npos) return result;
        return "<empty_or_error>";
    }
    return result;
}

/**
 * @brief Safely masks a pointer to extract the lower 48 bits
 */
template<typename T>
T *MaskedPtr(T *raw) {
    if (!raw) return nullptr;
    try {
        const auto v = reinterpret_cast<uintptr_t>(raw);
        return reinterpret_cast<T *>(ExtractLower48(v));
    } catch (const std::exception &e) {
        Logger::LogException(e, "MaskedPtr");
        return nullptr;
    } catch (...) {
        Logger::LogUnknownException("MaskedPtr");
        return nullptr;
    }
}

/**
 * @brief Checks if a pointer is non-null and the memory it points to is readable.
 */
template<typename T>
bool IsValidPtr(T *ptr, const char *context) {
    if (!ptr || reinterpret_cast<uintptr_t>(ptr) < 0x10000 || reinterpret_cast<uintptr_t>(ptr) == UINTPTR_MAX) {
        return false;
    }
    return IsMemoryReadable(ptr, sizeof(T), context);
}

//-----------------------------------------------------------------------------
// New Memory Access Utilities (Safe Implementation)
//-----------------------------------------------------------------------------

/**
 * @brief Safely retrieves a vtable function pointer with proper error handling
 * @tparam ReturnType The return type of the virtual function
 * @param instance Pointer to the class instance with vtable
 * @param vtableIndex Index into the vtable
 * @return Function pointer or nullptr if access fails
 */
template<typename ReturnType>
using GenericMemberFn = ReturnType(__thiscall*)(void*);

template<typename ReturnType>
GenericMemberFn<ReturnType> GetVTableFunction(const void* instance, size_t vtableIndex) {
    if (!instance || !IsMemoryReadable(instance, sizeof(void*), "GetVTableFunction")) {
        return nullptr;
    }

    void** vtable = *reinterpret_cast<void***>(const_cast<void*>(instance));
    if (!vtable || !IsMemoryReadable(vtable + vtableIndex, sizeof(void*),
                                   "GetVTableFunction vtable entry")) {
        return nullptr;
    }

    return reinterpret_cast<GenericMemberFn<ReturnType>>(vtable[vtableIndex]);
}

/**
 * @brief Safely reads a field from memory with proper error handling
 * @tparam T The type of the field to read
 * @param instance Pointer to the structure/class
 * @param offset Byte offset of the field
 * @return Value of the field or default-constructed T if access fails
 */
template<typename T>
T SafeReadField(const void* instance, size_t offset) {
    if (!instance || !IsMemoryReadable(
        reinterpret_cast<const uint8_t*>(instance) + offset,
        sizeof(T), "SafeReadField")) {
        return T{};
    }
    return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(instance) + offset);
}

//-----------------------------------------------------------------------------
// Math Classes (Added IsValid checks and epsilon comparisons)
//-----------------------------------------------------------------------------
constexpr float VEC_EPSILON = std::numeric_limits<float>::epsilon();
constexpr double DVEC_EPSILON = std::numeric_limits<double>::epsilon();

struct Vector2 {
    float x{0.0f};
    float y{0.0f};

    Vector2() = default;

    Vector2(const float inX, const float inY) : x(inX), y(inY) {
    }

    Vector2 operator+(const Vector2 &other) const { return {x + other.x, y + other.y}; }
    Vector2 operator-(const Vector2 &other) const { return {x - other.x, y - other.y}; }
    Vector2 operator*(const float scalar) const { return {x * scalar, y * scalar}; }

    Vector2 operator/(const float scalar) const {
        if (std::abs(scalar) < VEC_EPSILON) return {};
        return {x / scalar, y / scalar};
    }

    Vector2 &operator+=(const Vector2 &other) {
        x += other.x;
        y += other.y;
        return *this;
    }

    Vector2 &operator*=(const float scalar) {
        x *= scalar;
        y *= scalar;
        return *this;
    }

    [[nodiscard]] float Magnitude() const { return std::sqrt(x * x + y * y); }

    [[nodiscard]] Vector2 Normalized() const {
        if (const float mag = Magnitude(); mag > VEC_EPSILON)
            return *this / mag;

        return {};
    }

    void Normalize() { *this = Normalized(); }
    static float Distance(const Vector2 &a, const Vector2 &b) { return (a - b).Magnitude(); }
    [[nodiscard]] bool IsValid() const { return !std::isnan(x) && !std::isinf(x) && !std::isnan(y) && !std::isinf(y); }
};

struct Vector3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};

    Vector3() = default;

    Vector3(const float inX, const float inY, const float inZ) : x(inX), y(inY), z(inZ) {
    }

    explicit Vector3(const float *data) {
        if (data && IsMemoryReadable(data, sizeof(float) * 3, "Vector3 Constructor")) {
            x = data[0];
            y = data[1];
            z = data[2];
        } else {
            x = y = z = std::numeric_limits<float>::quiet_NaN(); // Indicate invalid state
        }
    }

    Vector3 operator+(const Vector3 &other) const { return {x + other.x, y + other.y, z + other.z}; }
    Vector3 operator-(const Vector3 &other) const { return {x - other.x, y - other.y, z - other.z}; }
    Vector3 operator*(const float scalar) const { return {x * scalar, y * scalar, z * scalar}; }

    Vector3 operator/(const float scalar) const {
        if (std::abs(scalar) < VEC_EPSILON) return {NAN, NAN, NAN}; // Return NaN on division by zero
        return {x / scalar, y / scalar, z / scalar};
    }

    Vector3 &operator+=(const Vector3 &other) {
        x += other.x;
        y += other.y;
        z += other.z;
        return *this;
    }

    [[nodiscard]] float Magnitude() const { return std::sqrt(x * x + y * y + z * z); }

    [[nodiscard]] Vector3 Normalized() const {
        if (const float mag = Magnitude(); mag > VEC_EPSILON)
            return *this / mag;

        return {};
    }

    void Normalize() { *this = Normalized(); }
    static float Distance(const Vector3 &a, const Vector3 &b) { return (a - b).Magnitude(); }

    [[nodiscard]] bool IsValid() const {
        return !std::isnan(x) && !std::isinf(x) && !std::isnan(y) && !std::isinf(y) && !std::isnan(z) && !std::isinf(z);
    }

    [[nodiscard]] bool IsOrigin() const {
        return std::abs(x) < VEC_EPSILON && std::abs(y) < VEC_EPSILON && std::abs(z) < VEC_EPSILON;
    }

    [[nodiscard]] ImVec2 ToImVec2() const { return {x, y}; }

    [[nodiscard]] bool IsOnScreen(const Vector2 &screenSize) const {
        return IsValid() && x >= 0 && x <= screenSize.x && y >= 0 && y <= screenSize.y && z > 0;
    }
};

struct Vector4 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float w{1.0f}; // Default w to 1 for quaternions/colors

    Vector4() = default;

    Vector4(const float inX, const float inY, const float inZ, const float inW) : x(inX), y(inY), z(inZ), w(inW) {
    }

    explicit Vector4(const float *data) {
        if (data && IsMemoryReadable(data, sizeof(float) * 4, "Vector4 Constructor")) {
            x = data[0];
            y = data[1];
            z = data[2];
            w = data[3];
        } else {
            x = y = z = w = std::numeric_limits<float>::quiet_NaN(); // Indicate invalid state
        }
    }

    bool operator==(const Vector4 &other) const {
        return std::abs(x - other.x) < VEC_EPSILON && std::abs(y - other.y) < VEC_EPSILON &&
               std::abs(z - other.z) < VEC_EPSILON && std::abs(w - other.w) < VEC_EPSILON;
    }

    [[nodiscard]] Vector4 GetConjugate() const { return {-x, -y, -z, w}; }

    Vector4 operator*(const Vector4 &other) const {
        // Quaternion multiplication
        return {
            w * other.x + x * other.w + y * other.z - z * other.y,
            w * other.y - x * other.z + y * other.w + z * other.x,
            w * other.z + x * other.y - y * other.x + z * other.w,
            w * other.w - x * other.x - y * other.y - z * other.z
        };
    }

    [[nodiscard]] bool IsValid() const {
        return !std::isnan(x) && !std::isinf(x) && !std::isnan(y) && !std::isinf(y) && !std::isnan(z) && !std::isinf(z)
               && !std::isnan(w) && !std::isinf(w);
    }
};

struct DVec3 {
    double x{0.0};
    double y{0.0};
    double z{0.0};

    DVec3() = default;

    DVec3(const double inX, const double inY, const double inZ) : x(inX), y(inY), z(inZ) {
    }

    explicit DVec3(const double *data) {
        if (data && IsMemoryReadable(data, sizeof(double) * 3, "DVec3 Constructor")) {
            x = data[0];
            y = data[1];
            z = data[2];
        } else {
            x = y = z = std::numeric_limits<double>::quiet_NaN(); // Indicate invalid state
        }
    }

    explicit DVec3(const Vector3 &v) : x(v.x), y(v.y), z(v.z) {
    }

    DVec3 operator+(const DVec3 &other) const { return {x + other.x, y + other.y, z + other.z}; }
    DVec3 operator+(const Vector3 &other) const { return {x + other.x, y + other.y, z + other.z}; }
    DVec3 operator-(const DVec3 &other) const { return {x - other.x, y - other.y, z - other.z}; }
    DVec3 operator*(const double scalar) const { return {x * scalar, y * scalar, z * scalar}; }

    DVec3 operator/(const double scalar) const {
        if (std::abs(scalar) < DVEC_EPSILON) return {NAN, NAN, NAN};
        return {x / scalar, y / scalar, z / scalar};
    }

    DVec3 operator*(const DVec3 &other) const { return {x * other.x, y * other.y, z * other.z}; } // Element-wise
    DVec3 &operator+=(const DVec3 &other) {
        x += other.x;
        y += other.y;
        z += other.z;
        return *this;
    }

    bool operator==(const DVec3 &other) const {
        return std::abs(x - other.x) < DVEC_EPSILON && std::abs(y - other.y) < DVEC_EPSILON && std::abs(z - other.z) <
               DVEC_EPSILON;
    }

    [[nodiscard]] double Magnitude() const { return std::sqrt(x * x + y * y + z * z); }

    [[nodiscard]] DVec3 Normalized() const {
        if (const double mag = Magnitude(); mag > DVEC_EPSILON)
            return *this / mag;

        return {};
    }

    static double Distance(const DVec3 &a, const DVec3 &b) { return (a - b).Magnitude(); }

    [[nodiscard]] bool IsValid() const {
        return !std::isnan(x) && !std::isinf(x) && !std::isnan(y) && !std::isinf(y) && !std::isnan(z) && !std::isinf(z);
    }

    [[nodiscard]] bool IsOrigin() const {
        return std::abs(x) < DVEC_EPSILON && std::abs(y) < DVEC_EPSILON && std::abs(z) < DVEC_EPSILON;
    }
};

//-----------------------------------------------------------------------------
// Game Structure Definitions (Refactored to use direct memory-safe functions)
//-----------------------------------------------------------------------------

#pragma pack(push, 1)

// Forward declarations
class CZone;
class CEntityClass;
class CEntity;
class CEntitySystem;
class CSystem;
class CRenderer;
class GEnv;
class CPhysicalEntity; // Forward declare for CZone if needed
struct SPhysicalEntity; // Forward declare for CZone if needed

/**
 * @brief Entity class, representing a type of game entity
 */
class CEntityClass {
public:
    // Direct accessors for fields
    int64_t GetFlags() const { return SafeReadField<int64_t>(this, 0x08); }
    const char* GetName() const { return SafeReadField<const char*>(this, 0x10); }

    // Compatibility accessors (matching original macro pattern)
    int64_t flags__ref() const { return GetFlags(); }
    const char* name__ref() const { return GetName(); }
}; // Size: 0x0090 (Verify this size!)

/**
 * @brief Entity instance in the game world
 */
class CEntity {
public:
    // Virtual function invocation
    DVec3 GetWorldPos() const {
        auto func = GetVTableFunction<DVec3>(this, 88);
        if (!func) return DVec3{};

        try {
            return func(const_cast<void*>(static_cast<const void*>(this)));
        } catch (const std::exception& e) {
            Logger::LogException(e, "CEntity::GetWorldPos");
            return DVec3{};
        } catch (...) {
            Logger::LogUnknownException("CEntity::GetWorldPos");
            return DVec3{};
        }
    }

    // Direct field accessors
    int64_t GetId() const { return SafeReadField<int64_t>(this, 0x10); }
    CEntityClass* GetEntityClass() const { return SafeReadField<CEntityClass*>(this, 0x20); }

    // Local position components
    double GetLocalX() const { return SafeReadField<double>(this, 0xF0); }
    double GetLocalY() const { return SafeReadField<double>(this, 0xF8); }
    double GetLocalZ() const { return SafeReadField<double>(this, 0x100); }

    // Entity name
    const char* GetNamePtr() const { return SafeReadField<const char*>(this, 0x290); }

    std::string GetName() const {
        const char* namePtr = GetNamePtr();
        return namePtr ? ReadCString(namePtr) : "<null>";
    }

    // Zone reference
    CZone* GetZone() const { return SafeReadField<CZone*>(this, 0x2A8); }

    // Compatibility accessors (matching original macro pattern)
    int64_t flags__ref() const { return SafeReadField<int64_t>(this, 0x08); }
    int64_t id__ref() const { return GetId(); }
    CEntityClass* c_entity_class__ref() const { return GetEntityClass(); }
    double x_local__ref() const { return GetLocalX(); }
    double y_local__ref() const { return GetLocalY(); }
    double z_local__ref() const { return GetLocalZ(); }
    const char* name__ref() const { return GetNamePtr(); }
    CZone* zone__ref() const { return GetZone(); }
}; // Size: 0x0A10 (Verify this size!)

/**
 * @brief Array container for game entities
 */
template<typename T>
class CEntityArray {
public:
    // Direct field accessors
    int64_t GetMaxSize() const { return SafeReadField<int64_t>(this, 0x00); }
    int64_t GetCurrentSize() const { return SafeReadField<int64_t>(this, 0x08); }
    T* GetData() const { return SafeReadField<T*>(this, 0x18); }

    // Get array element with bounds checking
    Result<T> GetAt(int64_t index) const {
        if (!IsStructureValid()) {
            return std::nullopt;
        }

        int64_t currSize = GetCurrentSize();
        T* data = GetData();

        if (!data || index < 0 || index >= currSize ||
            !IsMemoryReadable(data + index, sizeof(T), "CEntityArray::GetAt")) {
            return std::nullopt;
        }

        return data[index];
    }

    /**
     * @brief Verify if the array structure itself is readable and basic checks pass.
     */
    [[nodiscard]] bool IsStructureValid() const {
        if (const size_t requiredSize = 0x18 + sizeof(void*);
            !IsMemoryReadable(this, requiredSize, "CEntityArray::IsStructureValid")) {
            return false;
        }

        int64_t maxSize = GetMaxSize();
        int64_t currSize = GetCurrentSize();
        T* data = GetData();

        if (maxSize < 0 || currSize < 0 || currSize > maxSize) {
            return false;
        }

        if (!data && maxSize > 0) {
            return false;
        }

        return true;
    }

    // Compatibility accessors (matching original macro pattern)
    int64_t max_size__ref() const { return GetMaxSize(); }
    int64_t curr_size__ref() const { return GetCurrentSize(); }
    T* data__ref() const { return GetData(); }
};

/**
 * @brief Registry for entity classes
 */
class CEntityClassRegistry {
public:
    // Get entity class by name (virtual function at index 4)
    CEntityClass* GetEntityClass(const char* name) const {
        if (!name) return nullptr;

        auto func = GetVTableFunction<CEntityClass*>(this, 4);
        if (!func) return nullptr;

        try {
            return func(const_cast<void*>(static_cast<const void*>(this)));
        } catch (const std::exception& e) {
            Logger::LogException(e, "CEntityClassRegistry::GetEntityClass");
            return nullptr;
        } catch (...) {
            Logger::LogUnknownException("CEntityClassRegistry::GetEntityClass");
            return nullptr;
        }
    }
};

/**
 * @brief Entity management system
 */
class CEntitySystem {
public:
    // Get class registry (virtual function at index 24)
    CEntityClassRegistry* GetClassRegistry() const {
        auto func = GetVTableFunction<CEntityClassRegistry*>(this, 24);
        if (!func) return nullptr;

        try {
            return func(const_cast<void*>(static_cast<const void*>(this)));
        } catch (const std::exception& e) {
            Logger::LogException(e, "CEntitySystem::GetClassRegistry");
            return nullptr;
        } catch (...) {
            Logger::LogUnknownException("CEntitySystem::GetClassRegistry");
            return nullptr;
        }
    }

    // Direct field accessors
    const CEntityArray<CEntity*>& GetEntityArray() const {
        return *reinterpret_cast<const CEntityArray<CEntity*>*>(
            reinterpret_cast<const uint8_t*>(this) + 0x118);
    }

    CEntityClassRegistry* GetEntityClassRegistry() const {
        return SafeReadField<CEntityClassRegistry*>(this, 0x6D8);
    }

    void* GetEntRegistrySystem() const {
        return SafeReadField<void*>(this, 0x6E0);
    }

    // Compatibility accessors (matching original macro pattern)
    const CEntityArray<CEntity*>& entity_array__ref() const { return GetEntityArray(); }
    CEntityClassRegistry* entity_class_registry__ref() const { return GetEntityClassRegistry(); }
    void* m_pEntRegistrySystem__ref() const { return GetEntRegistrySystem(); }
};

/**
 * @brief Renderer system for the game
 */
class CRenderer {
public:
    // MT_Update virtual function
    void MT_Update() {
        auto func = GetVTableFunction<void>(this, 7);
        if (!func) return;

        try {
            func(static_cast<void*>(this));
        } catch (const std::exception& e) {
            Logger::LogException(e, "CRenderer::MT_Update");
        } catch (...) {
            Logger::LogUnknownException("CRenderer::MT_Update");
        }
    }

    // Static WorldToScreen function - Relies on global state (ProjectToScreen_stub, pRendererAddr)
    /**
     * @brief Convert 3D world position to 2D screen coordinates
     * @param pos 3D world position
     * @param out Pointer to Vector3 to receive screen coordinates
     * @param resolution Current screen resolution
     * @param isPlayerViewportRelative Flag to indicate player-relative coordinates
     * @return true if position is on screen, false otherwise
     */
    static bool WorldToScreen(
        const DVec3 &pos,
        Vector3 *out,
        const Vector2 &resolution, // Now takes resolution explicitly
        bool isPlayerViewportRelative = false
    );
};

/**
 * @brief Main game system
 */
class CSystem {
public:
    // Direct field accessors
    void* GetVfTable1() const { return SafeReadField<void*>(this, 0x0000); }
    void* GetVfTable2() const { return SafeReadField<void*>(this, 0x0008); }
    CEntitySystem* GetEntitySystem() const { return SafeReadField<CEntitySystem*>(this, 0x00B8); }
    CRenderer* GetRenderer() const { return SafeReadField<CRenderer*>(this, 0x0110); }

    // Compatibility accessors (matching original macro pattern)
    void* vfTable_1__ref() const { return GetVfTable1(); }
    void* vfTable_2__ref() const { return GetVfTable2(); }
    CEntitySystem* pEntitySystem__ref() const { return GetEntitySystem(); }
    CRenderer* pRenderer__ref() const { return GetRenderer(); }
};

/**
 * @brief Global environment container
 */
class GEnv {
public:
    // Direct field accessors
    CEntitySystem* GetEntitySystem() const { return SafeReadField<CEntitySystem*>(this, 0x00A0); }
    CSystem* GetSystem() const { return SafeReadField<CSystem*>(this, 0x00C0); }
    CRenderer* GetRenderer() const { return SafeReadField<CRenderer*>(this, 0x00F8); }

    // Compatibility accessors (matching original macro pattern)
    CEntitySystem* c_entity_system__ref() const { return GetEntitySystem(); }
    CSystem* c_system__ref() const { return GetSystem(); }
    CRenderer* c_renderer__ref() const { return GetRenderer(); }
};

#pragma pack(pop)

//-----------------------------------------------------------------------------
// Function Pointer Types for Hooking
//-----------------------------------------------------------------------------

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain *, UINT, UINT);
using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);

// WARNING: Signature needs verification! Assumed based on usage.
using ProjectToScreenFunc = bool (*)(
    void *pRenderer, // Should be CRenderer*? Or maybe an internal renderer interface?
    double worldX, double worldY, double worldZ,
    float *screenX, float *screenY, float *screenZ, // Outputs
    char isPlayerViewportRelative, // bool? char? int?
    int64_t unknown // Unknown parameter, might be flags or context
);

//-----------------------------------------------------------------------------
// Global Variables (Namespace and Initialization)
//-----------------------------------------------------------------------------

namespace Globals {
    // Module information
    inline std::atomic<uintptr_t> moduleBase = 0;

    // Game function pointers (atomic for thread safety during init)
    inline std::atomic<ProjectToScreenFunc> ProjectToScreen_stub = nullptr;
    // WARNING: pRendererAddr is a pointer *to* the CRenderer pointer (likely in CSystem).
    // It's the address where the game stores the CRenderer*.
    inline std::atomic<uintptr_t *> pRendererAddr = nullptr;

    // DirectX hook state
    inline PresentFn originalPresent = nullptr;
    inline ResizeBuffersFn originalResizeBuffers = nullptr;
    inline std::atomic hooksInitialized = false;
    inline std::atomic imguiInitialized = false;
    inline std::once_flag initImGuiOnce;

    // DirectX resources
    inline HWND hwnd = nullptr;
    inline ID3D11Device *pd3dDevice = nullptr;
    inline ID3D11DeviceContext *pd3dDeviceContext = nullptr;
    inline ID3D11RenderTargetView *mainRenderTargetView = nullptr;
    // Store original state for restoration in Present hook
    inline ID3D11RenderTargetView *originalRenderTargetView = nullptr;
    inline ID3D11DepthStencilView *originalDepthStencilView = nullptr;

    // Entity scanner thread
    inline HANDLE entityScanThreadHandle = nullptr;
    inline std::atomic shutdownRequested = false;

    // Information about found player entities
    struct PlayerInfo {
        // Store data needed for rendering, not the raw pointer if possible
        int64_t id;
        DVec3 position;
        std::string name; // Store copied name

        PlayerInfo(const int64_t entityId, const DVec3 &pos, std::string entityName)
            : id(entityId), position(pos), name(std::move(entityName)) {
        }
    };

    // Double-buffered player data for thread safety
    inline std::vector<std::unique_ptr<PlayerInfo> > playerInfoBackBuffer;
    inline std::vector<std::unique_ptr<PlayerInfo> > playerInfoFrontBuffer;
    inline std::mutex playerInfoMutex;

    // Global GEnv pointer cache (updated periodically)
    inline std::atomic<GEnv *> cachedGEnv = nullptr;
    inline std::mutex gEnvMutex; // Mutex for updating cachedGEnv safely
}

//-----------------------------------------------------------------------------
// Dynamic Screen Dimension Helpers
//-----------------------------------------------------------------------------

/**
 * @brief Get the current screen dimensions from ImGui IO (if available) or fallback.
 * @return Vector2 containing screen width and height. Returns fallback if ImGui not ready.
 */
Vector2 GetScreenDimensions() {
    if (Globals::imguiInitialized.load() && ImGui::GetCurrentContext()) {
        try {
            if (const ImGuiIO &io = ImGui::GetIO(); io.DisplaySize.x > 0 && io.DisplaySize.y > 0) {
                return Vector2{io.DisplaySize.x, io.DisplaySize.y};
            }
        } catch (const std::exception &e) {
            Logger::LogException(e, "GetScreenDimensions (ImGui)");
        } catch (...) {
            Logger::LogUnknownException("GetScreenDimensions (ImGui)");
        }
    }
    // Fallback if ImGui isn't ready or gives invalid dimensions
    return Vector2{DEFAULT_SCREEN_WIDTH, DEFAULT_SCREEN_HEIGHT};
}

//-----------------------------------------------------------------------------
// Renderer Implementation (CRenderer::WorldToScreen - Enhanced)
//-----------------------------------------------------------------------------

/**
 * @brief Safely calls the game's ProjectToScreen function.
 */
bool CRenderer::WorldToScreen(
    const DVec3 &pos,
    Vector3 *out,
    const Vector2 &resolution, // Use provided resolution
    const bool isPlayerViewportRelative
) {
    // 1. Validate input parameters
    if (!out) {
        Logger::Log("WorldToScreen: Output parameter 'out' is null.");
        return false;
    }
    out->x = out->y = out->z = std::numeric_limits<float>::quiet_NaN();
    if (!pos.IsValid()) {
        // Logger::Log("WorldToScreen: Input position is invalid (NaN/Inf)."); // Can be noisy
        return false;
    }
    if (resolution.x <= 0 || resolution.y <= 0 || !resolution.IsValid()) {
        Logger::LogFormat("WorldToScreen: Invalid resolution provided (%.2f, %.2f).", resolution.x, resolution.y);
        return false;
    }

    // 2. Get function pointer and renderer instance pointer safely
    const ProjectToScreenFunc projectionFunc = Globals::ProjectToScreen_stub.load();
    uintptr_t *pRendererAddr = Globals::pRendererAddr.load();

    if (!projectionFunc) {
        static bool loggedFuncNull = false;
        if (!loggedFuncNull) {
            Logger::Log("WorldToScreen: ProjectToScreen_stub function pointer is null.");
            loggedFuncNull = true;
        }
        return false;
    }
    if (!pRendererAddr) {
        static bool loggedAddrNull = false;
        if (!loggedAddrNull) {
            Logger::Log("WorldToScreen: pRendererAddr is null.");
            loggedAddrNull = true;
        }
        return false;
    }

    // 3. Dereference the renderer address pointer safely
    if (!IsMemoryReadable(pRendererAddr, sizeof(uintptr_t),
                          "CRenderer::WorldToScreen - Read Check for pRendererAddr")) {
        static bool loggedAddrUnreadable = false;
        if (!loggedAddrUnreadable) {
            Logger::LogFormat("WorldToScreen: Cannot read renderer pointer address %p.",
                              static_cast<void *>(pRendererAddr));
            loggedAddrUnreadable = true;
        }
        return false;
    }

    // 4. Call the projection function within a try-catch block
    bool success = false;
    float screenX = 0.0f, screenY = 0.0f, screenZ = 0.0f;
    try {
        // Read the actual renderer pointer value safely
        uintptr_t rendererPtrValue = 0;
        try { rendererPtrValue = *pRendererAddr; } catch (...) {
            Logger::LogFormat("WorldToScreen: Exception reading value from renderer pointer address %p.",
                              static_cast<void *>(pRendererAddr));
            return false;
        }
        void *actualRendererPtr = reinterpret_cast<void *>(rendererPtrValue);
        // Optional: Validate the actualRendererPtr? Could be null legitimately?
        // if (!IsValidPtr(actualRendererPtr, "CRenderer::WorldToScreen - actualRendererPtr check")) { ... }

        success = projectionFunc(
            actualRendererPtr, // Pass the actual renderer pointer value
            pos.x, pos.y, pos.z,
            &screenX, &screenY, &screenZ,
            static_cast<char>(isPlayerViewportRelative),
            0 // Pass the unknown parameter as 0 (assumption)
        );
    } catch (const std::exception &e) {
        Logger::LogException(e, "WorldToScreen -> projectionFunc call");
        return false;
    } catch (...) {
        Logger::LogUnknownException("WorldToScreen -> projectionFunc call");
        return false;
    }

    // 5. Process the result
    if (success) {
        if (std::isnan(screenX) || std::isinf(screenX) || std::isnan(screenY) || std::isinf(screenY) ||
            std::isnan(screenZ) || std::isinf(screenZ)) {
            // Logger::LogFormat("WorldToScreen: Projection succeeded but returned NaN/Inf (%.2f, %.2f, %.2f)", screenX, screenY, screenZ); // Noisy
            return false;
        }
        out->x = screenX * (resolution.x * 0.01f);
        out->y = screenY * (resolution.y * 0.01f);
        out->z = screenZ;
        return out->z > 0.0f; // Return true only if Z > 0 (in front of camera)
    }
    return false;
}

//-----------------------------------------------------------------------------
// DirectX & ImGui Rendering Utilities (Enhanced)
//-----------------------------------------------------------------------------

namespace Renderer {
    // Forward declare ShutdownImGui within the namespace
    void ShutdownImGui();

    /**
     * @brief Clean up the main render target view safely.
     */
    void CleanupRenderTarget() {
        if (Globals::mainRenderTargetView) {
            try {
                Globals::mainRenderTargetView->Release();
            } catch (const std::exception &e) {
                Logger::LogException(e, "CleanupRenderTarget -> Release");
            } catch (...) {
                Logger::LogUnknownException("CleanupRenderTarget -> Release");
            }
            Globals::mainRenderTargetView = nullptr;
        }
    }

    /**
     * @brief Create a new render target for ImGui safely.
     */
    bool CreateRenderTarget(IDXGISwapChain *swapChain) {
        if (!swapChain || !Globals::pd3dDevice) {
            Logger::LogFormat("CreateRenderTarget: Invalid state (SwapChain: %p, Device: %p)",
                              static_cast<void *>(swapChain), static_cast<void *>(Globals::pd3dDevice));
            return false;
        }
        CleanupRenderTarget(); // Clean existing first

        ID3D11Texture2D *pBackBuffer = nullptr;
        HRESULT hr = E_FAIL;
        try {
            hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&pBackBuffer));
            if (FAILED(hr) || !pBackBuffer) {
                Logger::LogFormat("CreateRenderTarget: Failed to get swap chain buffer (HRESULT: 0x%X, Buffer: %p)", hr,
                                  static_cast<void *>(pBackBuffer));
                if (pBackBuffer) pBackBuffer->Release(); // Release if non-null despite failure code?
                return false;
            }
            hr = Globals::pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &Globals::mainRenderTargetView);
            pBackBuffer->Release(); // Release buffer ptr
            if (FAILED(hr) || !Globals::mainRenderTargetView) {
                Logger::LogFormat("CreateRenderTarget: Failed to create render target view (HRESULT: 0x%X, RTV: %p)",
                                  hr, static_cast<void *>(Globals::mainRenderTargetView));
                Globals::mainRenderTargetView = nullptr;
                return false;
            }
            return true;
        } catch (const std::exception &e) {
            Logger::LogException(e, "CreateRenderTarget");
            if (pBackBuffer) pBackBuffer->Release();
            CleanupRenderTarget();
            return false;
        } catch (...) {
            Logger::LogUnknownException("CreateRenderTarget");
            if (pBackBuffer) pBackBuffer->Release();
            CleanupRenderTarget();
            return false;
        }
    }

    /**
     * @brief Initialize ImGui for rendering safely.
     */
    bool InitializeImGui(IDXGISwapChain *swapChain) {
        if (Globals::imguiInitialized.load()) return true;
        if (!swapChain) {
            Logger::Log("InitializeImGui: SwapChain is null.");
            return false;
        }

        Logger::Log("Attempting ImGui Initialization...");
        DXGI_SWAP_CHAIN_DESC desc;
        HRESULT hr = E_FAIL;
        try {
            hr = swapChain->GetDesc(&desc);
            if (FAILED(hr)) {
                Logger::LogFormat("InitializeImGui: Failed to get swap chain description (HRESULT: 0x%X)", hr);
                return false;
            }
            Globals::hwnd = desc.OutputWindow;
            if (!Globals::hwnd) {
                Logger::Log("InitializeImGui: Swap chain description provided null OutputWindow.");
                return false;
            }
            Logger::LogFormat("InitializeImGui: Found window handle: %p", Globals::hwnd);

            hr = swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void **>(&Globals::pd3dDevice));
            if (FAILED(hr) || !Globals::pd3dDevice) {
                Logger::LogFormat("InitializeImGui: Failed to get D3D11 device (HRESULT: 0x%X, Device: %p)", hr,
                                  static_cast<void *>(Globals::pd3dDevice));
                Globals::pd3dDevice = nullptr;
                return false;
            }
            Logger::LogFormat("InitializeImGui: Got D3D11 device: %p", static_cast<void *>(Globals::pd3dDevice));

            Globals::pd3dDevice->GetImmediateContext(&Globals::pd3dDeviceContext);
            if (!Globals::pd3dDeviceContext) {
                Logger::Log("InitializeImGui: Failed to get D3D11 device context.");
                Globals::pd3dDevice->Release();
                Globals::pd3dDevice = nullptr;
                return false;
            }
            Logger::LogFormat("InitializeImGui: Got D3D11 device context: %p",
                              static_cast<void *>(Globals::pd3dDeviceContext));

            if (!CreateRenderTarget(swapChain)) {
                Logger::Log("InitializeImGui: Failed to create render target view.");
                Globals::pd3dDeviceContext->Release();
                Globals::pd3dDeviceContext = nullptr;
                Globals::pd3dDevice->Release();
                Globals::pd3dDevice = nullptr;
                return false;
            }

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO &io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            Logger::Log("InitializeImGui: ImGui context created.");

            if (!ImGui_ImplWin32_Init(Globals::hwnd)) {
                Logger::Log("InitializeImGui: ImGui_ImplWin32_Init failed.");
                ImGui::DestroyContext();
                CleanupRenderTarget();
                Globals::pd3dDeviceContext->Release();
                Globals::pd3dDeviceContext = nullptr;
                Globals::pd3dDevice->Release();
                Globals::pd3dDevice = nullptr;
                return false;
            }
            Logger::Log("InitializeImGui: ImGui Win32 backend initialized.");

            if (!ImGui_ImplDX11_Init(Globals::pd3dDevice, Globals::pd3dDeviceContext)) {
                Logger::Log("InitializeImGui: ImGui_ImplDX11_Init failed.");
                ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
                CleanupRenderTarget();
                Globals::pd3dDeviceContext->Release();
                Globals::pd3dDeviceContext = nullptr;
                Globals::pd3dDevice->Release();
                Globals::pd3dDevice = nullptr;
                return false;
            }
            Logger::Log("InitializeImGui: ImGui DX11 backend initialized.");

            Logger::Log("ImGui Initialized Successfully.");
            Globals::imguiInitialized.store(true);
            return true;
        } catch (const std::exception &e) {
            Logger::LogException(e, "InitializeImGui");
            ShutdownImGui(); // Attempt full cleanup
            return false;
        } catch (...) {
            Logger::LogUnknownException("InitializeImGui");
            ShutdownImGui(); // Attempt full cleanup
            return false;
        }
    }

    /**
     * @brief Shutdown ImGui and release DirectX resources safely.
     */
    void ShutdownImGui() {
        if (!Globals::imguiInitialized.load() && !Globals::pd3dDevice && !Globals::pd3dDeviceContext && !
            Globals::mainRenderTargetView) {
            return;
        }
        Logger::Log("Shutting down ImGui and associated resources...");
        try {
            if (ImGui::GetCurrentContext()) {
                const ImGuiIO &io = ImGui::GetIO();
                if (io.BackendRendererUserData) ImGui_ImplDX11_Shutdown();
                if (io.BackendPlatformUserData) ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
            }
        } catch (const std::exception &e) { Logger::LogException(e, "ShutdownImGui (ImGui Cleanup)"); }
        catch (...) { Logger::LogUnknownException("ShutdownImGui (ImGui Cleanup)"); }

        CleanupRenderTarget();

        if (Globals::pd3dDeviceContext) {
            try { Globals::pd3dDeviceContext->Release(); } catch (const std::exception &e) {
                Logger::LogException(e, "ShutdownImGui (Context Release)");
            }
            catch (...) { Logger::LogUnknownException("ShutdownImGui (Context Release)"); }
            Globals::pd3dDeviceContext = nullptr;
        }
        if (Globals::pd3dDevice) {
            try { Globals::pd3dDevice->Release(); } catch (const std::exception &e) {
                Logger::LogException(e, "ShutdownImGui (Device Release)");
            }
            catch (...) { Logger::LogUnknownException("ShutdownImGui (Device Release)"); }
            Globals::pd3dDevice = nullptr;
        }
        Globals::hwnd = nullptr;
        Globals::imguiInitialized.store(false);
        Logger::Log("ImGui Shutdown Complete.");
    }

    /**
     * @brief Render text on the screen at the specified position safely.
     */
    void RenderText(ImDrawList *drawList, const ImVec2 &pos, const ImU32 color, const char *text) {
        if (!drawList || !text) return;
        if (std::isnan(pos.x) || std::isinf(pos.x) || std::isnan(pos.y) || std::isinf(pos.y)) return;
        try {
            drawList->AddText(ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 200), text);
            drawList->AddText(pos, color, text);
        } catch (const std::exception &e) {
            static bool loggedRenderTextError = false;
            if (!loggedRenderTextError) {
                Logger::LogException(e, "RenderText -> AddText");
                loggedRenderTextError = true;
            }
        } catch (...) {
            static bool loggedRenderTextUnknownError = false;
            if (!loggedRenderTextUnknownError) {
                Logger::LogUnknownException("RenderText -> AddText");
                loggedRenderTextUnknownError = true;
            }
        }
    }

    // Forward declaration for RenderPlayerESP needed here
    void RenderPlayerESP(ImDrawList *drawList, ImU32 textColor, ImU32 errorColor);
} // namespace Renderer

//-----------------------------------------------------------------------------
// Entity Scanner Implementation (Enhanced)
//-----------------------------------------------------------------------------

namespace EntityScanner {
    /**
     * @brief Safely find and return the game's GEnv instance address. Updates global cache.
     */
    GEnv *GetGameEnvironment() {
        uintptr_t currentModuleBase = Globals::moduleBase.load();
        if (currentModuleBase == 0) {
            if (HMODULE hGameModule = GetModuleHandleA(TARGET_MODULE_NAME)) {
                currentModuleBase = reinterpret_cast<uintptr_t>(hGameModule);
                Globals::moduleBase.store(currentModuleBase);
                Logger::LogFormat("GetGameEnvironment: Got module base address: 0x%p",
                                  reinterpret_cast<void *>(currentModuleBase));
            } else {
                static bool loggedModuleError = false;
                if (!loggedModuleError) {
                    Logger::LogFormat("GetGameEnvironment: Failed to get module handle for %s. GetLastError: %lu",
                                      TARGET_MODULE_NAME, GetLastError());
                    loggedModuleError = true;
                }
                return nullptr;
            }
        }

        uintptr_t gEnvInstanceAddr = 0;
        try {
            gEnvInstanceAddr = currentModuleBase + GENV_POINTER_OFFSET;
            if (gEnvInstanceAddr < currentModuleBase) {
                Logger::LogFormat(
                    "GetGameEnvironment: Potential overflow calculating GEnv instance address (Base: 0x%p, Offset: 0x%llX)",
                    reinterpret_cast<void *>(currentModuleBase), GENV_POINTER_OFFSET);
                return nullptr;
            }
        } catch (...) {
            Logger::LogUnknownException("GetGameEnvironment (Address Calculation)");
            return nullptr;
        }

        const auto gEnvPtr = reinterpret_cast<GEnv *>(gEnvInstanceAddr);
        try {
            if (!IsValidPtr(gEnvPtr, "EntityScanner::GetGameEnvironment - gEnvPtr check")) {
                static std::chrono::steady_clock::time_point lastLogTimeInvalid;
                if (const auto now = std::chrono::steady_clock::now();
                    now - lastLogTimeInvalid > std::chrono::seconds(5)) {
                    Logger::LogFormat(
                        "GetGameEnvironment: GEnv instance address 0x%p points to invalid/unreadable memory (Size: %zu).",
                        reinterpret_cast<void *>(gEnvInstanceAddr), sizeof(GEnv));
                    lastLogTimeInvalid = now;
                }
                std::lock_guard lock(Globals::gEnvMutex);
                Globals::cachedGEnv.store(nullptr);
                return nullptr;
            }
            if (Globals::cachedGEnv.load() != gEnvPtr) {
                std::lock_guard lock(Globals::gEnvMutex);
                Globals::cachedGEnv.store(gEnvPtr);
            }
            return gEnvPtr;
        } catch (const std::exception &e) {
            Logger::LogException(e, "GetGameEnvironment (Validation/Cache Update)");
            std::lock_guard lock(Globals::gEnvMutex);
            Globals::cachedGEnv.store(nullptr);
            return nullptr;
        } catch (...) {
            Logger::LogUnknownException("GetGameEnvironment (Validation/Cache Update)");
            std::lock_guard lock(Globals::gEnvMutex);
            Globals::cachedGEnv.store(nullptr);
            return nullptr;
        }
    }

    /**
        * @brief Initialize game function pointers needed for entity scanning safely.
        */
    bool InitializeFunctionPointers(GEnv *gEnv) {
        const uintptr_t currentModuleBase = Globals::moduleBase.load();
        if (currentModuleBase == 0) {
            Logger::Log("InitializeFunctionPointers: Module base is not yet set.");
            return false;
        }
        if (!gEnv || !IsValidPtr(gEnv, "EntityScanner::InitializeFunctionPointers - gEnv check")) {
            Logger::Log("InitializeFunctionPointers: Provided GEnv pointer is invalid.");
            return false;
        }

        Logger::LogFormat("Initializing function pointers with module base 0x%p and GEnv 0x%p",
                          reinterpret_cast<void *>(currentModuleBase), static_cast<void *>(gEnv));

        bool success = true;
        ProjectToScreenFunc localProjFunc = nullptr;
        uintptr_t *localRendererAddrPtr = nullptr;

        try {
            // --- ProjectToScreen ---
            constexpr uintptr_t projectToScreenOffset = 0x977a60; // VERIFY OFFSET!
            uintptr_t funcAddr = currentModuleBase + projectToScreenOffset;
            if (funcAddr < currentModuleBase) {
                Logger::LogFormat(
                    "InitializeFunctionPointers: Overflow calculating ProjectToScreen address (Base: 0x%p, Offset: 0x%llX)",
                    reinterpret_cast<void *>(currentModuleBase), projectToScreenOffset);
                success = false;
            } else {
                localProjFunc = reinterpret_cast<ProjectToScreenFunc>(funcAddr);
                // Basic check if function address seems plausible (e.g., not null)
                if (!localProjFunc) {
                    Logger::Log("InitializeFunctionPointers: Calculated ProjectToScreen address is null.");
                    success = false;
                }
            }

            // --- Renderer Pointer Address (FIXED IMPLEMENTATION) ---
            if (success) {
                try {
                    // Get the renderer pointer directly
                    CRenderer* renderer = gEnv->GetRenderer();
                    Logger::LogFormat("InitializeFunctionPointers: Renderer pointer value: %p",
                                      static_cast<void*>(renderer));

                    if (!renderer) {
                        Logger::Log("InitializeFunctionPointers: GetRenderer returned nullptr.");
                        success = false;
                    } else {
                        // Get the address where the renderer pointer is stored
                        localRendererAddrPtr = reinterpret_cast<uintptr_t*>(
                            reinterpret_cast<uintptr_t>(gEnv) + 0x00F8);

                        if (!IsMemoryReadable(localRendererAddrPtr, sizeof(uintptr_t),
                                            "EntityScanner::InitializeFunctionPointers - localRendererAddrPtr read check")) {
                            Logger::LogFormat(
                                "InitializeFunctionPointers: Calculated renderer pointer address 0x%p is not readable.",
                                reinterpret_cast<void*>(localRendererAddrPtr));
                            localRendererAddrPtr = nullptr;
                            success = false;
                        }
                    }
                } catch (const std::exception &e) {
                    Logger::LogException(e, "InitializeFunctionPointers (Renderer Address Calculation)");
                    localRendererAddrPtr = nullptr;
                    success = false;
                } catch (...) {
                    Logger::LogUnknownException("InitializeFunctionPointers (Renderer Address Calculation)");
                    localRendererAddrPtr = nullptr;
                    success = false;
                }
            }
        } catch (const std::exception &e) {
            Logger::LogException(e, "InitializeFunctionPointers (Address Calculation)");
            success = false;
        }
        catch (...) {
            Logger::LogUnknownException("InitializeFunctionPointers (Address Calculation)");
            success = false;
        }

        if (!success || !localProjFunc || !localRendererAddrPtr) {
            Logger::Log("InitializeFunctionPointers: Failed to calculate one or more valid pointers/addresses.");
            Globals::ProjectToScreen_stub.store(nullptr);
            Globals::pRendererAddr.store(nullptr);
            return false;
        }

        Globals::ProjectToScreen_stub.store(localProjFunc);
        Globals::pRendererAddr.store(localRendererAddrPtr);
        Logger::LogFormat("InitializeFunctionPointers: ProjectToScreen function address set to: 0x%p",
                          reinterpret_cast<void *>(localProjFunc));
        Logger::LogFormat("InitializeFunctionPointers: Renderer pointer address location set to: 0x%p",
                          reinterpret_cast<void *>(localRendererAddrPtr));

        if (Globals::ProjectToScreen_stub.load() == nullptr || Globals::pRendererAddr.load() == nullptr) {
            Logger::Log(
                "InitializeFunctionPointers: One or more pointers/addresses remain null after atomic store attempt.");
            return false;
        }
        Logger::Log("InitializeFunctionPointers: Successfully initialized function pointers.");
        return true;
    }
} // namespace EntityScanner

//-----------------------------------------------------------------------------
// Optimized entity tracking system (Enhanced Resilience)
//-----------------------------------------------------------------------------
namespace EntityTracker {
    // Constants
    constexpr int FULL_SCAN_INTERVAL_MS = 2000;
    constexpr int POSITION_UPDATE_INTERVAL_MS = 50; // Not currently used effectively
    constexpr int ENTITY_STALE_THRESHOLD_MS = 5000;
    constexpr int MAX_ENTITIES_PER_SCAN = 250000; // Limit iterations (adjust based on performance)

    // Types and Data Structures
    enum class EntityStatus { Active, Stale, Invalid };

    struct TrackedEntity {
        CEntity *entityPtr; // Store the raw pointer for checks
        int64_t entityId;
        DVec3 position;
        std::string name; // Store a copy of the name
        std::chrono::steady_clock::time_point lastSeen;
        EntityStatus status;

        TrackedEntity(CEntity *e, const int64_t id, const DVec3 &pos, std::string entityName)
            : entityPtr(e), entityId(id), position(pos), name(std::move(entityName)),
              lastSeen(std::chrono::steady_clock::now()), status(EntityStatus::Active) {
        }

        /**
         * @brief Update entity position using the stored pointer (UNSAFE - pointer may be invalid now).
         * Uses the safe GetWorldPosSafe wrapper internally.
         */
        bool UpdatePositionUnsafe() {
            if (status == EntityStatus::Invalid) return false;
            CEntity *currentPtr = nullptr;
            try { currentPtr = entityPtr; } catch (...) {
                status = EntityStatus::Invalid;
                return false;
            }

            if (!IsValidPtr(currentPtr, "TrackedEntity::UpdatePositionUnsafe - currentPtr check")) {
                // Logger::LogFormat("UpdatePositionUnsafe: Stored entity pointer %p for ID %lld became invalid.", static_cast<void *>(currentPtr), entityId); // Noisy
                status = EntityStatus::Invalid;
                return false;
            }
            if (const Result<int64_t> currentIdOpt = currentPtr->GetId();
                !currentIdOpt.has_value() || currentIdOpt.value() != entityId) {
                // Logger::LogFormat("UpdatePositionUnsafe: Entity ID mismatch for pointer %p (Expected: %lld, Got: %lld or error).", static_cast<void *>(currentPtr), entityId, currentIdOpt.value_or(-1)); // Noisy
                status = EntityStatus::Invalid;
                return false;
            }
            try {
                if (!IsValidPtr(currentPtr, "TrackedEntity::UpdatePositionUnsafe - currentPtr check before GetWorldPos")) {
                     Logger::LogFormat("UpdatePositionUnsafe: currentPtr pointer %p invalid before calling GetWorldPos.", static_cast<void*>(currentPtr));
                     status = EntityStatus::Invalid;
                     return false;
                }

                const DVec3 newPosition = currentPtr->GetWorldPos();

                if (newPosition.IsValid()) {
                    position = newPosition;
                    return true;
                }
                return false;
            } catch (const std::exception &e) {
                Logger::LogException(e, "UpdatePositionUnsafe (GetWorldPosSafe)");
                status = EntityStatus::Invalid;
                return false;
            }
            catch (...) {
                Logger::LogUnknownException("UpdatePositionUnsafe (GetWorldPosSafe)");
                status = EntityStatus::Invalid;
                return false;
            }
        }

        /** @brief Check if entity is still valid (basic pointer check). */
        [[nodiscard]] bool IsPointerValid() const {
            try { return IsValidPtr(entityPtr, "TrackedEntity::IsPointerValid - entityPtr check"); } catch (...) {
                return false;
            }
        }

        /** @brief Update the last seen timestamp and status. */
        void MarkAsSeen(CEntity *currentPtr, const DVec3 &currentPos, const std::string &currentName) {
            try {
                entityPtr = currentPtr;
                position = currentPos;
                name = currentName;
                lastSeen = std::chrono::steady_clock::now();
                status = EntityStatus::Active;
            } catch (const std::exception &e) {
                Logger::LogException(e, "MarkAsSeen");
                status = EntityStatus::Invalid;
            }
            catch (...) {
                Logger::LogUnknownException("MarkAsSeen");
                status = EntityStatus::Invalid;
            }
        }

        /** @brief Check if entity has gone stale. */
        [[nodiscard]] bool IsStale() const {
            if (status != EntityStatus::Active) return false;
            try {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSeen).count();
                return elapsed > ENTITY_STALE_THRESHOLD_MS;
            } catch (const std::exception &e) {
                Logger::LogException(e, "IsStale time calculation");
                return true;
            }
            catch (...) {
                Logger::LogUnknownException("IsStale time calculation");
                return true;
            }
        }
    };

    // Entity Tracking State
    inline std::unordered_map<int64_t, std::unique_ptr<TrackedEntity> > trackedEntities;
    inline std::mutex trackerMutex;
    inline std::chrono::steady_clock::time_point lastFullScan;
    inline std::chrono::steady_clock::time_point lastPositionUpdate;
    inline CEntityClass *playerClassCache = nullptr;
    inline std::atomic trackerInitialized = false;

    // Implementation

    /** @brief Initialize the entity tracker state. */
    void Initialize() {
        if (trackerInitialized.load()) return;
        Logger::Log("Initializing Entity Tracker...");
        std::lock_guard lock(trackerMutex);
        trackedEntities.clear();
        playerClassCache = nullptr;
        lastFullScan = std::chrono::steady_clock::now();
        lastPositionUpdate = lastFullScan;
        trackerInitialized.store(true);
        Logger::Log("Entity Tracker Initialized.");
    }

    /** @brief Safely get player class reference from game. Caches the result. */
    CEntityClass *GetEntityClass(const GEnv *gEnv) {
        CEntityClass *cached = nullptr;
        try { cached = playerClassCache; } catch (...) {
        }
        if (cached && IsValidPtr(cached, "EntityTracker::GetEntityClass - cached pointer check")) {
            return cached;
        }
        playerClassCache = nullptr;
        if (!gEnv) return nullptr;

        // Get entity system and check validity
        CEntitySystem *entitySystem = gEnv->GetEntitySystem();
        if (!entitySystem) return nullptr;

        if (!IsValidPtr(entitySystem, "EntityTracker::GetEntityClass - entitySystem check")) {
            Logger::Log("GetEntityClass: EntitySystem pointer is invalid before calling GetClassRegistry.");
            return nullptr;
        }

        // Get class registry using safer direct implementation
        CEntityClassRegistry *registry = entitySystem->GetClassRegistry();
        if (!registry) {
            Logger::Log("GetEntityClass: Failed to get class registry");
            return nullptr;
        }

        if (!IsValidPtr(registry, "EntityTracker::GetEntityClass - registry pointer check")) {
             Logger::Log("GetEntityClass: Registry pointer is invalid");
             return nullptr;
        }

        try {
            // Get player class directly
            CEntityClass *foundClass = registry->GetEntityClass("Player");
            if (foundClass && IsValidPtr(foundClass, "EntityTracker::GetEntityClass - foundClass pointer check")) {
                Logger::LogFormat("GetEntityClass: Found and cached 'Player' class at %p.",
                                  static_cast<void *>(foundClass));
                playerClassCache = foundClass;
                return foundClass;
            }

            if (foundClass) {
                Logger::LogFormat("GetEntityClass: Found 'Player' class but it's an invalid pointer: %p.",
                                  static_cast<void *>(foundClass));
            } else {
                static std::chrono::steady_clock::time_point lastLogTimeNotFound;
                auto now = std::chrono::steady_clock::now();
                if (now - lastLogTimeNotFound > std::chrono::seconds(10)) {
                    Logger::Log("GetEntityClass: 'Player' class not found in registry.");
                    lastLogTimeNotFound = now;
                }
            }
            return nullptr;
        } catch (const std::exception &e) {
            Logger::LogException(e, "GetEntityClass -> GetEntityClass call");
            playerClassCache = nullptr;
            return nullptr;
        }
        catch (...) {
            Logger::LogUnknownException("GetEntityClass -> GetEntityClass call");
            playerClassCache = nullptr;
            return nullptr;
        }
    }

    /** @brief Perform a full scan for player entities safely. */
    void PerformFullScan(GEnv *gEnv) {
        if (!gEnv) return;
        const CEntitySystem *entitySystem = gEnv->GetEntitySystem();
        if (!entitySystem) return;
        const CEntityClass *playerClass = GetEntityClass(gEnv);
        if (!playerClass) return;

        const CEntityArray<CEntity *> &entities = entitySystem->GetEntityArray();
        if (!entities.IsStructureValid()) return;
        int64_t maxEntities = entities.GetMaxSize();
        if (maxEntities <= 0) return;

        const int64_t scanLimit = std::min(maxEntities, static_cast<int64_t>(MAX_ENTITIES_PER_SCAN));
        if (maxEntities > MAX_ENTITIES_PER_SCAN) {
            Logger::LogFormat("PerformFullScan: Warning - Clamping scan iterations from %lld to %d.", maxEntities,
                              MAX_ENTITIES_PER_SCAN);
        }

        size_t markedStale = 0;
        for (auto &pair: trackedEntities) {
            try {
                if (pair.second && pair.second->status == EntityStatus::Active) {
                    pair.second->status = EntityStatus::Stale;
                    markedStale++;
                }
            } catch (...) {
            }
        }

        size_t newEntities = 0, updatedEntities = 0, skippedInvalid = 0, skippedWrongClass = 0, foundPlayers = 0;

        for (int64_t i = 0; i < scanLimit; ++i) {
            try {
                Result<CEntity *> entityPtrOpt = entities.GetAt(i);
                if (!entityPtrOpt.has_value()) continue;
                CEntity *rawPtr = entityPtrOpt.value();
                if (!rawPtr) continue;
                CEntity *entity = MaskedPtr(rawPtr);
                if (!IsValidPtr(entity, "EntityTracker::PerformFullScan - entity pointer check")) {
                    skippedInvalid++;
                    continue;
                }

                const CEntityClass *entityClass = entity->GetEntityClass();
                if (!entityClass) {
                    skippedInvalid++;
                    continue;
                }
                if (entityClass != playerClass) {
                    skippedWrongClass++;
                    continue;
                }

                foundPlayers++;
                int64_t entityId = entity->GetId();

                if (!IsValidPtr(entity, "EntityTracker::PerformFullScan - entity check before GetWorldPos")) {
                    Logger::LogFormat("PerformFullScan: Entity pointer %p invalid before calling GetWorldPos.", static_cast<void*>(entity));
                    skippedInvalid++;
                    continue;
                }

                DVec3 position = entity->GetWorldPos();

                if (!position.IsValid()) {
                    Logger::LogFormat("PerformFullScan: Failed to get valid position for player entity %lld (%p).",
                                      entityId, static_cast<void *>(entity));
                    skippedInvalid++;
                    continue;
                }

                std::string name = entity->GetName();
                if (name.find('<') != std::string::npos) {
                    Logger::LogFormat("PerformFullScan: Failed to get valid name for player entity %lld (%p). Got: %s",
                                      entityId, static_cast<void *>(entity), name.c_str());
                    name = "[Invalid Name]";
                }

                if (auto it = trackedEntities.find(entityId); it != trackedEntities.end()) {
                    if (it->second) {
                        it->second->MarkAsSeen(entity, position, name);
                        updatedEntities++;
                    } else {
                        trackedEntities.erase(it);
                        trackedEntities[entityId] = std::make_unique<TrackedEntity>(entity, entityId, position, name);
                        newEntities++;
                    }
                } else {
                    trackedEntities[entityId] = std::make_unique<TrackedEntity>(entity, entityId, position, name);
                    newEntities++;
                }
            } catch (const std::exception &e) {
                Logger::LogException(e, "PerformFullScan loop iteration");
                skippedInvalid++;
            }
            catch (...) {
                Logger::LogUnknownException("PerformFullScan loop iteration");
                skippedInvalid++;
            }
        }

        static std::chrono::steady_clock::time_point lastLogTime;
        if (const auto now = std::chrono::steady_clock::now(); now - lastLogTime > std::chrono::seconds(5)) {
            Logger::LogFormat(
                "Full scan: FoundPlayers=%zu, New=%zu, Updated=%zu, MarkedStale=%zu, SkippedInvalid=%zu, SkippedWrongClass=%zu (Limit=%lld, Max=%lld, Tracked=%zu)",
                foundPlayers, newEntities, updatedEntities, markedStale, skippedInvalid, skippedWrongClass, scanLimit,
                maxEntities, trackedEntities.size());
            lastLogTime = now;
        }
        lastFullScan = std::chrono::steady_clock::now();
    }

    /** @brief Update positions of already tracked entities (using potentially stale pointers - UNSAFE). */
    void UpdatePositionsUnsafe() {
        if (!trackerInitialized.load()) return;
        size_t updatedCount = 0, failedCount = 0;
        for (auto it = trackedEntities.begin(); it != trackedEntities.end();) {
            try {
                TrackedEntity *entity = it->second.get();
                if (!entity) {
                    it = trackedEntities.erase(it);
                    continue;
                }
                if (entity->status == EntityStatus::Active) {
                    if (entity->UpdatePositionUnsafe()) {
                        updatedCount++;
                        ++it;
                    } else {
                        if (entity->status == EntityStatus::Invalid) {
                            Logger::LogFormat("UpdatePositionsUnsafe: Entity %lld marked invalid during update.",
                                              it->first);
                        } else { failedCount++; }
                        ++it;
                    }
                } else { ++it; }
            } catch (const std::exception &e) {
                Logger::LogException(e, "UpdatePositionsUnsafe loop iteration");
                try { it = trackedEntities.erase(it); } catch (...) {
                    Logger::LogUnknownException("UpdatePositionsUnsafe (erase after exception)");
                    break;
                }
            }
            catch (...) {
                Logger::LogUnknownException("UpdatePositionsUnsafe loop iteration");
                try { it = trackedEntities.erase(it); } catch (...) {
                    Logger::LogUnknownException("UpdatePositionsUnsafe (erase after unknown exception)");
                    break;
                }
            }
        }
        static std::chrono::steady_clock::time_point lastLogTime;
        auto now = std::chrono::steady_clock::now();
        if (now - lastLogTime > std::chrono::seconds(5)) {
            if (updatedCount > 0 || failedCount > 0) {
                Logger::LogFormat("Position update (unsafe): Updated=%zu, Failed=%zu", updatedCount, failedCount);
            }
            lastLogTime = now;
        }
        lastPositionUpdate = std::chrono::steady_clock::now();
    }

    /** @brief Remove invalid and stale entities from tracking safely. */
    size_t PruneEntities() {
        if (!trackerInitialized.load()) return 0;
        size_t removedCount = 0;
        try {
            for (auto it = trackedEntities.begin(); it != trackedEntities.end();) {
                bool remove = false;
                try {
                    if (const TrackedEntity *entity = it->second.get(); !entity) {
                        remove = true;
                        Logger::LogFormat("PruneEntities: Found null unique_ptr in map for ID %lld!", it->first);
                    } else if (entity->status == EntityStatus::Invalid) { remove = true; } else if (entity->IsStale()) {
                        remove = true;
                    }
                } catch (const std::exception &e) {
                    Logger::LogException(e, "PruneEntities (Check Phase)");
                    remove = true;
                }
                catch (...) {
                    Logger::LogUnknownException("PruneEntities (Check Phase)");
                    remove = true;
                }
                if (remove) {
                    try {
                        it = trackedEntities.erase(it);
                        removedCount++;
                    } catch (const std::exception &e) {
                        Logger::LogException(e, "PruneEntities (Erase Phase)");
                        break;
                    } catch (...) {
                        Logger::LogUnknownException("PruneEntities (Erase Phase)");
                        break;
                    }
                } else {
                    try { ++it; } catch (const std::exception &e) {
                        Logger::LogException(e, "PruneEntities (Iterator Increment)");
                        break;
                    } catch (...) {
                        Logger::LogUnknownException("PruneEntities (Iterator Increment)");
                        break;
                    }
                }
            }
        } catch (const std::exception &e) { Logger::LogException(e, "PruneEntities (Outer Loop)"); }
        catch (...) { Logger::LogUnknownException("PruneEntities (Outer Loop)"); }
        if (removedCount > 0) {
            Logger::LogFormat("Pruned %zu stale/invalid entities. Total tracked: %zu", removedCount,
                              trackedEntities.size());
        }
        return removedCount;
    }

    /** @brief Main entity tracking update function. Manages scan timing and pruning. */
    std::vector<std::unique_ptr<Globals::PlayerInfo> > Update(GEnv *gEnv) {
        std::vector<std::unique_ptr<Globals::PlayerInfo> > result;
        if (!trackerInitialized.load() || !gEnv) return result;
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(trackerMutex);
        try {
            const auto fullScanElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFullScan).
                    count();
            bool didFullScan = false;
            if (fullScanElapsed >= FULL_SCAN_INTERVAL_MS) {
                PerformFullScan(gEnv);
                PruneEntities();
                didFullScan = true;
            }
            if (!didFullScan) {
                static std::chrono::steady_clock::time_point lastPruneTime;
                if (now - lastPruneTime > std::chrono::seconds(1)) {
                    PruneEntities();
                    lastPruneTime = now;
                }
            }
            result.reserve(trackedEntities.size());
            for (const auto &pair: trackedEntities) {
                try {
                    if (const TrackedEntity *entity = pair.second.get();
                        entity && entity->status == EntityStatus::Active && entity->position.IsValid()) {
                        result.push_back(
                            std::make_unique<Globals::PlayerInfo>(entity->entityId, entity->position, entity->name));
                    }
                } catch (const std::exception &e) {
                    Logger::LogException(e, "EntityTracker::Update (Populate Result Loop)");
                }
                catch (...) { Logger::LogUnknownException("EntityTracker::Update (Populate Result Loop)"); }
            }
        } catch (const std::exception &e) {
            Logger::LogException(e, "EntityTracker::Update");
            result.clear();
        }
        catch (...) {
            Logger::LogUnknownException("EntityTracker::Update");
            result.clear();
        }
        return result;
    }
} // namespace EntityTracker


//-----------------------------------------------------------------------------
// ESP Rendering (Enhanced)
//-----------------------------------------------------------------------------

/**
 * @brief Renders player ESP information using data from Globals::playerInfoFrontBuffer.
 */
void Renderer::RenderPlayerESP(ImDrawList *drawList, ImU32 textColor, ImU32 errorColor) {
    if (!drawList || !Globals::imguiInitialized.load()) return;

    float yPos = 10.0f;
    constexpr float xPos = 10.0f;
    float lineHeight = 15.0f;
    try {
        lineHeight = ImGui::GetTextLineHeightWithSpacing();
        if (lineHeight <= 0) lineHeight = 15.0f;
    } catch (...) {
    }

    const Vector2 screenSize = GetScreenDimensions();
    if (!screenSize.IsValid() || screenSize.x <= 0 || screenSize.y <= 0) {
        RenderText(drawList, ImVec2(xPos, yPos), errorColor, "ESP Error: Invalid Screen Size");
        return;
    }

    size_t playerCount = 0;
    try {
        std::lock_guard lock(Globals::playerInfoMutex);
        playerCount = Globals::playerInfoFrontBuffer.size();
    } catch (...) { playerCount = 0; }
    char headerBuffer[128];
    snprintf(headerBuffer, sizeof(headerBuffer), "[Players] found: %zu", playerCount);
    RenderText(drawList, ImVec2(xPos, yPos), textColor, headerBuffer);
    yPos += lineHeight;

    try {
        int displayedCount = 0;
        std::lock_guard lock(Globals::playerInfoMutex); // Lock for the duration of iteration
        for (const auto &playerInfoPtr: Globals::playerInfoFrontBuffer) {
            if (displayedCount >= 100) {
                RenderText(drawList, ImVec2(xPos, yPos), errorColor, "[...] Too many players to display all");
                yPos += lineHeight;
                break;
            }
            if (!playerInfoPtr) continue;
            const Globals::PlayerInfo &playerInfo = *playerInfoPtr;
            const int64_t id = playerInfo.id;
            const DVec3 &pos3D = playerInfo.position;
            const std::string &name = playerInfo.name;
            if (!pos3D.IsValid()) continue;

            Vector3 screenPos;
            if (!pos3D.IsOrigin() && CRenderer::WorldToScreen(pos3D, &screenPos, screenSize)) {
                if (!screenPos.IsValid()) continue;
                char entityInfoBuffer[512];
                snprintf(entityInfoBuffer, sizeof(entityInfoBuffer), "%s [%lld]", name.c_str(), id);
                RenderText(drawList, ImVec2(screenPos.x, screenPos.y), textColor, entityInfoBuffer);
                displayedCount++;
            }
        }
    } catch (const std::exception &e) {
        Logger::LogException(e, "RenderPlayerESP (Buffer Iteration)");
        RenderText(drawList, ImVec2(xPos, yPos), errorColor, "ESP Error: Exception during rendering");
    }
    catch (...) {
        Logger::LogUnknownException("RenderPlayerESP (Buffer Iteration)");
        RenderText(drawList, ImVec2(xPos, yPos), errorColor, "ESP Error: Unknown exception during rendering");
    }
}

//-----------------------------------------------------------------------------
// Entity Scan Thread Implementation (Concurrent Memory Acquisition)
//-----------------------------------------------------------------------------

/**
 * @brief Thread procedure for asynchronous entity enumeration with memory barrier synchronization
 */
DWORD WINAPI EntityScanThread([[maybe_unused]] LPVOID lpParam) {
    Logger::Log("EntityScanThread started.");
    int warmupCyclesRemaining = INITIAL_WARMUP_CYCLES;
    bool functionsInitialized = false;
    EntityTracker::Initialize();

    while (!Globals::shutdownRequested.load(std::memory_order_acquire)) {
        try {
            GEnv *gEnv = EntityScanner::GetGameEnvironment();
            if (!gEnv) {
                if (functionsInitialized || warmupCyclesRemaining < INITIAL_WARMUP_CYCLES) {
                    Logger::Log("EntityScanThread: GEnv lost. Resetting state.");
                    warmupCyclesRemaining = INITIAL_WARMUP_CYCLES;
                    functionsInitialized = false;
                    Globals::ProjectToScreen_stub.store(nullptr, std::memory_order_release);
                    Globals::pRendererAddr.store(nullptr, std::memory_order_release);
                    EntityTracker::playerClassCache = nullptr; {
                        std::lock_guard lock(Globals::playerInfoMutex);
                        Globals::playerInfoBackBuffer.clear();
                        Globals::playerInfoFrontBuffer.clear();
                    } {
                        std::lock_guard lockTracker(EntityTracker::trackerMutex);
                        EntityTracker::trackedEntities.clear();
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }

            if (warmupCyclesRemaining > 0) {
                warmupCyclesRemaining--;
                if (!functionsInitialized && gEnv) {
                    functionsInitialized = EntityScanner::InitializeFunctionPointers(gEnv);
                    if (!functionsInitialized) Logger::Log(
                        "Warmup: Failed to initialize function pointers (GEnv was valid), will retry.");
                    else Logger::Log("Warmup: Function pointers initialized.");
                } else if (!gEnv) { Logger::Log("Warmup: Skipping function pointer init, GEnv not yet valid."); }
                EntityTracker::Initialize(); // Ensure tracker is initialized
                std::this_thread::sleep_for(std::chrono::milliseconds(SCAN_THREAD_INTERVAL_MS * 2));
                continue;
            }

            if (!functionsInitialized) {
                if (gEnv) {
                    functionsInitialized = EntityScanner::InitializeFunctionPointers(gEnv);
                    if (!functionsInitialized) {
                        Logger::Log(
                            "EntityScanThread: Failed to initialize function pointers after warmup (GEnv was valid). Skipping scan cycle.");
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                        continue;
                    }
                    Logger::Log("EntityScanThread: Function pointers initialized after warmup phase.");
                } else {
                    Logger::Log(
                        "EntityScanThread: Cannot initialize function pointers post-warmup, GEnv is null. Skipping scan cycle.");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    continue;
                }
            }

            auto playerEntitiesData = EntityTracker::Update(gEnv); {
                std::lock_guard lock(Globals::playerInfoMutex);
                Globals::playerInfoBackBuffer = std::move(playerEntitiesData);
            }
        } catch (const std::exception &e) {
            Logger::LogException(e, "EntityScanThread main loop");
            try {
                {
                    std::lock_guard lock(Globals::playerInfoMutex);
                    Globals::playerInfoBackBuffer.clear();
                } {
                    std::lock_guard lockTracker(EntityTracker::trackerMutex);
                    EntityTracker::trackedEntities.clear();
                }
            } catch (...) { Logger::LogUnknownException("EntityScanThread (Exception Cleanup)"); }
            warmupCyclesRemaining = INITIAL_WARMUP_CYCLES;
            functionsInitialized = false;
            EntityTracker::playerClassCache = nullptr;
            Globals::ProjectToScreen_stub.store(nullptr, std::memory_order_release);
            Globals::pRendererAddr.store(nullptr, std::memory_order_release);
        } catch (...) {
            Logger::LogUnknownException("EntityScanThread main loop");
            try {
                {
                    std::lock_guard lock(Globals::playerInfoMutex);
                    Globals::playerInfoBackBuffer.clear();
                } {
                    std::lock_guard lockTracker(EntityTracker::trackerMutex);
                    EntityTracker::trackedEntities.clear();
                }
            } catch (...) { Logger::LogUnknownException("EntityScanThread (Unknown Exception Cleanup)"); }
            warmupCyclesRemaining = INITIAL_WARMUP_CYCLES;
            functionsInitialized = false;
            EntityTracker::playerClassCache = nullptr;
            Globals::ProjectToScreen_stub.store(nullptr, std::memory_order_release);
            Globals::pRendererAddr.store(nullptr, std::memory_order_release);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(SCAN_THREAD_INTERVAL_MS));
    }
    Logger::Log("EntityScanThread exiting.");
    return 0;
}

//-----------------------------------------------------------------------------
// DirectX Hooking Implementation (IAT/VTable Interception)
//-----------------------------------------------------------------------------

namespace Hooks {
    /**
     * @brief Trampoline for IDXGISwapChain::Present with callback interception
     * Offsets 0x18 into IDXGISwapChain's vtable (index 8)
     */
    HRESULT __stdcall HookedPresent(IDXGISwapChain *swapChain, const UINT syncInterval, const UINT flags) {
        try {
            std::call_once(Globals::initImGuiOnce, [swapChain] {
                if (!Renderer::InitializeImGui(swapChain)) {
                    Logger::Log("HookedPresent: Initial ImGui initialization failed via call_once.");
                }
            });
        } catch (const std::exception &e) { Logger::LogException(e, "HookedPresent (call_once ImGui Init)"); }
        catch (...) { Logger::LogUnknownException("HookedPresent (call_once ImGui Init)"); }

        if (Globals::imguiInitialized.load(std::memory_order_acquire) && Globals::pd3dDeviceContext && Globals::pd3dDevice) {
            try {
                if (!Globals::mainRenderTargetView) {
                    if (!Renderer::CreateRenderTarget(swapChain)) { goto CallOriginalPresent; }
                } {
                    std::lock_guard lock(Globals::playerInfoMutex);
                    Globals::playerInfoFrontBuffer.swap(Globals::playerInfoBackBuffer);
                    Globals::playerInfoBackBuffer.clear();
                }

                Globals::originalRenderTargetView = nullptr;
                Globals::originalDepthStencilView = nullptr;
                Globals::pd3dDeviceContext->OMGetRenderTargets(1, &Globals::originalRenderTargetView,
                                                               &Globals::originalDepthStencilView);

                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();
                if (ImDrawList *drawList = ImGui::GetBackgroundDrawList()) {
                    constexpr ImU32 textColor = IM_COL32(0, 255, 0, 255);
                    constexpr ImU32 errorColor = IM_COL32(255, 100, 0, 255);
                    Renderer::RenderPlayerESP(drawList, textColor, errorColor);
                }
                ImGui::Render();
                Globals::pd3dDeviceContext->OMSetRenderTargets(1, &Globals::mainRenderTargetView, nullptr);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                Globals::pd3dDeviceContext->OMSetRenderTargets(1, &Globals::originalRenderTargetView,
                                                               Globals::originalDepthStencilView);
                if (Globals::originalRenderTargetView) {
                    Globals::originalRenderTargetView->Release();
                    Globals::originalRenderTargetView = nullptr;
                }
                if (Globals::originalDepthStencilView) {
                    Globals::originalDepthStencilView->Release();
                    Globals::originalDepthStencilView = nullptr;
                }
            } catch (const std::exception &e) {
                Logger::LogException(e, "HookedPresent (ImGui Render Block)");
                try {
                    if (Globals::originalRenderTargetView) Globals::pd3dDeviceContext->OMSetRenderTargets(
                        1, &Globals::originalRenderTargetView, Globals::originalDepthStencilView);
                    if (Globals::originalRenderTargetView) {
                        Globals::originalRenderTargetView->Release();
                        Globals::originalRenderTargetView = nullptr;
                    }
                    if (Globals::originalDepthStencilView) {
                        Globals::originalDepthStencilView->Release();
                        Globals::originalDepthStencilView = nullptr;
                    }
                } catch (...) { Logger::LogUnknownException("HookedPresent (Exception State Restore)"); }
            } catch (...) {
                Logger::LogUnknownException("HookedPresent (ImGui Render Block)");
                try {
                    if (Globals::originalRenderTargetView) Globals::pd3dDeviceContext->OMSetRenderTargets(
                        1, &Globals::originalRenderTargetView, Globals::originalDepthStencilView);
                    if (Globals::originalRenderTargetView) {
                        Globals::originalRenderTargetView->Release();
                        Globals::originalRenderTargetView = nullptr;
                    }
                    if (Globals::originalDepthStencilView) {
                        Globals::originalDepthStencilView->Release();
                        Globals::originalDepthStencilView = nullptr;
                    }
                } catch (...) { Logger::LogUnknownException("HookedPresent (Unknown Exception State Restore)"); }
            }
        }

    CallOriginalPresent:
        HRESULT result = E_FAIL;
        if (Globals::originalPresent) {
            try { result = Globals::originalPresent(swapChain, syncInterval, flags); } catch (const std::exception &e) {
                Logger::LogException(e, "HookedPresent -> originalPresent call");
                result = E_FAIL;
            }
            catch (...) {
                Logger::LogUnknownException("HookedPresent -> originalPresent call");
                result = E_FAIL;
            }
        } else {
            Logger::Log("HookedPresent: Error - originalPresent is null!");
            result = DXGI_ERROR_INVALID_CALL;
        }

        if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
            Logger::LogFormat("HookedPresent: Detected device lost/reset (HRESULT: 0x%X). Shutting down ImGui.",
                              result);
            Renderer::ShutdownImGui();
            // Reset the once_flag manually is tricky and potentially unsafe.
            // Rely on ShutdownImGui setting imguiInitialized=false, allowing InitializeImGui to be called again.
        }
        return result;
    }

    /**
     * @brief Trampoline for IDXGISwapChain::ResizeBuffers with callback interception
     * Offsets 0x68 into IDXGISwapChain's vtable (index 13)
     */
    HRESULT __stdcall HookedResizeBuffers(IDXGISwapChain *swapChain, const UINT bufferCount, const UINT width,
                                          const UINT height, const DXGI_FORMAT newFormat, const UINT swapChainFlags) {
        Logger::LogFormat("HookedResizeBuffers called: Width=%u, Height=%u", width, height);
        if (Globals::mainRenderTargetView) {
            if (Globals::pd3dDeviceContext) {
                ID3D11RenderTargetView *nullRTV = nullptr;
                try { Globals::pd3dDeviceContext->OMSetRenderTargets(1, &nullRTV, nullptr); } catch (...) {
                    Logger::LogUnknownException("HookedResizeBuffers (OMSetRenderTargets to null)");
                }
            }
            Renderer::CleanupRenderTarget();
        }
        if (Globals::imguiInitialized.load(std::memory_order_acquire) && ImGui::GetCurrentContext()) {
            try {
                Logger::Log("HookedResizeBuffers: Invalidating ImGui device objects.");
                ImGui_ImplDX11_InvalidateDeviceObjects();
            } catch (const std::exception &e) { Logger::LogException(e, "HookedResizeBuffers (ImGui Invalidate)"); }
            catch (...) { Logger::LogUnknownException("HookedResizeBuffers (ImGui Invalidate)"); }
        }

        HRESULT hr = E_FAIL;
        if (Globals::originalResizeBuffers) {
            try {
                hr = Globals::originalResizeBuffers(swapChain, bufferCount, width, height, newFormat, swapChainFlags);
            } catch (const std::exception &e) {
                Logger::LogException(e, "HookedResizeBuffers -> originalResizeBuffers call");
                hr = E_FAIL;
            } catch (...) {
                Logger::LogUnknownException("HookedResizeBuffers -> originalResizeBuffers call");
                hr = E_FAIL;
            }
        } else {
            Logger::Log("HookedResizeBuffers: Error - originalResizeBuffers is null!");
            hr = DXGI_ERROR_INVALID_CALL;
        }

        if (SUCCEEDED(hr)) {
            Logger::Log("HookedResizeBuffers: Original ResizeBuffers succeeded. Recreating resources...");
            if (!Renderer::CreateRenderTarget(swapChain)) {
                Logger::Log("HookedResizeBuffers: Failed to recreate render target view after resize.");
            }
            if (Globals::imguiInitialized.load(std::memory_order_acquire) && ImGui::GetCurrentContext()) {
                try {
                    Logger::Log("HookedResizeBuffers: Recreating ImGui device objects.");
                    if (!ImGui_ImplDX11_CreateDeviceObjects()) {
                        Logger::Log("HookedResizeBuffers: Failed to recreate ImGui device objects.");
                    }
                } catch (const std::exception &e) {
                    Logger::LogException(e, "HookedResizeBuffers (ImGui CreateDeviceObjects)");
                } catch (...) { Logger::LogUnknownException("HookedResizeBuffers (ImGui CreateDeviceObjects)"); }
            }
        } else {
            Logger::LogFormat(
                "HookedResizeBuffers: Original ResizeBuffers failed (HRESULT: 0x%X). Not recreating resources.", hr);
            Renderer::CleanupRenderTarget();
        }
        return hr;
    }

    /**
     * @brief Creates ephemeral DirectX device for VTable address acquisition
     * Necessary for obtaining function addresses from the COM interface
     */
    bool CreateDummyDevice(IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice, ID3D11DeviceContext **ppContext,
                           HWND *pWindow) {
        Logger::Log("Creating dummy D3D11 device to find VTable addresses...");
        *ppSwapChain = nullptr;
        *ppDevice = nullptr;
        *ppContext = nullptr;
        *pWindow = nullptr;
        HWND hWnd = GetForegroundWindow();
        bool dummyWindowCreated = false;
        const HINSTANCE hInstance = GetModuleHandle(nullptr);
        const auto className = "DummyDXClass";

        if (!hWnd) {
            Logger::Log("No foreground window found, creating temporary dummy window...");
            const WNDCLASSEXA wc = {
                sizeof(WNDCLASSEX), CS_CLASSDC, DefWindowProc, 0L, 0L, hInstance, nullptr, nullptr, nullptr, nullptr,
                className, nullptr
            };
            if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                Logger::LogFormat("Failed to register dummy window class. GetLastError: %lu", GetLastError());
                return false;
            }
            hWnd = CreateWindowExA(0, className, "DummyDXWindow", WS_OVERLAPPEDWINDOW, 100, 100, 300, 300, nullptr,
                                   nullptr, hInstance, nullptr);
            if (!hWnd) {
                Logger::LogFormat("Failed to create dummy window. GetLastError: %lu", GetLastError());
                UnregisterClassA(className, hInstance);
                return false;
            }
            dummyWindowCreated = true;
            Logger::LogFormat("Dummy window created: %p", hWnd);
        } else { Logger::LogFormat("Using foreground window: %p", hWnd); }

        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 1;
        sd.BufferDesc.Width = 100;
        sd.BufferDesc.Height = 100;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hWnd;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        D3D_FEATURE_LEVEL featureLevel;
        constexpr D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        HRESULT hr = E_FAIL;
        try {
            constexpr UINT creationFlags = 0;
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags, featureLevels,
                                               std::size(featureLevels), D3D11_SDK_VERSION, &sd, ppSwapChain, ppDevice,
                                               &featureLevel, ppContext);
            if (FAILED(hr)) {
                Logger::LogFormat("Hardware D3D11CreateDeviceAndSwapChain failed (HRESULT: 0x%X), trying WARP.", hr);
                hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, creationFlags, featureLevels,
                                                   std::size(featureLevels), D3D11_SDK_VERSION, &sd, ppSwapChain,
                                                   ppDevice, &featureLevel, ppContext);
            }
            if (FAILED(hr)) {
                Logger::LogFormat("WARP D3D11CreateDeviceAndSwapChain also failed (HRESULT: 0x%X).", hr);
                if (dummyWindowCreated) {
                    DestroyWindow(hWnd);
                    UnregisterClassA(className, hInstance);
                }
                if (*ppSwapChain) (*ppSwapChain)->Release();
                if (*ppDevice) (*ppDevice)->Release();
                if (*ppContext) (*ppContext)->Release();
                *ppSwapChain = nullptr;
                *ppDevice = nullptr;
                *ppContext = nullptr;
                return false;
            }
            Logger::LogFormat("Dummy device created successfully (Device: %p, Context: %p, SwapChain: %p)",
                              static_cast<void *>(*ppDevice), static_cast<void *>(*ppContext),
                              static_cast<void *>(*ppSwapChain));
            *pWindow = hWnd;
            return true;
        } catch (const std::exception &e) {
            Logger::LogException(e, "CreateDummyDevice -> D3D11CreateDeviceAndSwapChain");
        }
        catch (...) { Logger::LogUnknownException("CreateDummyDevice -> D3D11CreateDeviceAndSwapChain"); }
        if (*ppSwapChain) (*ppSwapChain)->Release();
        if (*ppDevice) (*ppDevice)->Release();
        if (*ppContext) (*ppContext)->Release();
        *ppSwapChain = nullptr;
        *ppDevice = nullptr;
        *ppContext = nullptr;
        if (dummyWindowCreated) {
            DestroyWindow(hWnd);
            UnregisterClassA(className, hInstance);
        }
        return false;
    }

    /**
     * @brief Establishes API trampoline hooks via MinHook IAT/VTable interception
     */
    bool InitializeHooks() {
        if (Globals::hooksInitialized.load(std::memory_order_acquire)) {
            Logger::Log("InitializeHooks: Hooks already initialized.");
            return true;
        }
        Logger::Log("Initializing MinHook and DirectX hooks...");

        MH_STATUS mhStatus = MH_Initialize();
        if (mhStatus != MH_OK) {
            Logger::LogFormat("MH_Initialize failed. Status: %s", MH_StatusToString(mhStatus));
            return false;
        }
        Logger::Log("MinHook initialized successfully.");

        IDXGISwapChain *pSwapChain = nullptr;
        ID3D11Device *pDevice = nullptr;
        ID3D11DeviceContext *pContext = nullptr;
        HWND hUsedWnd = nullptr;
        bool dummyCreated = false;
        const HINSTANCE hInstance = GetModuleHandle(nullptr);
        if (!CreateDummyDevice(&pSwapChain, &pDevice, &pContext, &hUsedWnd)) {
            Logger::Log("Failed to create dummy DirectX device.");
            MH_Uninitialize();
            return false;
        }
        dummyCreated = GetForegroundWindow() != hUsedWnd;

        void *presentPtr = nullptr;
        void *resizeBuffersPtr = nullptr;
        bool vtableOk = false;
        try {
            if (!pSwapChain) {
                Logger::Log("Dummy SwapChain pointer is null after creation.");
                goto cleanup_dummy_hook_init;
            }
            void **pSwapChainVTable = nullptr;
            try { pSwapChainVTable = *reinterpret_cast<void ***>(pSwapChain); } catch (...) {
            }
            if (!IsValidPtr(pSwapChainVTable, "Hooks::InitializeHooks - pSwapChainVTable check")) {
                Logger::Log("Failed to get valid SwapChain VTable pointer.");
                goto cleanup_dummy_hook_init;
            }
            constexpr int presentIndex = 8;        // IDXGISwapChain::Present at vtable[8]
            constexpr int resizeBuffersIndex = 13; // IDXGISwapChain::ResizeBuffers at vtable[13]
            if (!IsMemoryReadable(pSwapChainVTable + presentIndex, sizeof(void *),
                                  "Hooks::InitializeHooks - VTable Present entry check")) {
                Logger::LogFormat("Cannot read VTable entry at index %d (Present).", presentIndex);
                goto cleanup_dummy_hook_init;
            }
            if (!IsMemoryReadable(pSwapChainVTable + resizeBuffersIndex, sizeof(void *),
                                  "Hooks::InitializeHooks - VTable ResizeBuffers entry check")) {
                Logger::LogFormat("Cannot read VTable entry at index %d (ResizeBuffers).", resizeBuffersIndex);
                goto cleanup_dummy_hook_init;
            }
            try {
                presentPtr = pSwapChainVTable[presentIndex];
                resizeBuffersPtr = pSwapChainVTable[resizeBuffersIndex];
            } catch (...) {
                Logger::LogUnknownException("InitializeHooks (Reading VTable Entries)");
                goto cleanup_dummy_hook_init;
            }
            if (!presentPtr || !resizeBuffersPtr) {
                Logger::LogFormat("Found null function pointer in VTable (Present: %p, ResizeBuffers: %p).", presentPtr,
                                  resizeBuffersPtr);
                goto cleanup_dummy_hook_init;
            }
            Logger::LogFormat("Found Present address: %p", presentPtr);
            Logger::LogFormat("Found ResizeBuffers address: %p", resizeBuffersPtr);
            vtableOk = true;
        } catch (const std::exception &e) { Logger::LogException(e, "InitializeHooks (VTable Access)"); }
        catch (...) { Logger::LogUnknownException("InitializeHooks (VTable Access)"); }

    cleanup_dummy_hook_init:
        if (pContext) pContext->Release();
        if (pDevice) pDevice->Release();
        if (pSwapChain) pSwapChain->Release();
        if (dummyCreated && hUsedWnd) {
            const auto className = "DummyDXClass";
            Logger::LogFormat("Destroying dummy window %p.", hUsedWnd);
            DestroyWindow(hUsedWnd);
            UnregisterClassA(className, hInstance);
        }
        Logger::Log("Dummy DirectX objects released.");
        if (!vtableOk) {
            MH_Uninitialize();
            return false;
        }

        bool hooksCreated = false;
        mhStatus = MH_CreateHook(presentPtr, reinterpret_cast<LPVOID>(HookedPresent),
                                 reinterpret_cast<void **>(&Globals::originalPresent));
        if (mhStatus != MH_OK) {
            Logger::LogFormat("MH_CreateHook failed for Present. Status: %s", MH_StatusToString(mhStatus));
        } else {
            mhStatus = MH_CreateHook(resizeBuffersPtr, reinterpret_cast<LPVOID>(HookedResizeBuffers),
                                     reinterpret_cast<void **>(&Globals::originalResizeBuffers));
            if (mhStatus != MH_OK) {
                Logger::LogFormat("MH_CreateHook failed for ResizeBuffers. Status: %s", MH_StatusToString(mhStatus));
                MH_RemoveHook(presentPtr);
            } else { hooksCreated = true; }
        }
        if (!hooksCreated) {
            MH_Uninitialize();
            return false;
        }
        Logger::Log("MinHook trampolines created successfully.");

        mhStatus = MH_EnableHook(presentPtr);
        if (mhStatus != MH_OK) {
            Logger::LogFormat("MH_EnableHook failed for Present. Status: %s", MH_StatusToString(mhStatus));
        } else {
            mhStatus = MH_EnableHook(resizeBuffersPtr);
            if (mhStatus != MH_OK) {
                Logger::LogFormat("MH_EnableHook failed for ResizeBuffers. Status: %s", MH_StatusToString(mhStatus));
                MH_DisableHook(presentPtr);
            } else {
                Logger::Log("DirectX hooks enabled successfully.");
                Globals::hooksInitialized.store(true, std::memory_order_release);
                return true;
            }
        }

        Logger::Log("Hook enabling failed. Removing hooks and uninitializing MinHook.");
        MH_RemoveHook(resizeBuffersPtr);
        MH_RemoveHook(presentPtr);
        MH_Uninitialize();
        return false;
    }

    /**
     * @brief Removes API hooks and restores original function pointers
     */
    void ShutdownHooks() {
        if (!Globals::hooksInitialized.load(std::memory_order_acquire)) { return; }
        Logger::Log("Disabling and removing hooks...");
        MH_STATUS status = MH_DisableHook(MH_ALL_HOOKS);
        if (status != MH_OK && status != MH_ERROR_DISABLED) {
            Logger::LogFormat("MH_DisableHook(MH_ALL_HOOKS) failed: %s", MH_StatusToString(status));
        } else { Logger::Log("All hooks disabled."); }
        status = MH_Uninitialize();
        if (status != MH_OK) { Logger::LogFormat("MH_Uninitialize failed: %s", MH_StatusToString(status)); } else {
            Logger::Log("MinHook uninitialized.");
        }
        Globals::hooksInitialized.store(false, std::memory_order_release);
        Globals::originalPresent = nullptr;
        Globals::originalResizeBuffers = nullptr;
    }
} // namespace Hooks

//-----------------------------------------------------------------------------
// Initialization Thread and DLL Entry Point
//-----------------------------------------------------------------------------

/**
 * @brief Thread procedure for asynchronous module initialization
 * Creates a separate execution context for DirectX hook establishment
 */
DWORD WINAPI StartupThread(LPVOID lpParam) {
    Logger::Log("StartupThread running...");
    bool success = false;
    try {
        if (!Hooks::InitializeHooks()) {
            Logger::Log("StartupThread: Failed to initialize hooks. Aborting startup.");
            return 1;
        }
        Globals::entityScanThreadHandle = CreateThread(nullptr, 0, EntityScanThread, nullptr, 0, nullptr);
        if (!Globals::entityScanThreadHandle) {
            Logger::LogFormat(
                "StartupThread: Failed to create entity scan thread. GetLastError: %lu. Cleaning up hooks.",
                GetLastError());
            Hooks::ShutdownHooks();
            return 1;
        }
        Logger::Log("StartupThread: Initialization completed successfully. Scan thread started.");
        success = true;
        return 0;
    } catch (const std::exception &e) { Logger::LogException(e, "StartupThread"); }
    catch (...) { Logger::LogUnknownException("StartupThread"); }
    if (!success) {
        Logger::Log("StartupThread: Exiting due to error.");
        if (Globals::hooksInitialized.load(std::memory_order_acquire)) { Hooks::ShutdownHooks(); }
        if (Globals::entityScanThreadHandle) {
            CloseHandle(Globals::entityScanThreadHandle);
            Globals::entityScanThreadHandle = nullptr;
        }
    }
    return 1;
}

/**
 * @brief PE/DLL entry point with process attach/detach handlers
 * Called by loader during module initialization/termination phases
 */
BOOL APIENTRY DllMain(const HMODULE hModule, const DWORD ul_reason_for_call, const LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hModule);  // Suppress DLL_THREAD_ATTACH/DETACH notifications
            if (FILE *f = fopen(LOG_FILE_NAME, "w")) fclose(f); // Clear log on attach
            Logger::Log("DLL Attached. Starting initialization...");
            if (const HANDLE hThread = CreateThread(nullptr, 0, StartupThread, nullptr, 0, nullptr)) {
                CloseHandle(hThread);  // Release thread handle while leaving thread running
                Logger::Log("Startup thread created.");
            } else {
                Logger::LogFormat("FATAL: Failed to create startup thread! GetLastError: %lu. DLL may not function.",
                                  GetLastError());
            }
            break;
        }
        case DLL_PROCESS_DETACH: {
            if (lpReserved == nullptr) {
                // Normal unload via FreeLibrary()
                Logger::Log("DLL Detaching (Normal Unload)...");
                Globals::shutdownRequested.store(true, std::memory_order_release);
                if (Globals::entityScanThreadHandle) {
                    Logger::Log("Waiting for entity scan thread to exit...");
                    if (const DWORD waitResult = WaitForSingleObject(Globals::entityScanThreadHandle, 2000);
                        waitResult == WAIT_TIMEOUT) {
                        Logger::Log("Entity scan thread did not exit gracefully, terminating...");
                        TerminateThread(Globals::entityScanThreadHandle, 0);
                    } else { Logger::Log("Entity scan thread exited."); }
                    CloseHandle(Globals::entityScanThreadHandle);
                    Globals::entityScanThreadHandle = nullptr;
                }
                Renderer::ShutdownImGui(); // Shutdown ImGui before removing hooks
                Hooks::ShutdownHooks();    // Remove hooks
                Globals::cachedGEnv.store(nullptr, std::memory_order_release);
                try {
                    std::lock_guard lock(Globals::playerInfoMutex);
                    Globals::playerInfoFrontBuffer.clear();
                    Globals::playerInfoBackBuffer.clear();
                } catch (...) {
                }
                Logger::Log("DLL Detach cleanup complete.");
            } else {
                // Process terminating - skip extensive cleanup
                Logger::Log("DLL Detaching (Process Terminating)... Skipping full cleanup.");
            }
            break;
        }
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH: break; // Suppressed via DisableThreadLibraryCalls
        default: break;
    }
    return TRUE;
}
