#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <memory>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

// Function prototypes
typedef HRESULT(__stdcall* Present)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
typedef HRESULT(__stdcall* ResizeBuffers)(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);

// Global variables for DirectX
Present oPresent = nullptr;
ResizeBuffers oResizeBuffers = nullptr;
ID3D11Device* pDevice = nullptr;
ID3D11DeviceContext* pContext = nullptr;
ID3D11RenderTargetView* mainRenderTargetView = nullptr;
HWND gameWindow = nullptr;
bool initialized = false;

// ESP Configuration
bool showNPCs = true;  // Toggle for showing NPC entities
int textScale = 100;   // Text scale percentage (100 = normal size)
bool showDistance = true; // Toggle for showing distance to entities
bool showBoxes = false;   // Toggle for showing bounding boxes
float fieldOfViewDegrees = 90.0f; // Default FOV for projection

// Font scaling (ImGui Version compatible)
ImFont* defaultFont = nullptr;
ImFont* smallFont = nullptr;
ImFont* mediumFont = nullptr;
ImFont* largeFont = nullptr;

// --- Game Structures ---
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

    std::string ToString() const {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(2) << "(" << x << ", " << y << ", " << z << ")";
        return ss.str();
    }
};

// 3x3 Matrix for view transformations
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

// Camera state tracking
struct CameraState {
    Vec3 position;
    float pitch, yaw, roll;
    bool isValid;

    CameraState() : position{0,0,0}, pitch(0), yaw(0), roll(0), isValid(false) {}
};

// Structure for 2D screen coordinates
struct Vec2 {
    float x, y, z; // z depth value for visibility test
    bool success;  // Whether projection was successful

    Vec2() : x(0), y(0), z(0), success(false) {}
    Vec2(float _x, float _y, float _z = 0, bool _success = true) : x(_x), y(_y), z(_z), success(_success) {}

    std::string ToString() const {
        if (!success) return "(projection failed)";
        std::stringstream ss;
        ss << std::fixed << std::setprecision(2) << "Screen(" << x << ", " << y << ")";
        return ss.str();
    }

    bool IsValid(float maxWidth, float maxHeight) const {
        if (!success) return false;
        if (std::isnan(x) || std::isnan(y)) return false;
        if (x <= 0 || y <= 0) return false;
        if (x >= maxWidth || y >= maxHeight) return false;
        if (x == 0 && y == 0) return false; // Often indicates projection failure
        return true;
    }
};

// Forward declarations for interdependent classes
class CEntityClass;
class CEntity;
class CEntityArray;
class CEntityClassRegistry;
class CEntitySystem;
class CRenderer;
class GEnv;

Vec2 WorldToScreen(const Vec3& worldPos, const CameraState& camera,
                  float fovX_deg, float screenW, float screenH);

//=============================================================================
// Entity Detection System - Adapted from previous implementation
//=============================================================================

// --- Utility Functions ---
namespace Utils {
    std::ofstream logFile;
    std::mutex logMutex;

    void InitLogging() {
        std::lock_guard<std::mutex> lock(logMutex);
        logFile.open("esp_overlay.log", std::ios::out | std::ios::trunc);
        if (logFile.is_open()) { logFile << "--- Log Started ---" << std::endl; }
    }

    void ShutdownLogging() {
        std::lock_guard<std::mutex> lock(logMutex);
        if (logFile.is_open()) {
            logFile << "--- Log Ended ---" << std::endl;
            logFile.close();
        }
    }

    void Log(const std::string& message) {
        std::lock_guard<std::mutex> lock(logMutex);
        if (logFile.is_open()) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            logFile << "[" << std::setw(2) << std::setfill('0') << st.wHour << ":" << std::setw(2) << std::setfill('0')
                << st.wMinute << ":" << std::setw(2) << std::setfill('0') << st.wSecond << "." << std::setw(3) <<
                std::setfill('0') << st.wMilliseconds << "] " << message << std::endl;
        }
    }

    void Log(const std::stringstream& ss) { Log(ss.str()); }

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

    bool ReadCString(uintptr_t address, std::string& outString, size_t maxLength = 256) {
        outString.clear();
        if (!IsValidPtr(reinterpret_cast<const void*>(address))) return false;
        try {
            char buffer[2] = { 0 };
            for (size_t i = 0; i < maxLength; ++i) {
                uintptr_t charAddr = address + i;
                if (!IsValidPtr(reinterpret_cast<const void*>(charAddr))) {
                    outString.clear();
                    return false;
                }
                char c = *reinterpret_cast<const char*>(charAddr);
                if (c == '\0') return true;
                buffer[0] = c;
                outString.append(buffer);
            }
            outString.clear();
            return false;
        }
        catch (...) {
            outString.clear();
            return false;
        }
    }

    uintptr_t GetGameModuleBase(const char* moduleName, int maxRetries = 10, int retryDelayMs = 1000) {
        uintptr_t base = 0;
        for (int i = 0; i < maxRetries && base == 0; ++i) {
            base = (uintptr_t)GetModuleHandleA(moduleName);
            if (base == 0) {
                std::stringstream ss;
                ss << "Module '" << moduleName << "' not found, retrying (" << (i + 1) << "/" << maxRetries << ")...";
                Log(ss);
                Sleep(retryDelayMs);
            }
        }
        return base;
    }

    template<typename FuncType, typename InstanceType, typename... Args>
    auto CallVFunc(InstanceType* instance, int index,
        Args... args) -> decltype(std::declval<FuncType>()(instance, args...)) {
        void* voidInstance = reinterpret_cast<void*>(instance);
        if (!IsValidPtr(voidInstance, false)) throw std::runtime_error("CallVFunc: Invalid instance pointer.");
        uintptr_t* vtable = *reinterpret_cast<uintptr_t**>(voidInstance);
        if (!IsValidPtr(vtable, true)) throw std::runtime_error("CallVFunc: Invalid VTable pointer.");
        if (index < 0 || index > 500) {
             std::stringstream ss;
             ss << "CallVFunc: VTable index " << index << " is out of reasonable bounds.";
             throw std::runtime_error(ss.str());
        }
        uintptr_t funcAddr = vtable[index];
        if (!IsValidPtr(reinterpret_cast<void*>(funcAddr), false)) {
            std::stringstream ss;
            ss << "CallVFunc: Invalid function address at VTable index " << index << " (Address: 0x" << std::hex << funcAddr << ")";
            throw std::runtime_error(ss.str());
        }
        FuncType func = reinterpret_cast<FuncType>(funcAddr);
        return func(instance, args...);
    }
} // namespace Utils

// --- Game Structures & Offsets ---
namespace GameOffsets {
    // Base Offsets
    const uintptr_t GEnvOffset = 0x981D200; // Offset from module base to GEnv* pointer

    // Offsets relative to GEnv*
    const uintptr_t GEnv_EntitySystemOffset = 0x00A0;
    const uintptr_t GEnv_RendererOffset = 0x00F8; // Offset to CRenderer in GEnv

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

    // Camera state offsets (from Frida script)
    const uintptr_t CameraFunctionOffset = 0x147097AF0; // Offset of camera function

    namespace CameraOffsets {
        const uintptr_t CameraDataBase = 0x9A * 8;

        // Rotation value offsets (relative to pCameraState)
        namespace Rotation {
            const uintptr_t xmm0 = 0xA2 * 8; // pitch related
            const uintptr_t xmm7 = 0x9E * 8; // roll related
            const uintptr_t xmm8 = 0x9F * 8; // roll related
            const uintptr_t xmm10 = 0x9B * 8;
            const uintptr_t xmm11 = 0xA3 * 8; // yaw related
            const uintptr_t xmm12 = 0xA4 * 8; // yaw related
        }

        // Position offsets (relative to pCameraData)
        namespace Position {
            const uintptr_t x = 3 * 8;
            const uintptr_t y = 7 * 8;
            const uintptr_t z = 0xB * 8;
        }
    }
}

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

    std::string GetName() const {
        std::string name;
        uintptr_t namePtr = *reinterpret_cast<uintptr_t*>(ptr + GameOffsets::EntityClass_NameOffset);
        if (!Utils::ReadCString(namePtr, name)) {
            return "<invalid-class-name>";
        }
        return name;
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

    std::shared_ptr<CEntityClass> GetEntityClass() const {
        uintptr_t classPtr = GetEntityClassPtr();
        if (!Utils::IsValidPtr(reinterpret_cast<void*>(classPtr))) {
            return nullptr;
        }
        return std::make_shared<CEntityClass>(classPtr);
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
            std::stringstream ss;
            ss << "Exception in GetWorldPos: " << e.what();
            Utils::Log(ss);
        }
        return outPos;
    }

    std::string GetName() const {
        std::string name;
        uintptr_t namePtr = *reinterpret_cast<uintptr_t*>(ptr + GameOffsets::Entity_NameOffset);
        if (!Utils::ReadCString(namePtr, name)) {
            return "<unnamed>";
        }
        return name;
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

    std::shared_ptr<CEntity> At(int64_t i) const {
        if (i < 0 || i >= GetMaxSize()) return nullptr;

        uintptr_t* data = reinterpret_cast<uintptr_t*>(GetDataPtr());
        if (!Utils::IsValidPtr(data)) return nullptr;

        uintptr_t maskedIndex = i & (GetMaxSize() - 1);
        uintptr_t elementPtr = data[maskedIndex];
        if (!Utils::IsValidPtr(reinterpret_cast<void*>(elementPtr))) return nullptr;

        uintptr_t realPtr = Utils::MaskPointer(elementPtr);
        if (!Utils::IsValidPtr(reinterpret_cast<void*>(realPtr))) return nullptr;

        return std::make_shared<CEntity>(realPtr);
    }

    std::vector<std::shared_ptr<CEntity>> ToVector() const {
        std::vector<std::shared_ptr<CEntity>> entities;
        for (int64_t i = 0; i < GetMaxSize(); i++) {
            auto entity = At(i);
            if (entity) {
                entities.push_back(entity);
            }
        }
        return entities;
    }
};

// Wrapper for CEntityClassRegistry
class CEntityClassRegistry {
private:
    uintptr_t ptr;

public:
    CEntityClassRegistry(uintptr_t address) : ptr(address) {}

    uintptr_t GetPtr() const { return ptr; }

    std::shared_ptr<CEntityClass> FindClass(const std::string& name) const {
        try {
            typedef void* (*FindClassFunc)(void* registry, const char* className);
            void* classPtr = Utils::CallVFunc<FindClassFunc>(
                reinterpret_cast<void*>(ptr),
                GameOffsets::EntitySystem_VTableIndex_GetEntityClass,
                name.c_str()
            );

            if (!Utils::IsValidPtr(classPtr)) return nullptr;

            return std::make_shared<CEntityClass>(reinterpret_cast<uintptr_t>(classPtr));
        } catch (const std::exception& e) {
            std::stringstream ss;
            ss << "Exception in FindClass: " << e.what();
            Utils::Log(ss);
            return nullptr;
        }
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

    std::shared_ptr<CEntityClassRegistry> GetClassRegistry() const {
        uintptr_t registryPtr = *reinterpret_cast<uintptr_t*>(ptr + GameOffsets::EntitySystem_ClassRegistryOffset);
        if (!Utils::IsValidPtr(reinterpret_cast<void*>(registryPtr))) return nullptr;
        return std::make_shared<CEntityClassRegistry>(registryPtr);
    }

    std::shared_ptr<CEntityClass> GetClassByName(const std::string& name) const {
        auto registry = GetClassRegistry();
        if (!registry) return nullptr;
        return registry->FindClass(name);
    }
};

// Wrapper for CRenderer
class CRenderer {
private:
    uintptr_t ptr;

public:
    CRenderer(uintptr_t address) : ptr(address) {}

    uintptr_t GetPtr() const { return ptr; }

    Vec2 ProjectToScreen(const Vec3& pos, const Vec2& resolution = Vec2(1920.0f, 1080.0f), bool isPlayerViewportRelative = true) const {
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
                isPlayerViewportRelative,
                0
            );

            if (result && outZ > 0.0f) {
                float screenX = outX * (resolution.x * 0.01f);
                float screenY = outY * (resolution.y * 0.01f);

                return Vec2(screenX, screenY, outZ, true);
            }

            return Vec2(0, 0, 0, false);
        } catch (const std::exception& e) {
            std::stringstream ss;
            ss << "Exception in ProjectToScreen: " << e.what();
            Utils::Log(ss);
            return Vec2(0, 0, 0, false);
        }
    }
};

// Wrapper for GEnv
class GEnv {
private:
    uintptr_t ptr;

public:
    GEnv(uintptr_t address) : ptr(address) {}

    uintptr_t GetPtr() const { return ptr; }

    std::shared_ptr<CEntitySystem> GetEntitySystem() const {
        uintptr_t sysPtr = *reinterpret_cast<uintptr_t*>(ptr + GameOffsets::GEnv_EntitySystemOffset);
        if (!Utils::IsValidPtr(reinterpret_cast<void*>(sysPtr))) return nullptr;
        return std::make_shared<CEntitySystem>(sysPtr);
    }

    std::shared_ptr<CRenderer> GetRenderer() const {
        uintptr_t rendererPtr = *reinterpret_cast<uintptr_t*>(ptr + GameOffsets::GEnv_RendererOffset);
        if (!Utils::IsValidPtr(reinterpret_cast<void*>(rendererPtr))) return nullptr;
        return std::make_shared<CRenderer>(rendererPtr);
    }
};

// Actor data structure holding entity information with screen position
struct ActorData {
    int64_t id;
    uintptr_t entityPtr;
    std::string name;
    std::string className;
    Vec3 rawPosition;
    Vec3 smoothPosition;
    Vec2 screenPos;
    std::chrono::steady_clock::time_point lastUpdate;

    bool operator==(const ActorData& other) const {
        return id == other.id && entityPtr == other.entityPtr;
    }
};

// Thread-safe lock-free actor cache using double buffering
class ActorCache {
private:
    std::vector<ActorData> buffers[2];
    std::atomic<int> activeBufferIndex{0};
    std::atomic<bool> pendingUpdate{false};

public:
    ActorCache() {
        buffers[0].reserve(100);
        buffers[1].reserve(100);
    }

    void UpdateCache(const std::vector<ActorData>& newActors) {
        int pendingIndex = 1 - activeBufferIndex.load(std::memory_order_acquire);

        buffers[pendingIndex].clear();
        buffers[pendingIndex] = newActors;

        pendingUpdate.store(true, std::memory_order_release);
        activeBufferIndex.store(pendingIndex, std::memory_order_release);
        pendingUpdate.store(false, std::memory_order_release);
    }

    std::vector<ActorData> GetCurrentActors() const {
        return buffers[activeBufferIndex.load(std::memory_order_acquire)];
    }

    bool HasPendingUpdate() const {
        return pendingUpdate.load(std::memory_order_acquire);
    }
};

// Forward declarations for necessary cross-references
namespace Globals {
    extern HMODULE hModule;
    extern uintptr_t moduleBase;

    struct ScreenResolution {
        float width;
        float height;
    };
    extern ScreenResolution screenResolution;

    void UpdateUserPlayer(int64_t id, const Vec3& position);
    Vec3 GetUserPlayerPosition();
    bool IsActorClass(const std::string& className);
}

// --- Global State Implementation ---
namespace Globals {
    HMODULE hModule = nullptr;
    uintptr_t moduleBase = 0;
    std::atomic<bool> runScannerThread = true;
    std::atomic<bool> runReporterThread = true;
    HANDLE scannerThreadHandle = nullptr;
    HANDLE reporterThreadHandle = nullptr;

    std::unordered_map<std::string, uintptr_t> entityClassCache;
    std::mutex entityClassCacheMutex;

    std::unordered_set<std::string> actorClassNames;
    std::mutex actorClassNamesMutex;

    ActorCache actorCache;

    std::atomic<size_t> scanCount{0};
    std::atomic<size_t> reportCount{0};

    Vec3 cameraPosition{0, 0, 0};

    ScreenResolution screenResolution = {1920.0f, 1080.0f};

    // User player tracking
    int64_t userPlayerId = -1;
    Vec3 userPlayerPosition{0, 0, 0};
    std::mutex userPlayerMutex;

    void UpdateUserPlayer(int64_t id, const Vec3& position) {
        std::lock_guard<std::mutex> lock(userPlayerMutex);

        // Only set user player once when first discovered
        if (userPlayerId == -1) {
            userPlayerId = id;
            Utils::Log("User player identified: ID=" + std::to_string(id));
        }

        // Always update position of identified user player
        if (userPlayerId == id) {
            userPlayerPosition = position;
        }
    }

    Vec3 GetUserPlayerPosition() {
        std::lock_guard<std::mutex> lock(userPlayerMutex);
        return userPlayerPosition;
    }

    void AddActorClass(const std::string& className) {
        std::lock_guard<std::mutex> lock(actorClassNamesMutex);
        actorClassNames.insert(className);
    }

    bool IsActorClass(const std::string& className) {
        std::lock_guard<std::mutex> lock(actorClassNamesMutex);
        return actorClassNames.count(className) > 0 ||
               className == "Player" ||
               className.find("NPC_") == 0;
    }

    void ClearActorClasses() {
        std::lock_guard<std::mutex> lock(actorClassNamesMutex);
        actorClassNames.clear();
    }

    uintptr_t GetCachedEntityClass(const std::string& className) {
        std::lock_guard<std::mutex> lock(entityClassCacheMutex);
        auto it = entityClassCache.find(className);
        if (it != entityClassCache.end()) {
            return it->second;
        }
        return 0;
    }

    void CacheEntityClass(const std::string& className, uintptr_t classPtr) {
        std::lock_guard<std::mutex> lock(entityClassCacheMutex);
        entityClassCache[className] = classPtr;
    }

    void ClearEntityClassCache() {
        std::lock_guard<std::mutex> lock(entityClassCacheMutex);
        entityClassCache.clear();
    }
}

// Camera state management namespace
namespace CameraHook {
    // Function prototype matching the game's camera function
    typedef void (*CameraFunc)(uintptr_t cameraStatePtr);

    CameraFunc originalCameraFunc = nullptr;
    CameraState currentCamera;
    std::mutex cameraMutex;
    uintptr_t cameraStatePtr = 0;
    const float RAD_TO_DEG = 57.295776f;

    // Hook implementation of camera function
    void __fastcall HookCameraFunc(uintptr_t statePtrArg) {
        if (cameraStatePtr == 0) {
            cameraStatePtr = statePtrArg;
            Utils::Log("Camera state pointer captured: 0x" + std::to_string(cameraStatePtr));
        }

        // Call original function
        originalCameraFunc(statePtrArg);

        // Update our camera state
        if (cameraStatePtr == 0) return;

        try {
            std::lock_guard<std::mutex> lock(cameraMutex);

            // Extract data pointers
            uintptr_t pCameraData = cameraStatePtr + GameOffsets::CameraOffsets::CameraDataBase;

            // Read rotation values
            double xmm0Val = *reinterpret_cast<double*>(cameraStatePtr + GameOffsets::CameraOffsets::Rotation::xmm0);
            double xmm7Val = *reinterpret_cast<double*>(cameraStatePtr + GameOffsets::CameraOffsets::Rotation::xmm7);
            double xmm8Val = *reinterpret_cast<double*>(cameraStatePtr + GameOffsets::CameraOffsets::Rotation::xmm8);
            double xmm11Val = *reinterpret_cast<double*>(cameraStatePtr + GameOffsets::CameraOffsets::Rotation::xmm11);
            double xmm12Val = *reinterpret_cast<double*>(cameraStatePtr + GameOffsets::CameraOffsets::Rotation::xmm12);

            // Calculate angles
            float xmm0Float = static_cast<float>(xmm0Val);
            float xmm7Float = static_cast<float>(xmm7Val);
            float xmm8Float = static_cast<float>(xmm8Val);
            float xmm11Float = static_cast<float>(xmm11Val);
            float xmm12Float = static_cast<float>(xmm12Val);

            // Calculate pitch
            float xmm1 = -xmm0Float;
            float xmm4 = std::max(std::min(xmm1, 1.0f), -1.0f);
            float pitchRad = asinf(xmm4);

            // Calculate yaw
            float yawRad = 0;
            if (fabsf(fabsf(pitchRad) - 1.5707964f) >= 0.0099999998f) {
                yawRad = atan2f(xmm11Float, xmm12Float);
            }

            // Calculate roll
            float rollRad = atan2f(xmm7Float, xmm8Float);

            // Read position values
            double posX = *reinterpret_cast<double*>(pCameraData + GameOffsets::CameraOffsets::Position::x);
            double posY = *reinterpret_cast<double*>(pCameraData + GameOffsets::CameraOffsets::Position::y);
            double posZ = *reinterpret_cast<double*>(pCameraData + GameOffsets::CameraOffsets::Position::z);

            // Update camera state
            currentCamera.position.x = posX;
            currentCamera.position.y = posY;
            currentCamera.position.z = posZ;
            currentCamera.pitch = pitchRad;
            currentCamera.yaw = yawRad;
            currentCamera.roll = rollRad;
            currentCamera.isValid = true;
        } catch (const std::exception& e) {
            Utils::Log("Exception in UpdateCameraState: " + std::string(e.what()));
        }
    }

    bool Initialize(uintptr_t moduleBase) {
        try {
            // Camera function address
            uintptr_t cameraFuncAddr = moduleBase + GameOffsets::CameraFunctionOffset;

            if (MH_CreateHook(reinterpret_cast<void*>(cameraFuncAddr),
                             reinterpret_cast<void*>(&HookCameraFunc),
                             reinterpret_cast<void**>(&originalCameraFunc)) != MH_OK) {
                Utils::Log("Failed to create camera function hook");
                return false;
            }

            if (MH_EnableHook(reinterpret_cast<void*>(cameraFuncAddr)) != MH_OK) {
                Utils::Log("Failed to enable camera function hook");
                return false;
            }

            Utils::Log("Camera function hook established");
            return true;
        } catch (const std::exception& e) {
            Utils::Log("Exception in CameraHook::Initialize: " + std::string(e.what()));
            return false;
        }
    }

    CameraState GetCameraState() {
        std::lock_guard<std::mutex> lock(cameraMutex);
        return currentCamera;
    }

    void Shutdown() {
        if (originalCameraFunc) {
            Utils::Log("Shutting down camera hook");
            if (Globals::moduleBase != 0) {
                MH_DisableHook(reinterpret_cast<void*>(Globals::moduleBase + GameOffsets::CameraFunctionOffset));
            }
            originalCameraFunc = nullptr;
        }
    }
}

// --- Entity Scanner Logic ---
namespace EntityScanner {
    uintptr_t GetGEnvPtr() {
        if (Globals::moduleBase == 0) return 0;
        uintptr_t gEnvPtrAddr = Globals::moduleBase + GameOffsets::GEnvOffset;
        if (!Utils::IsValidPtr(reinterpret_cast<void*>(gEnvPtrAddr))) {
            return 0;
        }
        return gEnvPtrAddr;
    }

    void UpdateEntityClassCache(std::shared_ptr<CEntitySystem> entitySystem) {
        if (!entitySystem) return;

        std::stringstream ss;
        ss << "[*] Looking up Player class...";
        Utils::Log(ss);

        auto playerClass = entitySystem->GetClassByName("Player");
        if (playerClass) {
            Globals::CacheEntityClass("Player", playerClass->GetPtr());
            std::stringstream ss2;
            ss2 << "    → Player class at 0x" << std::hex << playerClass->GetPtr();
            Utils::Log(ss2);
        } else {
            Utils::Log("    [!] Could not find Player class");
        }

        auto entityArray = entitySystem->GetEntityArray();
        auto entities = entityArray.ToVector();

        std::stringstream ss3;
        ss3 << "[*] Enumerating all entities...";
        Utils::Log(ss3);
        std::stringstream ss4;
        ss4 << "    → Found " << entities.size() << " entities";
        Utils::Log(ss4);

        std::unordered_map<std::string, std::shared_ptr<CEntityClass>> entityClasses;

        for (const auto& entity : entities) {
            auto entityClass = entity->GetEntityClass();
            if (!entityClass) continue;

            std::string className = entityClass->GetName();
            if (className.empty()) continue;

            entityClasses[className] = entityClass;
        }

        std::stringstream ss5;
        ss5 << "    → Found " << entityClasses.size() << " entity classes";
        Utils::Log(ss5);

        std::vector<std::string> classNames;
        for (const auto& pair : entityClasses) {
            classNames.push_back(pair.first);
        }
        std::sort(classNames.begin(), classNames.end());

        for (const auto& name : classNames) {
            std::stringstream ss6;
            ss6 << "        [→ " << name << "]";
            Utils::Log(ss6);
        }

        Globals::ClearActorClasses();
        for (const auto& name : classNames) {
            if (name == "Player" || name.find("NPC_") == 0) {
                Globals::AddActorClass(name);
                Globals::CacheEntityClass(name, entityClasses[name]->GetPtr());
            }
        }

        std::vector<std::string> actorClasses;
        {
            std::lock_guard<std::mutex> lock(Globals::actorClassNamesMutex);
            actorClasses.insert(actorClasses.end(), Globals::actorClassNames.begin(), Globals::actorClassNames.end());
        }
        actorClasses.push_back("Player");

        std::stringstream ss7;
        ss7 << "Actor classes: ";
        for (size_t i = 0; i < actorClasses.size(); ++i) {
            if (i > 0) ss7 << ", ";
            ss7 << actorClasses[i];
        }
        Utils::Log(ss7);
    }

    // tuning constants:
    constexpr float BASE_TAU   = 0.05f;   // 50 ms at d = 0
    constexpr float SCALE_TAU  = 0.002f;  // +2 ms per meter

    // Dedicated scanner thread - runs at 1Hz to perform full entity scans
    void ScannerThreadFunc() {
        Utils::Log("Entity Scanner Thread started (1Hz scan rate).");
        int consecutiveErrors = 0;
        const int maxConsecutiveErrors = 10;

        auto nextScanTime = std::chrono::steady_clock::now();

        while (Globals::runScannerThread) {
            try {
                auto now = std::chrono::steady_clock::now();
                if (now < nextScanTime) {
                    auto sleepTime = std::chrono::duration_cast<std::chrono::milliseconds>(nextScanTime - now);
                    if (sleepTime.count() > 0) {
                        std::this_thread::sleep_for(sleepTime);
                    }
                }

                nextScanTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                Globals::scanCount++;

                uintptr_t gEnvPtr = GetGEnvPtr();
                if (!gEnvPtr) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    consecutiveErrors++;
                    if (consecutiveErrors == maxConsecutiveErrors) {
                        Utils::Log("ScanThread: Failed to get GEnv* multiple times.");
                    }
                    continue;
                }

                GEnv gEnv(gEnvPtr);
                auto entitySystem = gEnv.GetEntitySystem();
                if (!entitySystem) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    consecutiveErrors++;
                    if (consecutiveErrors == maxConsecutiveErrors) {
                        Utils::Log("ScanThread: EntitySystem is null.");
                    }
                    continue;
                }

                if (Globals::scanCount % 10 == 0 || Globals::scanCount == 1) {
                    UpdateEntityClassCache(entitySystem);
                }

                auto entityArray = entitySystem->GetEntityArray();
                auto allEntities = entityArray.ToVector();

                if (allEntities.empty()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    consecutiveErrors++;
                    if (consecutiveErrors == maxConsecutiveErrors) {
                        Utils::Log("ScanThread: No entities found.");
                    }
                    continue;
                }

                consecutiveErrors = 0;

                std::stringstream ss;
                ss << "[*] Full entity scan #" << Globals::scanCount << " - Finding actors...";
                Utils::Log(ss);

                std::vector<ActorData> newActors;
                auto renderer = gEnv.GetRenderer();

                // Process all entities
                for (const auto& entity : allEntities) {
                    try {
                        auto entityClass = entity->GetEntityClass();
                        if (!entityClass) continue;

                        std::string className = entityClass->GetName();
                        Vec3 entityPos = entity->GetWorldPos();

                        // Update user player tracking if this is a player entity
                        if (className == "Player") {
                            // Update user player tracking system
                            Globals::UpdateUserPlayer(entity->GetId(), entityPos);
                        }

                        // Add entity to actors list if it's a tracked class
                        if (Globals::IsActorClass(className)) {
                            ActorData actor;
                            actor.id = entity->GetId();
                            actor.entityPtr = entity->GetPtr();
                            actor.name = entity->GetName();
                            actor.className = className;
                            actor.rawPosition = entityPos;

                            auto now = std::chrono::steady_clock::now();
                            float dt = std::chrono::duration<float>( now - actor.lastUpdate ).count();

                            // first frame for this actor?
                            if (actor.lastUpdate == std::chrono::steady_clock::time_point{}) {
                                actor.smoothPosition = actor.rawPosition;
                            } else {
                                float dist = actor.rawPosition.DistanceTo( Globals::GetUserPlayerPosition() );
                                float tau = BASE_TAU + SCALE_TAU * dist;
                                float alpha = dt / (tau + dt);
                                actor.smoothPosition.x = actor.smoothPosition.x * (1.0f - alpha)
                                                        + actor.rawPosition.x     * alpha;
                                actor.smoothPosition.y = actor.smoothPosition.y * (1.0f - alpha)
                                                        + actor.rawPosition.y     * alpha;
                                actor.smoothPosition.z = actor.smoothPosition.z * (1.0f - alpha)
                                                        + actor.rawPosition.z     * alpha;
                            }

                            actor.lastUpdate = now;

                            // Get camera state for custom projection
                            CameraState camera = CameraHook::GetCameraState();

                            if (camera.isValid) {
                                // Use custom projection
                                actor.screenPos = WorldToScreen(
                                    actor.smoothPosition,
                                    CameraHook::GetCameraState(),
                                    fieldOfViewDegrees,
                                    Globals::screenResolution.width,
                                    Globals::screenResolution.height
                                );
                            } else if (renderer) {
                                // Fallback to game's renderer
                                Vec2 resolution = { Globals::screenResolution.width, Globals::screenResolution.height };
                                actor.screenPos = renderer->ProjectToScreen(actor.rawPosition, resolution);
                            } else {
                                actor.screenPos = {0, 0, 0, false};
                            }

                            newActors.push_back(actor);
                        }
                    } catch (const std::exception& e) {
                        // Skip problematic entities
                    }
                }

                std::stringstream ss2;
                ss2 << "    → Full scan found " << newActors.size() << " actor entities";
                Utils::Log(ss2);

                Globals::actorCache.UpdateCache(newActors);

            } catch (const std::exception &e) {
                Utils::Log(std::string("Exception in Scanner Thread main loop: ") + e.what());
                consecutiveErrors++;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } catch (...) {
                Utils::Log("Unknown exception in Scanner Thread main loop.");
                consecutiveErrors++;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        Utils::Log("Entity Scanner Thread finished.");
    }

    // Dedicated reporter thread - runs at 30Hz to provide frequent updates
    void ReporterThreadFunc() {
        Utils::Log("Entity Reporter Thread started (30Hz update rate).");

        const auto reportInterval = std::chrono::milliseconds(33);
        auto nextReportTime = std::chrono::steady_clock::now();

        uintptr_t gEnvPtr = 0;
        std::shared_ptr<CRenderer> renderer = nullptr;

        while (Globals::runReporterThread) {
            try {
                auto now = std::chrono::steady_clock::now();
                if (now < nextReportTime) {
                    auto sleepTime = std::chrono::duration_cast<std::chrono::milliseconds>(nextReportTime - now);
                    if (sleepTime.count() > 0) {
                        std::this_thread::sleep_for(sleepTime);
                    }
                }

                nextReportTime = std::chrono::steady_clock::now() + reportInterval;
                Globals::reportCount++;

                if (!renderer && Globals::reportCount % 30 == 0) {
                    gEnvPtr = GetGEnvPtr();
                    if (gEnvPtr) {
                        GEnv gEnv(gEnvPtr);
                        renderer = gEnv.GetRenderer();
                        if (renderer) {
                            Utils::Log("ReporterThread: Successfully got CRenderer.");
                        }
                    }
                }

                std::vector<ActorData> currentActors = Globals::actorCache.GetCurrentActors();

                if (currentActors.empty()) {
                    if (Globals::reportCount % 30 == 0) {
                        Utils::Log("ReporterThread: No actors in cache.");
                    }
                    continue;
                }

                if (Globals::reportCount % 30 == 0) {
                    std::stringstream ss;
                    ss << "[*] Real-time report #" << Globals::reportCount << " - Tracking " << currentActors.size() << " actors";
                    Utils::Log(ss);
                }

                // Update real-time screen positions
                for (auto& actor : currentActors) {
                    if (Utils::IsValidPtr(reinterpret_cast<void*>(actor.entityPtr))) {
                        CEntity entity(actor.entityPtr);

                        if (entity.IsValid()) {
                            Vec3 currentPos = entity.GetWorldPos();
                            actor.rawPosition = currentPos;

                            // Get camera state for projection
                            CameraState camera = CameraHook::GetCameraState();

                            if (camera.isValid) {
                                // Use custom projection
                                actor.screenPos = WorldToScreen(
                                    currentPos,
                                    camera,
                                    fieldOfViewDegrees,
                                    Globals::screenResolution.width,
                                    Globals::screenResolution.height
                                );
                            } else if (renderer) {
                                // Fallback to renderer
                                Vec2 resolution(Globals::screenResolution.width, Globals::screenResolution.height);
                                actor.screenPos = renderer->ProjectToScreen(currentPos, resolution);
                            }
                        }
                    }
                }

                // CRITICAL: Write updated actors back to the shared cache
                Globals::actorCache.UpdateCache(currentActors);

            } catch (const std::exception &e) {
                Utils::Log(std::string("Exception in Reporter Thread: ") + e.what());
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } catch (...) {
                Utils::Log("Unknown exception in Reporter Thread.");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        Utils::Log("Entity Reporter Thread finished.");
    }

    void StartThreads() {
        if (Globals::scannerThreadHandle == nullptr) {
            Globals::runScannerThread = true;
            Globals::scannerThreadHandle = CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE) ScannerThreadFunc, nullptr, 0, nullptr);
            if (Globals::scannerThreadHandle) {
                Utils::Log("Entity Scanner thread created successfully.");
            } else {
                std::stringstream ss;
                ss << "Failed to create Entity Scanner thread. Error code: " << GetLastError();
                Utils::Log(ss);
            }
        }

        if (Globals::reporterThreadHandle == nullptr) {
            Globals::runReporterThread = true;
            Globals::reporterThreadHandle = CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE) ReporterThreadFunc, nullptr, 0, nullptr);
            if (Globals::reporterThreadHandle) {
                Utils::Log("Entity Reporter thread created successfully.");
            } else {
                std::stringstream ss;
                ss << "Failed to create Entity Reporter thread. Error code: " << GetLastError();
                Utils::Log(ss);
            }
        }
    }

    void StopThreads() {
        if (Globals::scannerThreadHandle != nullptr) {
            Utils::Log("Stopping Entity Scanner thread...");
            Globals::runScannerThread = false;
            DWORD waitResult = WaitForSingleObject(Globals::scannerThreadHandle, 5000);
            if (waitResult == WAIT_TIMEOUT) {
                Utils::Log("Warning: Entity Scanner thread did not exit gracefully within timeout. Terminating.");
                TerminateThread(Globals::scannerThreadHandle, 1);
            } else {
                Utils::Log("Entity Scanner thread joined successfully.");
            }
            CloseHandle(Globals::scannerThreadHandle);
            Globals::scannerThreadHandle = nullptr;
        }

        if (Globals::reporterThreadHandle != nullptr) {
            Utils::Log("Stopping Entity Reporter thread...");
            Globals::runReporterThread = false;
            DWORD waitResult = WaitForSingleObject(Globals::reporterThreadHandle, 5000);
            if (waitResult == WAIT_TIMEOUT) {
                Utils::Log("Warning: Entity Reporter thread did not exit gracefully within timeout. Terminating.");
                TerminateThread(Globals::reporterThreadHandle, 1);
            } else {
                Utils::Log("Entity Reporter thread joined successfully.");
            }
            CloseHandle(Globals::reporterThreadHandle);
            Globals::reporterThreadHandle = nullptr;
        }

        CameraHook::Shutdown();
        Globals::ClearEntityClassCache();
        Globals::ClearActorClasses();

        Utils::Log("All threads stopped and state reset.");
    }
} // namespace EntityScanner

// Create rotation matrix from angles
Mat3 MakeViewRotation(float pitch, float yaw, float roll) {
    Mat3 mat;

    float cx = cos(pitch);
    float sx = sin(pitch);
    float cy = cos(yaw);
    float sy = sin(yaw);
    float cz = cos(roll);
    float sz = sin(roll);

    mat.m[0][0] = cy * cz + sy * sx * sz;
    mat.m[0][1] = sz * cx;
    mat.m[0][2] = -sy * cz + cy * sx * sz;

    mat.m[1][0] = -cy * sz + sy * sx * cz;
    mat.m[1][1] = cz * cx;
    mat.m[1][2] = sz * sy + cy * sx * cz;

    mat.m[2][0] = sy * cx;
    mat.m[2][1] = -sx;
    mat.m[2][2] = cy * cx;

    return mat;
}

// World-to-screen projection function
Vec2 WorldToScreen(const Vec3& worldPos, const CameraState& camera, float fovX_deg, float screenW, float screenH) {
    Vec2 result(0, 0, 0, false);

    if (!camera.isValid) {
        return result;
    }

    // Calculate inverse view rotation matrix
    Mat3 R = MakeViewRotation(-camera.pitch, -camera.yaw, -camera.roll);

    // Vector from camera to target
    Vec3 toTarget;
    toTarget.x = worldPos.x - camera.position.x;
    toTarget.y = worldPos.y - camera.position.y;
    toTarget.z = worldPos.z - camera.position.z;

    // Transform to camera space
    Vec3 cam = R.MultiplyVec(toTarget);

    // Check if behind camera
    if (cam.z >= 0.0) {
        return result;
    }

    // Calculate projection parameters
    float aspect = screenW / screenH;
    float fx = 1.0f / tanf(fovX_deg * 0.5f * 3.14159f / 180.0f);
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

//=============================================================================
// ImGui DirectX Integration and Rendering
//=============================================================================

// Forward declare message handler from imgui_impl_win32.cpp
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

// Create render target
void CreateRenderTarget(IDXGISwapChain* pSwapChain) {
    ID3D11Texture2D* pBackBuffer;
    HRESULT hr = pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
    if (FAILED(hr)) {
        return;
    }

    hr = pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &mainRenderTargetView);
    pBackBuffer->Release();
    if (FAILED(hr)) {
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

// Select appropriate font based on scale factor
ImFont* GetScaledFont(float distanceScale) {
    // Scale down fonts for distant objects
    if (distanceScale < 0.7f) {
        return smallFont ? smallFont : ImGui::GetFont();
    }
    else if (distanceScale > 1.3f) {
        return largeFont ? largeFont : ImGui::GetFont();
    }
    else {
        return mediumFont ? mediumFont : ImGui::GetFont();
    }
}

// Initialize ImGui
bool InitImGui(IDXGISwapChain* pSwapChain) {
    // Get device and context
    HRESULT hr = pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&pDevice);
    if (FAILED(hr)) {
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

    // Load and setup multiple font sizes for scaling
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 1;
    fontConfig.PixelSnapH = true;

    // Default font remains the ImGui default
    defaultFont = ImGui::GetFont();

    // Load additional sizes (with font merging to keep glyphs from default font)
    smallFont = io.Fonts->AddFontDefault(&fontConfig);
    mediumFont = io.Fonts->AddFontDefault(&fontConfig);
    largeFont = io.Fonts->AddFontDefault(&fontConfig);

    // Build font atlas
    io.Fonts->Build();

    initialized = true;
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
}

// Hooked Present function
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

    // Get the display size
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 displaySize = io.DisplaySize;

    // Update global screen resolution if it changed
    if (Globals::screenResolution.width != displaySize.x ||
        Globals::screenResolution.height != displaySize.y) {
        Globals::screenResolution.width = displaySize.x;
        Globals::screenResolution.height = displaySize.y;
    }

    // ImGui overlay window
    {
        ImGui::Begin("ESP Config", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
        ImGui::Separator();

        // ESP Configuration Options
        ImGui::Checkbox("Show NPCs", &showNPCs);
        ImGui::Checkbox("Show Distance", &showDistance);
        ImGui::Checkbox("Show Boxes", &showBoxes);

        ImGui::SliderInt("Text Size", &textScale, 50, 200, "%d%%");
        ImGui::SliderFloat("Field of View", &fieldOfViewDegrees, 60.0f, 120.0f, "%.1f°");

        // Add camera info if available
        CameraState camera = CameraHook::GetCameraState();
        if (camera.isValid) {
            ImGui::Separator();
            ImGui::Text("Camera Status: Active");
            ImGui::Text("Pitch: %.1f° Yaw: %.1f° Roll: %.1f°",
                        camera.pitch * CameraHook::RAD_TO_DEG,
                        camera.yaw * CameraHook::RAD_TO_DEG,
                        camera.roll * CameraHook::RAD_TO_DEG);
            ImGui::Text("Position: %.2f, %.2f, %.2f",
                        camera.position.x, camera.position.y, camera.position.z);
        } else {
            ImGui::Text("Camera Status: Inactive");
        }

        ImGui::Separator();

        // Entity stats
        auto actors = Globals::actorCache.GetCurrentActors();
        int playerCount = 0;
        int npcCount = 0;

        for (const auto& actor : actors) {
            if (actor.className == "Player") {
                playerCount++;
            } else {
                npcCount++;
            }
        }

        ImGui::Text("Players: %d", playerCount);
        ImGui::Text("NPCs: %d", npcCount);
        ImGui::Text("Total: %d", playerCount + npcCount);

        ImGui::End();
    }

    // Get current actors from the global ActorCache
    std::vector<ActorData> actors = Globals::actorCache.GetCurrentActors();

    // Render each actor
    for (const auto& actor : actors) {
        // Skip NPCs if showNPCs is disabled
        if (!showNPCs && actor.className != "Player") {
            continue;
        }

        // Skip if screen position is invalid
        if (!actor.screenPos.IsValid(displaySize.x, displaySize.y)) {
            continue;
        }

        // Calculate distance
        float distance = 0.0f;
        std::string distanceText;
        if (showDistance) {
            // Use user player position for distance calculation instead of camera
            Vec3 userPos = Globals::GetUserPlayerPosition();
            distance = static_cast<float>(actor.rawP    osition.DistanceTo(userPos));
            std::stringstream ss;
            ss << std::fixed << std::setprecision(1) << " [" << distance << "m]";
            distanceText = ss.str();
        }

        // Set color based on actor type and distance
        ImVec4 textColor;
        if (actor.className == "Player") {
            // Players are yellow
            textColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
        } else {
            // NPCs are cyan
            textColor = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
        }

        // Calculate font scale based on distance and user preference
        float fontScale = 1.0f * (textScale / 100.0f);
        if (distance > 10.0f) {
            fontScale *= (1.0f - std::min((distance - 10.0f) / 300.0f, 0.6f));
        }

        // Select appropriate font based on scale
        ImFont* currentFont = GetScaledFont(fontScale);

        // Push the text color and font
        ImGui::PushStyleColor(ImGuiCol_Text, textColor);
        ImGui::PushFont(currentFont);

        // Create a window ID based on actor ID
        std::string windowName = "Actor" + std::to_string(actor.id);
        std::string displayText = actor.name + distanceText;

        // Calculate window position (centered on actor)
        ImVec2 textSize = ImGui::CalcTextSize(displayText.c_str());
        ImVec2 windowPos(actor.screenPos.x - (textSize.x / 2.0f), actor.screenPos.y - (textSize.y / 2.0f));

        // Create a transparent window for the text with no decorations
        ImGui::SetNextWindowPos(windowPos);
        ImGui::SetNextWindowBgAlpha(0.0f); // Transparent background
        ImGui::Begin(windowName.c_str(), nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav);

        // Display actor name
        ImGui::Text("%s", displayText.c_str());

        ImGui::End();

        // Draw bounding box if enabled
        if (showBoxes) {
            // Simple box dimensions based on text size
            float boxHeight = textSize.y * 1.5f;
            float boxWidth = textSize.x * 1.2f;

            ImVec2 boxMin(windowPos.x - boxWidth * 0.1f, windowPos.y - boxHeight * 0.1f);
            ImVec2 boxMax(boxMin.x + boxWidth, boxMin.y + boxHeight);

            ImGui::GetBackgroundDrawList()->AddRect(
                boxMin, boxMax,
                ImGui::ColorConvertFloat4ToU32(textColor),
                0.0f, 0, 1.0f
            );
        }

        // Pop style
        ImGui::PopFont();
        ImGui::PopStyleColor();
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
    }

    return result;
}

// Entry point for DLL
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        // Store module handle
        Globals::hModule = hModule;

        // Initialize logging
        Utils::InitLogging();
        Utils::Log("DLL Attached to process");

        // Create a detached thread for initialization to avoid deadlocks
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            // Initialize MinHook
            if (MH_Initialize() != MH_OK) {
                Utils::Log("Failed to initialize MinHook");
                return 1;
            }

            // Get address of Present and ResizeBuffers functions
            HWND tmpHwnd = CreateWindowA("STATIC", "TempWindow", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
            if (!tmpHwnd) {
                Utils::Log("Failed to create temporary window");
                return 1;
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
                Utils::Log("Failed to create temporary DirectX device");
                return 1;
            }

            // Get vtable addresses
            void** pSwapChainVTable = *reinterpret_cast<void***>(pTmpSwapChain);

            // Clean up temporary objects
            pTmpSwapChain->Release();
            pTmpDevice->Release();
            pTmpContext->Release();
            DestroyWindow(tmpHwnd);

            // Create hooks
            if (MH_CreateHook(pSwapChainVTable[8], reinterpret_cast<LPVOID>(&hkPresent), reinterpret_cast<LPVOID*>(&oPresent)) != MH_OK) {
                Utils::Log("Failed to create Present hook");
                return 1;
            }

            if (MH_CreateHook(pSwapChainVTable[13], reinterpret_cast<LPVOID>(&hkResizeBuffers), reinterpret_cast<LPVOID*>(&oResizeBuffers)) != MH_OK) {
                Utils::Log("Failed to create ResizeBuffers hook");
                return 1;
            }

            // Enable hooks
            if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
                Utils::Log("Failed to enable hooks");
                return 1;
            }

            Utils::Log("DirectX hooks initialized successfully");

            // Initialize entity detection system
            Globals::moduleBase = Utils::GetGameModuleBase("StarCitizen.exe");
            if (Globals::moduleBase != 0) {
                std::stringstream ss;
                ss << "Found StarCitizen.exe at 0x" << std::hex << Globals::moduleBase;
                Utils::Log(ss.str());

                // Initialize camera hook first
                if (CameraHook::Initialize(Globals::moduleBase)) {
                    Utils::Log("Camera hook system initialized");
                } else {
                    Utils::Log("Failed to initialize camera hook system");
                }

                // Start entity detection threads
                EntityScanner::StartThreads();
            } else {
                Utils::Log("Failed to find StarCitizen.exe module");
            }

            return 0;
        }, nullptr, 0, nullptr);
    }
    else if (dwReason == DLL_PROCESS_DETACH) {
        Utils::Log("DLL detaching from process");

        // Clean up entity detection system
        EntityScanner::StopThreads();

        // Clean up ImGui
        ShutdownImGui();

        // Disable and uninitialize MinHook
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();

        // Close logging
        Utils::ShutdownLogging();
    }

    return TRUE;
}
