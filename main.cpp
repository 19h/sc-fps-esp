#define WIN32_LEAN_AND_MEAN
//  Win32 / CRT
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <string_view>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <string>
#include <list>
#include <sstream>
#include <cmath>
#include <atomic>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include "imgui.h"
#include <Zydis/Zydis.h>
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

// External ImGui WndProc handler declaration
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// =================================================================
// Basic Vector Structures & Memory Helpers
// =================================================================

constexpr inline uintptr_t k48ByteMask = 0xFFFFFFFFFFFFULL;

template <typename T>
constexpr inline std::uintptr_t ExtractLower48Bytes(T* ptr) noexcept {
    return reinterpret_cast<uintptr_t>(ptr) & k48ByteMask;
}

inline uintptr_t GetFuncAddr(uintptr_t offset) {
    return reinterpret_cast<uintptr_t>(GetModuleHandle(0)) + offset;
}

struct Vec2 {
    float x, y, z; // z depth value for visibility test
    bool success;  // Whether projection was successful

    Vec2() : x(0), y(0), z(0), success(false) {}
    Vec2(float _x, float _y, float _z = 0, bool _success = true) : x(_x), y(_y), z(_z), success(_success) {}

    bool IsValid(float maxW,float maxH) const{
        if(!success)                      return false;
        if(std::isnan(x)||std::isnan(y))  return false;
        if(x < 0 || y < 0)                return false;
        if(x > maxW || y > maxH)          return false;
        return true;
    }
};

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

    Vec3 operator+(const Vec3& other) const {
        return Vec3(x + other.x, y + other.y, z + other.z);
    }

    Vec3 operator*(double scalar) const {
        return Vec3(x * scalar, y * scalar, z * scalar);
    }

    friend Vec3 operator*(double scalar, const Vec3& vec) {
        return vec * scalar;
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

struct Quaternion {
    float x, y, z, w;

    Quaternion() : x(0), y(0), z(0), w(1) {}
    Quaternion(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}

    Vec3 RotateVector(const Vec3& v) const {
        Vec3 u(x, y, z);
        float s = w;
        float uvDot2 = 2.0f * (u.x * v.x + u.y * v.y + u.z * v.z);
        float s2 = 2.0f * s;
        return Vec3(
            v.x + s2 * (u.y * v.z - u.z * v.y) + uvDot2 * u.x,
            v.y + s2 * (u.z * v.x - u.x * v.z) + uvDot2 * u.y,
            v.z + s2 * (u.x * v.y - u.y * v.x) + uvDot2 * u.z
        );
    }
};

// =================================================================
// Forward declarations
// =================================================================
class Hooking; // Forward declaration for Hooking class

// =================================================================
// Player/Entity Information Structures
// =================================================================

struct PlayerInfo {
    size_t index{};
    int64_t id{};
    std::string name{};
    Vec3 pos{};
    bool isPlayer{false};
    bool isLootable{false}; // New flag for lootable containers
};

struct ESPInfo {
    std::string name{};
    Vec3 pos{};
    int64_t id{0};      // Entity ID for tracking across frames
    bool isPlayer{false};
    bool isLootable{false}; // New flag for lootable containers
    float lastUpdateTime{0.0f};  // Timestamp of last update

    ESPInfo(const std::string& name, const Vec3& pos, bool isPlayer = false, bool isLootable = false)
        : name(name), pos(pos), isPlayer(isPlayer), isLootable(isLootable), id(0) {
        lastUpdateTime = static_cast<float>(GetTickCount64()) * 0.001f;  // Convert to seconds
    }

    ESPInfo(const PlayerInfo& player_info)
        : name(player_info.name), pos(player_info.pos),
          id(player_info.id), isPlayer(player_info.isPlayer),
          isLootable(player_info.isLootable) {
        lastUpdateTime = static_cast<float>(GetTickCount64()) * 0.001f;
    }
};

// Global storage for player information
static std::list<PlayerInfo> global_memory_player_info;

// =================================================================
// ESP Configuration
// =================================================================

struct ESPConfig {
    bool showPlayers = true;
    bool showNPCs = false;
    bool showLootables = false;
    bool showDistance = false;
    bool showBoxes = false;
    bool showWorldPosition = false;
    bool showCameraInfo = false;
    bool showConfigWindow = true;
    int textScale = 100;
    float fieldOfViewDegrees = 90.0f;
    float maxDistance = 2000.0f; // New field for maximum entity draw distance

    // Settings for 3D boxes
    bool show3DBoxes = false;         // Enable 3D entity boxes
    float boxHeight = 1.7f;          // 1.7 meters height (average person)
    float boxWidth = 0.5f;           // 50cm width
    float boxDepth = 0.2f;           // 20cm depth
    float boxThickness = 1.0f;       // Line thickness for 3D boxes

    ImU32 playerColor = IM_COL32(255, 255, 0, 255);     // Yellow for players
    ImU32 npcColor = IM_COL32(0, 255, 255, 255);        // Cyan for NPCs
    ImU32 npcPUColor = IM_COL32(200, 200, 200, 255);    // Gray for PU_ NPCs
    ImU32 npcOtherColor = IM_COL32(255, 0, 0, 255);     // Red for other NPCs
    ImU32 lootableColor = IM_COL32(0, 255, 0, 255);     // Green for lootables
    ImU32 textOutlineColor = IM_COL32(0, 0, 0, 200);    // Black outline for text
};

static ESPConfig config;

// =================================================================
// Function Pointer Declarations
// =================================================================

namespace OriginalFunctions {
    using tPresent = HRESULT(__stdcall*)(IDXGISwapChain*, uint32_t, uint32_t);
    using tResizeBuffers = HRESULT(__stdcall*)(IDXGISwapChain*, uint32_t, uint32_t, uint32_t, DXGI_FORMAT, uint32_t);
    using tWndProc = LRESULT(CALLBACK*)(HWND, uint32_t, WPARAM, LPARAM);
    using tCEntitySystem__Update = double(__fastcall*)(int64_t);

    // Function pointers for original functions
    inline bool (*ProjectToScreenStub)(void*, double, double, double, float*, float*, float*, char, int64_t);
    inline int64_t (*CEntity__GetWorldPos_stub)(void*, void*, int8_t);
    inline uintptr_t (*CEntityClassRegistry__FindClass_stub)(void*, const char*);

    inline tPresent oPresent = nullptr;
    inline tResizeBuffers oResizeBuffers = nullptr;
    inline tWndProc oWndProc = nullptr;
    inline tCEntitySystem__Update oCEntitySystem__Update = nullptr;
}

// =================================================================
// Game-Specific Classes
// =================================================================

class CEntityClass {
public:
    virtual void Function0();

    int64_t flags_;      // 0x0008
    char* name_;         // 0x0010
    char pad_0018[120];  // 0x0018
};

class CZone {};

class CEntity {
public:
    CEntityClass* GetEntityClass() {
        return reinterpret_cast<CEntityClass*>(ExtractLower48Bytes(entity_class_));
    }

    void GetWorldPos(Vec3* res) {
        OriginalFunctions::CEntity__GetWorldPos_stub(this, res, 0);
    }

    Vec3 GetWorldPos() {
        Vec3 res;
        GetWorldPos(&res);
        return res;
    }

    virtual void Function0();

    int64_t flags_;                     // 0x0008
    int64_t id_;                        // 0x0010
    char pad_0018[8];                   // 0x0018
    class CEntityClass* entity_class_;  // 0x0020
    char pad_0028[200];                 // 0x0028
    double x_local_;                    // 0x00F0
    double y_local_;                    // 0x00F8
    double z_local_;                    // 0x0100
    char pad_0108[392];                 // 0x0108
    const char* name_;                  // 0x0290
    char pad_0298[16];                  // 0x0298
    class CZone* zone_;                 // 0x02A8
    char pad_02B0[1888];                // 0x02B0
};

class CEntityArray {
public:
    CEntity*& operator[](size_t i) {
        return data_[i];
    }

    CEntity* operator[](size_t i) const {
        return data_[i];
    }

    int64_t max_size_;   // 0x0000
    int64_t curr_size_;  // 0x0008
    CEntity** junk_;     // 0x0010
    CEntity** data_;     // 0x0018
};

class CEntityClassRegistry {
public:
    virtual void Function0();
    virtual void Function1();
    virtual void Function2();
    virtual void Function3();
    virtual CEntityClass* FindClass(const char* name);
};

class CEntitySystem {
public:
    virtual void Function0();
    virtual void Function1();
    virtual void Function2();
    virtual void Function3();
    virtual void Function4();
    virtual void Function5();
    virtual void Function6();
    virtual void Function7();
    virtual void Function8();
    virtual void Function9();
    virtual void Function10();
    virtual void Function11();
    virtual void Function12();
    virtual void Function13();
    virtual void Function14();
    virtual void Function15();
    virtual void Function16();
    virtual void Function17();
    virtual void Function18();
    virtual void Function19();
    virtual void Function20();
    virtual void Function21();
    virtual void Function22();
    virtual void Function23();
    virtual CEntityClassRegistry* GetClassRegistry();

    char pad_0008[272];                                  // 0x0008
    class CEntityArray entity_array_;                    // 0x0118
    char pad_0138[1440];                                 // 0x0138
    class CEntityClassRegistry* entity_class_registry_;  // 0x06D8
};

class CSystem {
public:
    char pad_0000[48];           // 0x0000
    double x_camera_forward_;    // 0x0030
    double x_camera_up_;         // 0x0038
    double x_camera_world_pos_;  // 0x0040
    char pad_0048[8];            // 0x0048
    double y_camera_forward_;    // 0x0050
    double y_camera_up_;         // 0x0058
    double y_camera_world_pos_;  // 0x0060
    char pad_0068[8];            // 0x0068
    double z_camera_forward_;    // 0x0070
    double z_camera_up_;         // 0x0078
    double z_camera_world_pos_;  // 0x0080
    char pad_0088[144];          // 0x0088
    float internal_xfov_;        // 0x0118

    Vec3 GetCameraForward() const {
        return Vec3(x_camera_forward_, y_camera_forward_, z_camera_forward_);
    }

    Vec3 GetCameraUp() const {
        return Vec3(x_camera_up_, y_camera_up_, z_camera_up_);
    }

    Vec3 GetCameraWorldPos() const {
        return Vec3(x_camera_world_pos_, y_camera_world_pos_, z_camera_world_pos_);
    }

    float GetInternalXFOV() const {
        return internal_xfov_;
    }

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

class GEnv {
public:
    char pad_0000[160];                   // 0x0000
    class CEntitySystem* entity_system_;  // 0x00A0
    char pad_00A8[24];                    // 0x00A8
    class CSystem* system_;               // 0x00C0
    char pad_00C8[48];                    // 0x00C8
    void* renderer_;                      // 0x00F8
    char pad_0100[832];                   // 0x0100
};

// =================================================================
// Global Settings & Game Environment
// =================================================================

namespace SCOffsets {
    inline uintptr_t GEnv = 0x981D200;
}

inline GEnv& g_env = *reinterpret_cast<GEnv*>(GetFuncAddr(SCOffsets::GEnv));

// Global tracking data
namespace Globals {
    // Screen resolution cache
    struct ScreenResolution {
        float width = 1920.0f;
        float height = 1080.0f;
    };
    ScreenResolution screenResolution;

    // Camera state cache
    Vec3 cameraPosition{0, 0, 0};
    Quaternion cameraRotation;
    float cameraFOV = 90.0f;

    // Statistics
    std::atomic<size_t> entityCount{0};
    std::atomic<size_t> actorCount{0};
    std::atomic<size_t> playerCount{0};
    std::atomic<size_t> frameCount{0};

    // Update camera info from system
    void UpdateCameraInfo() {
        CSystem* system = g_env.system_;
        if (system) {
            cameraPosition = system->GetCameraWorldPos();
            cameraRotation = system->GetCameraQuaternion();
            cameraFOV = system->GetInternalXFOV();
        }
    }
}

// =================================================================
// Hooking Class Definition
// =================================================================

class Hooking {
public:
    Hooking() = default;
    ~Hooking() = default;

    bool Initialize();
    bool Uninitialize();

    void HookPresentAndResizeBuffers();
    void VanityHooks();
    static void HookWndProc();
    void HookCEntitySystem__Update();
    static void UnhookWndProc();

    // Made public for accessibility
    uintptr_t GetFuncAddr(uintptr_t offset) {
        return reinterpret_cast<uintptr_t>(GetModuleHandle(0)) + offset;
    }

protected:
    template <typename TDetourFunc, typename TOrigFunc>
    bool HookFunction(uintptr_t func_addr, TDetourFunc detour_func, TOrigFunc &orig_func);

    MODULEINFO mod_info_{};
};

// =================================================================
// ESP Overlay Renderer Class
// =================================================================

class OverlayRenderer {
public:
    // --- Rendering Helpers ---
    void RenderText(ImDrawList* draw_list, const ImVec2& pos, ImU32 color, const char* text, float scale = 1.0f) {
        if (!draw_list || !text) return;

        // Draw text outline
        draw_list->AddText(
            NULL, // Default font
            ImGui::GetFontSize() * scale,
            ImVec2(pos.x + 1, pos.y + 1),
            config.textOutlineColor,
            text
        );

        // Draw main text
        draw_list->AddText(
            NULL, // Default font
            ImGui::GetFontSize() * scale,
            pos,
            color,
            text
        );
    }

    bool WorldToScreen(const Vec3& pos, Vec2* out, const Vec2& resolution, bool is_player_viewport_relative = false) {
        if (!g_env.renderer_ || !g_env.system_) {
            out->success = false;
            return false;
        }

        float x, y, z;

        if (OriginalFunctions::ProjectToScreenStub(
                g_env.renderer_,
                pos.x,
                pos.y,
                pos.z,
                &x,
                &y,
                &z,
                is_player_viewport_relative,
                0)) {
            // Scale normalized coordinates to screen space
            // The game returns values in the range 0-100 (percentage of screen)
            out->x = x * (resolution.x / 100.0f);
            out->y = y * (resolution.y / 100.0f);
            out->z = z;

            // Success criteria based on depth
            // For this game, z < 0.01f means the position is in front of the camera
            // This is an empirical value specific to this engine
            //out->success = (z < 0.01f);
            out->success = (z >= 0.0f && z <= 1.0f);

            return true;
        }

        out->success = false;
        return false;
    }


    // Alternative world-to-screen implementation using quaternion rotation
    Vec2 WorldToScreenQuaternion(
        const Vec3& worldPos,
        const Vec3& cameraPos,
        const Quaternion& cameraRotation,
        float fovX,
        float screenW,
        float screenH)
    {
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

    // --- D3D Resource Management ---
    void CleanupRenderTarget() {
        if (main_render_targetview_) {
            main_render_targetview_->Release();
            main_render_targetview_ = nullptr;
        }
        if (original_render_targetview_) {
            original_render_targetview_->Release();
            original_render_targetview_ = nullptr;
        }
    }

    void CleanupDevice() {
        if (device_) {
            device_->Release();
            device_ = nullptr;
        }
    }

    void CleanupDeviceContext() {
        if (device_context_) {
            device_context_->Release();
            device_context_ = nullptr;
        }
    }

    void CleanupDepthStencilView() {
        if (original_depth_stencilview_) {
            original_depth_stencilview_->Release();
            original_depth_stencilview_ = nullptr;
        }
    }

    void CreateMainRenderTargetView() {
        CleanupRenderTarget();
        if (!swap_chain_) {
            std::cout << "[ERROR] Cannot create RTV: SwapChain is null\n";
            return;
        }
        if (!device_) {
            std::cout << "[ERROR] Cannot create RTV: Device is null\n";
            return;
        }

        ID3D11Texture2D* back_buffer = nullptr;
        if (SUCCEEDED(swap_chain_->GetBuffer(
                0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer)))) {
            if (FAILED(
                    device_->CreateRenderTargetView(back_buffer, nullptr, &main_render_targetview_))) {
                std::cout << "[ERROR] Failed to create main render target view\n";
                main_render_targetview_ = nullptr;  // Ensure it's null on failure
            }
            back_buffer->Release();
        } else {
            std::cout << "[ERROR] Failed to get back buffer from swap chain\n";
        }
    }

    // --- ImGui Initialization & Shutdown ---
    void InitializeImGui() {
        std::cout << "[INFO] Attempting ImGui Initialization...\n";

        if (!swap_chain_) {
            std::cout << "[ERROR] ImGui Init Failed: SwapChain is null.\n";
            return;
        }

        // Get swap chain description to find the window handle
        DXGI_SWAP_CHAIN_DESC desc;
        if (FAILED(swap_chain_->GetDesc(&desc))) {
            std::cout << "[ERROR] ImGui Init Failed: Could not get SwapChain description.\n";
            return;
        }
        window_ = desc.OutputWindow;
        if (!window_) {
            std::cout << "[ERROR] ImGui Init Failed: Could not get window handle from SwapChain.\n";
            return;
        }
        std::cout << "[INFO] Game Window HWND: " << window_ << "\n";

        // Get the Direct3D device from the swap chain
        if (FAILED(
                swap_chain_->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device_)))) {
            std::cout << "[ERROR] ImGui Init Failed: Could not get D3D11Device from SwapChain.\n";
            window_ = nullptr;  // Reset window if device fails
            return;
        }
        std::cout << "[INFO] D3D11 Device obtained: " << device_ << "\n";

        // Get the device context
        device_->GetImmediateContext(&device_context_);
        if (!device_context_) {
            std::cout << "[ERROR] ImGui Init Failed: Could not get Immediate Device Context.\n";
            CleanupDevice();
            window_ = nullptr;
            return;
        }
        std::cout << "[INFO] D3D11 Device Context obtained: " << device_context_ << "\n";

        // Create the main render target view needed for rendering
        CreateMainRenderTargetView();
        if (!main_render_targetview_) {
            std::cout << "[ERROR] ImGui Init Failed: Could not create main render target view.\n";
            CleanupDeviceContext();
            CleanupDevice();
            window_ = nullptr;
            return;
        }

        // Initialize ImGui
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();

        // Enable Keyboard Controls
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;  // Don't save imgui.ini settings

        ImGui::StyleColorsDark();

        // Initialize ImGui Platform and Renderer backends
        if (!ImGui_ImplWin32_Init(window_)) {
            std::cout << "[ERROR] Failed ImGui_ImplWin32_Init\n";
            ShutdownImGui();
            return;
        }

        if (!ImGui_ImplDX11_Init(device_, device_context_)) {
            std::cout << "[ERROR] Failed ImGui_ImplDX11_Init\n";
            ShutdownImGui();
            return;
        }

        Hooking::HookWndProc();  // Hook AFTER ImGui_ImplWin32_Init

        std::cout << "[SUCCESS] ImGui Initialized Successfully\n";
        is_initialized_ = true;
    }

    void ShutdownImGui() {
        if (!is_initialized_ && !device_ && !device_context_ && !main_render_targetview_ &&
            !ImGui::GetCurrentContext()) {
            std::cout << "[INFO] ImGui already shutdown or never initialized.\n";
            return;  // Nothing to clean up
        }

        std::cout << "[INFO] Shutting down ImGui...\n";

        Hooking::UnhookWndProc();  // Unhook BEFORE shutting down ImGui Win32 backend

        // Check if context exists before trying to access IO
        if (ImGui::GetCurrentContext()) {
            ImGuiIO& io = ImGui::GetIO();
            // Shutdown backends if they were initialized
            if (io.BackendRendererUserData) {
                ImGui_ImplDX11_Shutdown();
                std::cout << "[INFO] ImGui DX11 Backend Shutdown.\n";
            }
            if (io.BackendPlatformUserData) {
                ImGui_ImplWin32_Shutdown();
                std::cout << "[INFO] ImGui Win32 Backend Shutdown.\n";
            }
            ImGui::DestroyContext();
            std::cout << "[INFO] ImGui Context Destroyed.\n";
        } else {
            std::cout << "[WARN] ImGui Context does not exist, cannot perform backend shutdown or "
                      "context destruction.\n";
        }

        // Release D3D resources
        CleanupRenderTarget();
        CleanupDepthStencilView();
        CleanupDeviceContext();
        CleanupDevice();

        // Reset state variables
        window_ = nullptr;
        swap_chain_ = nullptr;
        is_initialized_ = false;

        std::cout << "[INFO] ImGui Shutdown Complete\n";
    }

    // =================================================================
    // Enhanced 3D Bounding Box Implementation
    // =================================================================

    static inline Vec3 CrossProduct(const Vec3& a, const Vec3& b)
    {
        return Vec3{ a.y*b.z - a.z*b.y,
                     a.z*b.x - a.x*b.z,
                     a.x*b.y - a.y*b.x };
    }

    /**
     * Builds axis-aligned bounding box corners in world space.
     *
     * @param entityFeetWorld    World position of entity's feet (bottom center of the box)
     * @param boxWidthMetres     Width of the box in meters (X dimension)
     * @param boxDepthMetres     Depth of the box in meters (Y dimension)
     * @param boxHeightMetres    Height of the box in meters (Z dimension)
     * @param outWorldCorners    Output array for the 8 corner positions
     */
    inline void BuildAxisAlignedBoundingBoxCorners(const Vec3& entityFeetWorld,
                                                  float        boxWidthMetres,
                                                  float        boxDepthMetres,
                                                  float        boxHeightMetres,
                                                  Vec3         outWorldCorners[8])
    {
        const float halfWidth  = 0.5f * boxWidthMetres;
        const float halfDepth  = 0.5f * boxDepthMetres;

        // Box feet positioned at entity's feet (Z coordinate is bottom of box)
        const Vec3 minCorner = {
            entityFeetWorld.x - halfWidth,
            entityFeetWorld.y - halfDepth,
            entityFeetWorld.z // feet position
        };

        const Vec3 maxCorner = {
            entityFeetWorld.x + halfWidth,
            entityFeetWorld.y + halfDepth,
            entityFeetWorld.z + boxHeightMetres
        };

        // Define all 8 corners of the box
        outWorldCorners[0] = {minCorner.x, minCorner.y, minCorner.z}; // front-left-bottom
        outWorldCorners[1] = {maxCorner.x, minCorner.y, minCorner.z}; // front-right-bottom
        outWorldCorners[2] = {maxCorner.x, maxCorner.y, minCorner.z}; // back-right-bottom
        outWorldCorners[3] = {minCorner.x, maxCorner.y, minCorner.z}; // back-left-bottom
        outWorldCorners[4] = {minCorner.x, minCorner.y, maxCorner.z}; // front-left-top
        outWorldCorners[5] = {maxCorner.x, minCorner.y, maxCorner.z}; // front-right-top
        outWorldCorners[6] = {maxCorner.x, maxCorner.y, maxCorner.z}; // back-right-top
        outWorldCorners[7] = {minCorner.x, maxCorner.y, maxCorner.z}; // back-left-top
    }

    /**
     * Draws a world-aligned 3D box around an entity by projecting each corner to screen space.
     * This implementation creates an axis-aligned box in world coordinates and projects
     * each corner to screen space, preventing unwanted tilting during camera movement.
     *
     * @param drawingList          ImGui draw list to render onto
     * @param entityFeetWorld      World position of entity's feet (bottom center of the box)
     * @param entityHeightMetres   Height of the box in meters (Z dimension)
     * @param entityWidthMetres    Width of the box in meters (X dimension)
     * @param entityDepthMetres    Depth of the box in meters (Y dimension)
     * @param lineColour           Color of the box lines
     * @param lineThickness        Thickness of the box lines
     * @return                     True if any part of the box was drawn on screen
     */
    bool DrawEntityBox(ImDrawList* drawingList,
                                      const Vec3& entityFeetWorld,
                                      float       entityHeightMetres,
                                      float       entityWidthMetres,
                                      float       entityDepthMetres,
                                      ImU32       lineColour,
                                      float       lineThickness)
    {
        if (!drawingList) return false;

        // Build eight world-space vertices
        Vec3 worldCorners[8];
        BuildAxisAlignedBoundingBoxCorners(entityFeetWorld,
                                          entityWidthMetres,
                                          entityDepthMetres,
                                          entityHeightMetres,
                                          worldCorners);

        // Project every vertex to screen space
        Vec2 screenCorners[8];
        float screenW = Globals::screenResolution.width;
        float screenH = Globals::screenResolution.height;

        int visibleCorners = 0;
        for (int i = 0; i < 8; ++i) {
            if (WorldToScreen(worldCorners[i], &screenCorners[i], {screenW, screenH}) &&
                screenCorners[i].IsValid(screenW, screenH)) {
                ++visibleCorners;
            }
        }

        // If no corners are visible, don't draw anything
        if (visibleCorners == 0) {
            return false;
        }

        // Define the 12 edges of the box (connecting pairs of vertices)
        static constexpr int edgePairs[12][2] = {
            {0,1},{1,2},{2,3},{3,0},  // bottom face
            {4,5},{5,6},{6,7},{7,4},  // top face
            {0,4},{1,5},{2,6},{3,7}   // vertical pillars
        };

        // Draw the edges
        bool anyEdgeDrawn = false;
        for (const auto& edge : edgePairs) {
            int a = edge[0], b = edge[1];
            if (screenCorners[a].success && screenCorners[b].success) {
                drawingList->AddLine(
                    ImVec2(screenCorners[a].x, screenCorners[a].y),
                    ImVec2(screenCorners[b].x, screenCorners[b].y),
                    lineColour,
                    lineThickness);
                anyEdgeDrawn = true;
            }
        }

        return anyEdgeDrawn;
    }

    /**
     * Builds oriented (non-axis-aligned) bounding box corners in world space.
     * Useful for entities that have rotation and need boxes aligned to their orientation.
     *
     * @param entityFeetWorld     World position of entity's feet (bottom center of the box)
     * @param boxWidthMetres      Width of the box in meters
     * @param boxDepthMetres      Depth of the box in meters
     * @param boxHeightMetres     Height of the box in meters
     * @param forwardDir          Entity's forward direction vector (normalized)
     * @param rightDir            Entity's right direction vector (normalized)
     * @param upDir               Up direction vector (usually world up: 0,0,1)
     * @param outWorldCorners     Output array for the 8 corner positions
     */
    inline void BuildOrientedBoundingBoxCorners(const Vec3& entityFeetWorld,
                                               float        boxWidthMetres,
                                               float        boxDepthMetres,
                                               float        boxHeightMetres,
                                               const Vec3&  forwardDir,
                                               const Vec3&  rightDir,
                                               const Vec3&  upDir,
                                               Vec3         outWorldCorners[8])
    {
        const float halfWidth = 0.5f * boxWidthMetres;
        const float halfDepth = 0.5f * boxDepthMetres;

        // Scaled direction vectors
        const Vec3 scaledRight = rightDir.Normalized() * halfWidth;
        const Vec3 scaledForward = forwardDir.Normalized() * halfDepth;
        const Vec3 scaledUp = upDir.Normalized() * boxHeightMetres;

        // Base corner (entity's feet position)
        const Vec3 baseFeet = entityFeetWorld;

        // Calculate bottom four corners
        outWorldCorners[0] = baseFeet - scaledRight - scaledForward;          // front-left-bottom
        outWorldCorners[1] = baseFeet + scaledRight - scaledForward;          // front-right-bottom
        outWorldCorners[2] = baseFeet + scaledRight + scaledForward;          // back-right-bottom
        outWorldCorners[3] = baseFeet - scaledRight + scaledForward;          // back-left-bottom

        // Calculate top four corners by adding height vector
        outWorldCorners[4] = outWorldCorners[0] + scaledUp;                  // front-left-top
        outWorldCorners[5] = outWorldCorners[1] + scaledUp;                  // front-right-top
        outWorldCorners[6] = outWorldCorners[2] + scaledUp;                  // back-right-top
        outWorldCorners[7] = outWorldCorners[3] + scaledUp;                  // back-left-top
    }

    /**
     * Draws an oriented 3D box around an entity based on its direction.
     * This implementation creates a box aligned with the entity's forward/right directions.
     *
     * @param drawingList          ImGui draw list to render onto
     * @param entityFeetWorld      World position of entity's feet (bottom center)
     * @param entityHeightMetres   Height of the box in meters (Z dimension)
     * @param entityWidthMetres    Width of the box in meters (aligned with entity's right)
     * @param entityDepthMetres    Depth of the box in meters (aligned with entity's forward)
     * @param entityForwardDir     Entity's forward direction vector
     * @param lineColour           Color of the box lines
     * @param lineThickness        Thickness of the box lines
     * @return                     True if any part of the box was drawn on screen
     */
    bool DrawOrientedEntityBox(ImDrawList* drawingList,
                                              const Vec3&   entityFeetWorld,
                                              float         entityHeightMetres,
                                              float         entityWidthMetres,
                                              float         entityDepthMetres,
                                              const Vec3&   entityForwardDir,
                                              ImU32         lineColour,
                                              float         lineThickness)
    {
        if (!drawingList) return false;

        // Calculate entity's right vector from forward and world up
        const Vec3 worldUp(0, 0, 1); // Z-up coordinate system
        Vec3 entityRightDir = CrossProduct(worldUp, entityForwardDir).Normalized();

        // Handle degenerate case (if entity is looking straight up/down)
        if (entityRightDir.Length() < 0.001f) {
            entityRightDir = Vec3(1, 0, 0); // Default to world X-axis
        }

        // Build eight world-space vertices with orientation
        Vec3 worldCorners[8];
        BuildOrientedBoundingBoxCorners(entityFeetWorld,
                                       entityWidthMetres,
                                       entityDepthMetres,
                                       entityHeightMetres,
                                       entityForwardDir,
                                       entityRightDir,
                                       worldUp,
                                       worldCorners);

        // Project every vertex to screen space
        Vec2 screenCorners[8];
        float screenW = Globals::screenResolution.width;
        float screenH = Globals::screenResolution.height;

        int visibleCorners = 0;
        for (int i = 0; i < 8; ++i) {
            if (WorldToScreen(worldCorners[i], &screenCorners[i], {screenW, screenH}) &&
                screenCorners[i].IsValid(screenW, screenH)) {
                ++visibleCorners;
            }
        }

        // If no corners are visible, don't draw anything
        if (visibleCorners == 0) {
            return false;
        }

        // Define the 12 edges of the box (connecting pairs of vertices)
        static constexpr int edgePairs[12][2] = {
            {0,1},{1,2},{2,3},{3,0},  // bottom face
            {4,5},{5,6},{6,7},{7,4},  // top face
            {0,4},{1,5},{2,6},{3,7}   // vertical pillars
        };

        // Draw the edges
        bool anyEdgeDrawn = false;
        for (const auto& edge : edgePairs) {
            int a = edge[0], b = edge[1];
            if (screenCorners[a].success && screenCorners[b].success) {
                drawingList->AddLine(
                    ImVec2(screenCorners[a].x, screenCorners[a].y),
                    ImVec2(screenCorners[b].x, screenCorners[b].y),
                    lineColour,
                    lineThickness);
                anyEdgeDrawn = true;
            }
        }

        return anyEdgeDrawn;
    }

    // --- Rendering Logic ---
    void RenderPlayerESP(ImDrawList* draw_list) {
        if (!draw_list) return;

        // Get screen dimensions
        float screenWidth = Globals::screenResolution.width;
        float screenHeight = Globals::screenResolution.height;

        // Display player count in top-left corner
        std::stringstream headerSS;
        headerSS << "[Players: " << Globals::playerCount.load()
                 << " | NPCs: " << (Globals::actorCount.load() - Globals::playerCount.load())
                 << " | Total: " << Globals::entityCount.load() << "]";
        std::string headerText = headerSS.str();

        ImVec2 esp_header_pos(10.f, 10.f);
        RenderText(draw_list, esp_header_pos, IM_COL32(255, 255, 255, 255), headerText.c_str(), 1.0f);

        // Display camera info if enabled
        if (config.showCameraInfo) {
            std::stringstream cameraSS;
            cameraSS << std::fixed << std::setprecision(1);
            cameraSS << "Camera: ("
                    << Globals::cameraPosition.x << ", "
                    << Globals::cameraPosition.y << ", "
                    << Globals::cameraPosition.z << ")";

            std::string cameraText = cameraSS.str();
            ImVec2 camera_info_pos(10.f, 30.f);
            RenderText(draw_list, camera_info_pos, IM_COL32(200, 200, 255, 255), cameraText.c_str(), 0.9f);

            // Display FOV info
            std::stringstream fovSS;
            fovSS << "FOV: " << Globals::cameraFOV * (180.0f/3.14159f) << "°";
            std::string fovText = fovSS.str();
            ImVec2 fov_info_pos(10.f, 50.f);
            RenderText(draw_list, fov_info_pos, IM_COL32(200, 200, 255, 255), fovText.c_str(), 0.9f);
        }

        // Access player information from global list
        std::lock_guard<std::mutex> lock_player_info(player_info_mutex_);

        // Process each entity
        for (ESPInfo& entity_info : drawing_esp_info_) {
            // Skip entities based on settings
            if (entity_info.isPlayer && !config.showPlayers) continue;
            if (!entity_info.isPlayer && !entity_info.isLootable && !config.showNPCs) continue;
            if (entity_info.isLootable && !config.showLootables) continue;

            // Calculate distance early for filtering and display
            float distance = static_cast<float>(entity_info.pos.DistanceTo(Globals::cameraPosition));

            // Skip entities beyond maximum distance (only if maxDistance > 0)
            if (config.maxDistance > 0.0f && distance > config.maxDistance) continue;

            // Use the raw position for rendering
            const Vec3& renderPos = entity_info.pos;

            // Use the standardized WorldToScreen function
            Vec2 sp;
            if (WorldToScreen(renderPos, &sp, {screenWidth, screenHeight})) {
                // Validate screen position and continue only if on screen
                if (!sp.IsValid(screenWidth, screenHeight)) continue;

                // Get position coordinates
                float x = sp.x;
                float y = sp.y;

                // --- 3D Box Logic ---
                if (config.show3DBoxes) {
                    ImU32 boxColor;
                    if (entity_info.isPlayer) {
                        boxColor = config.playerColor;
                    } else if (entity_info.isLootable) {
                        boxColor = config.lootableColor;
                    } else { // NPC
                        bool isPU_NPC_for_box = entity_info.name.find("PU_") != std::string::npos;
                        boxColor = isPU_NPC_for_box ? config.npcPUColor : config.npcOtherColor;
                    }
                    // Scale thickness based on distance
                    float scaledThickness = std::max(0.5f, config.boxThickness * (1.0f - distance/2000.0f));
                    DrawEntityBox(
                        draw_list,
                        renderPos,
                        config.boxHeight,
                        config.boxWidth,
                        config.boxDepth,
                        boxColor,
                        scaledThickness
                    );
                }

                // --- Text, Color, and Bolding Logic ---
                std::string displayText;
                ImU32 textColor;
                bool applyBold = false;

                if (entity_info.isPlayer) {
                    std::stringstream playerTextSS;
                    playerTextSS << entity_info.name;
                    if (config.showDistance) {
                        playerTextSS << std::endl << std::fixed << std::setprecision(1) << distance << "M"; // "M" for meters
                    }
                    // World position for players is omitted as per screenshot style
                    displayText = playerTextSS.str();
                    textColor = config.playerColor;
                    applyBold = false; // Players are not bolded in the screenshot style
                } else if (entity_info.isLootable) {
                    // Copy exact lootable name processing from original code
                    std::string containerName = entity_info.name;
                    bool isRareContainer = false; // Flag for bolding

                    // Check for rarity patterns first
                    if (containerName.find("_uncommon_") != std::string::npos) {
                        containerName = "uncommon";
                    } else if (containerName.find("_common_") != std::string::npos) {
                        containerName = "common";
                    } else if (containerName.find("_rare_") != std::string::npos) {
                        containerName = "rare";
                        isRareContainer = true;
                    } else {
                        // If no rarity pattern, check for asterisk splitting
                        size_t asteriskPos = containerName.find('*');
                        if (asteriskPos != std::string::npos && asteriskPos < containerName.length() - 1) {
                            containerName = containerName.substr(asteriskPos + 1);
                            size_t secondAsterisk = containerName.find('*');
                            if (secondAsterisk != std::string::npos) {
                                containerName = containerName.substr(0, secondAsterisk);
                            }
                        } else {
                            // If neither rarity pattern nor asterisk found, use original name processing
                            if (containerName.find("Lootable_") == 0) {
                                containerName = containerName.substr(9);
                            }
                            if (containerName.find("Generated_Container_") == 0) {
                                containerName = containerName.substr(20);
                            }
                            size_t underscorePos = containerName.rfind('_');
                            if (underscorePos != std::string::npos && underscorePos < containerName.length() - 1) {
                                bool allDigits = true;
                                for (size_t i = underscorePos + 1; i < containerName.length(); i++) {
                                    if (!std::isdigit(containerName[i])) {
                                        allDigits = false;
                                        break;
                                    }
                                }
                                if (allDigits) {
                                    containerName = containerName.substr(0, underscorePos);
                                }
                            }
                        }
                    }
                    // Check if container is "rare" for bold rendering later (original logic)
                    isRareContainer = isRareContainer ||
                                     (containerName.find("rare") != std::string::npos) ||
                                     (containerName.find("Rare") != std::string::npos);

                    applyBold = isRareContainer; // Bolding for rare lootables

                    std::stringstream lootTextSS;
                    if (config.showDistance) {
                        lootTextSS << "Loot: " << containerName << " [" << std::fixed << std::setprecision(1) << distance << "m]"; // "m" as original
                    } else {
                        lootTextSS << "Loot: " << containerName;
                    }
                    if (config.showWorldPosition) { // Keep world pos for lootables if enabled
                        lootTextSS << std::endl << "(" << std::fixed << std::setprecision(1)
                                  << entity_info.pos.x << ", " << entity_info.pos.y << ", " << entity_info.pos.z << ")";
                    }
                    displayText = lootTextSS.str();
                    textColor = config.lootableColor;

                } else { // NPCs
                    std::stringstream npcTextSS;
                    if (config.showDistance) {
                        npcTextSS << "NPC [" << std::fixed << std::setprecision(1) << distance << "m]"; // "m" as original
                    } else {
                        npcTextSS << "NPC";
                    }
                     if (config.showWorldPosition) { // Keep world pos for NPCs if enabled
                        npcTextSS << std::endl << "(" << std::fixed << std::setprecision(1)
                                 << entity_info.pos.x << ", " << entity_info.pos.y << ", " << entity_info.pos.z << ")";
                    }
                    displayText = npcTextSS.str();

                    bool isPU_NPC = entity_info.name.find("PU_") != std::string::npos;
                    textColor = isPU_NPC ? config.npcPUColor : config.npcOtherColor;
                    applyBold = isPU_NPC; // Bolding for PU_ NPCs
                }

                // Scale text based on distance and settings
                float fontScale = std::max(0.5f, std::min(2.0f,
                    config.textScale/100.0f * (1.0f - distance/1000.0f)));

                // Calculate text size for centering
                ImVec2 textSize = ImGui::CalcTextSize(displayText.c_str());
                textSize.x *= fontScale;
                textSize.y *= fontScale;

                // Draw text centered on entity's screen position (x, y)
                ImVec2 textPos(x - textSize.x * 0.5f, y - textSize.y * 0.5f);

                // Draw Chevron for players, above the text block
                if (entity_info.isPlayer) {
                    float chevronBaseWidth = 8.0f * fontScale;
                    float chevronHeight = 6.0f * fontScale;
                    float chevronGap = 2.0f * fontScale;

                    ImVec2 chevronTip(x, textPos.y - chevronGap - chevronHeight); // Tip points upwards
                    ImVec2 chevronBaseLeft(x - chevronBaseWidth * 0.5f, textPos.y - chevronGap);
                    ImVec2 chevronBaseRight(x + chevronBaseWidth * 0.5f, textPos.y - chevronGap);

                    // Basic check to avoid drawing off-screen or if text is too high
                    if (textPos.y - chevronGap - chevronHeight > 0) {
                         draw_list->AddTriangleFilled(chevronBaseLeft, chevronBaseRight, chevronTip, textColor);
                    }
                }

                // Draw text with potential bold effect for PU_ NPCs or rare lootables
                if (applyBold) {
                    // Draw text multiple times with small offsets to simulate bold
                    RenderText(draw_list, ImVec2(textPos.x-1, textPos.y), textColor, displayText.c_str(), fontScale);
                    RenderText(draw_list, ImVec2(textPos.x+1, textPos.y), textColor, displayText.c_str(), fontScale);
                    RenderText(draw_list, ImVec2(textPos.x, textPos.y-1), textColor, displayText.c_str(), fontScale);
                    RenderText(draw_list, ImVec2(textPos.x, textPos.y+1), textColor, displayText.c_str(), fontScale);
                }

                // Draw main text
                RenderText(draw_list, textPos, textColor, displayText.c_str(), fontScale);

                // --- 2D Box Logic (original) ---
                if (config.showBoxes) {
                    // The 2D box will be drawn around the (potentially multi-line) text block
                    float boxWidth = textSize.x * 1.2f;
                    float boxHeight = textSize.y * 1.5f;

                    // The box should be centered at (x,y) like the text.
                    // If a chevron is present for players, the box will appear lower relative to the chevron.
                    // This matches typical ESP behavior where the box is around the text.
                    draw_list->AddRect(
                        ImVec2(x - boxWidth * 0.5f, y - boxHeight * 0.5f),
                        ImVec2(x + boxWidth * 0.5f, y + boxHeight * 0.5f),
                        textColor, // Uses the determined textColor
                        0.0f, 0, 1.0f
                    );
                }
            }
        }
    }

    // Update the RenderConfigWindow function to include settings for 3D boxes and smoothing
    void RenderConfigWindow() {
        // Create config window with fixed width instead of auto-resize
        ImGui::SetNextWindowSizeConstraints(ImVec2(350, 0), ImVec2(450, FLT_MAX)); // Min/max width constraints
        ImGui::Begin("ESP Configuration", &config.showConfigWindow);

        // FPS counter
        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
        ImGui::Separator();

        // Entity filtering options
        ImGui::Text("Display Filters:");
        ImGui::Checkbox("Show Players", &config.showPlayers);
        ImGui::Checkbox("Show NPCs", &config.showNPCs);
        ImGui::Checkbox("Show Lootables", &config.showLootables);

        // Display options
        ImGui::Separator();
        ImGui::Text("Display Options:");
        ImGui::Checkbox("Show Distance", &config.showDistance);
        ImGui::Checkbox("Show 2D Boxes", &config.showBoxes);
        ImGui::Checkbox("Show 3D Boxes", &config.show3DBoxes);

        // Distance filter slider - add this new section
        ImGui::Separator();
        ImGui::Text("Distance Options:");
        ImGui::SliderFloat("Max Distance (m)", &config.maxDistance, 0.0f, 10000.0f, "%.1f");

        // 3D Box Configuration (only show if 3D boxes are enabled)
        if (config.show3DBoxes) {
            ImGui::SliderFloat("Box Height (m)", &config.boxHeight, 0.5f, 3.0f, "%.1f");
            ImGui::SliderFloat("Box Width (m)", &config.boxWidth, 0.1f, 1.0f, "%.1f");
            ImGui::SliderFloat("Box Depth (m)", &config.boxDepth, 0.1f, 1.0f, "%.1f");
            ImGui::SliderFloat("Line Thickness", &config.boxThickness, 0.5f, 3.0f, "%.1f");
        }

        ImGui::Checkbox("Show World Position", &config.showWorldPosition);
        ImGui::Checkbox("Show Camera Info", &config.showCameraInfo);
        ImGui::SliderInt("Text Size", &config.textScale, 50, 200, "%d%%");

        // Color configuration
        ImGui::Separator();
        ImGui::Text("Colors:");
        ImGui::ColorEdit4("Player Color", (float*)&config.playerColor);
        ImGui::ColorEdit4("NPC Color", (float*)&config.npcColor);
        ImGui::ColorEdit4("PU_ NPC Color", (float*)&config.npcPUColor);
        ImGui::ColorEdit4("Other NPC Color", (float*)&config.npcOtherColor);
        ImGui::ColorEdit4("Lootable Color", (float*)&config.lootableColor);
        ImGui::ColorEdit4("Text Outline", (float*)&config.textOutlineColor);

        // Field of View configuration
        ImGui::Separator();
        ImGui::SliderFloat("Field of View", &config.fieldOfViewDegrees, 60.0f, 120.0f, "%.1f°");

        // Statistics
        ImGui::Separator();
        ImGui::Text("Statistics:");
        ImGui::Text("Total Entities: %zu", Globals::entityCount.load());
        ImGui::Text("Players: %zu", Globals::playerCount.load());
        ImGui::Text("NPCs: %zu", Globals::actorCount.load() - Globals::playerCount.load());
        ImGui::Text("Frame: %zu", Globals::frameCount.load());

        // Camera position display
        if (config.showCameraInfo) {
            ImGui::Separator();
            ImGui::Text("Camera Position: (%.1f, %.1f, %.1f)",
                    Globals::cameraPosition.x,
                    Globals::cameraPosition.y,
                    Globals::cameraPosition.z);
            ImGui::Text("FOV: %.1f°", Globals::cameraFOV * (180.0f/3.14159f));
        }

        ImGui::End();
    }

    // --- Public Members ---
    HWND window_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* device_context_ = nullptr;
    IDXGISwapChain* swap_chain_ = nullptr;
    ID3D11RenderTargetView* main_render_targetview_ = nullptr;
    ID3D11RenderTargetView* original_render_targetview_ = nullptr;
    ID3D11DepthStencilView* original_depth_stencilview_ = nullptr;

    bool is_initialized_ = false;
    Vec2 resolution_{1280, 720};

    // ESP Data
    std::vector<ESPInfo> middle_esp_info_;
    std::vector<ESPInfo> drawing_esp_info_;
    std::mutex player_info_mutex_;

    // Menu State
    bool show_menu_ = true;
};

inline OverlayRenderer global_esp_visuals;

// =================================================================
// DirectX Hook Functions
// =================================================================

namespace DetourFunctions {
    LRESULT CALLBACK hkWndProc(HWND hwnd, uint32_t u_msg, WPARAM w_param, LPARAM l_param) {
        // First, pass messages to ImGui
        if (ImGui::GetCurrentContext() &&
            ImGui_ImplWin32_WndProcHandler(hwnd, u_msg, w_param, l_param)) {
            return 1;  // ImGui handled the message
        }

        // Handle INSERT key toggle after ImGui has had a chance to process it
        if (u_msg == WM_KEYDOWN && w_param == VK_INSERT) {
            config.showConfigWindow = !config.showConfigWindow;
            return 1;  // We've handled this message
        }

        // Check if ImGui wants to capture input
        if (ImGui::GetCurrentContext()) {
            ImGuiIO &io = ImGui::GetIO();
            if ((io.WantCaptureMouse && (u_msg >= WM_MOUSEFIRST && u_msg <= WM_MOUSELAST)) ||
                (io.WantCaptureKeyboard && (u_msg >= WM_KEYFIRST && u_msg <= WM_KEYLAST))) {
                return 1;  // ImGui wants to capture this input
            }
        }

        // Pass unhandled messages to the original WndProc
        if (OriginalFunctions::oWndProc) {
            return CallWindowProc(OriginalFunctions::oWndProc, hwnd, u_msg, w_param, l_param);
        } else {
            return DefWindowProc(hwnd, u_msg, w_param, l_param);
        }
    }

    HRESULT __stdcall hkPresent(IDXGISwapChain *swap_chain, uint32_t sync_interval, uint32_t flags) {
        // Check if swap chain is valid
        if (!swap_chain) {
            return OriginalFunctions::oPresent(swap_chain, sync_interval, flags);
        }

        // Initialization check
        if (!global_esp_visuals.is_initialized_) {
            // Get device and context
            ID3D11Device* device = nullptr;
            HRESULT hr = swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device));
            if (FAILED(hr)) {
                std::cout << "[ERROR] Failed to get device from swap chain\n";
                return OriginalFunctions::oPresent(swap_chain, sync_interval, flags);
            }

            // Store swap chain and device
            global_esp_visuals.swap_chain_ = swap_chain;
            global_esp_visuals.device_ = device;
            device->GetImmediateContext(&global_esp_visuals.device_context_);

            // Get window handle
            DXGI_SWAP_CHAIN_DESC sd;
            swap_chain->GetDesc(&sd);
            global_esp_visuals.window_ = sd.OutputWindow;

            // Create render target
            global_esp_visuals.CreateMainRenderTargetView();
            if (!global_esp_visuals.main_render_targetview_) {
                std::cout << "[ERROR] Failed to create main render target view\n";
                device->Release();
                global_esp_visuals.device_context_->Release();
                global_esp_visuals.device_ = nullptr;
                global_esp_visuals.device_context_ = nullptr;
                return OriginalFunctions::oPresent(swap_chain, sync_interval, flags);
            }

            // Initialize ImGui
            global_esp_visuals.InitializeImGui();
            if (!ImGui::GetCurrentContext()) {
                std::cout << "[ERROR] Failed to initialize ImGui\n";
                global_esp_visuals.CleanupRenderTarget();
                device->Release();
                global_esp_visuals.device_context_->Release();
                global_esp_visuals.device_ = nullptr;
                global_esp_visuals.device_context_ = nullptr;
                return OriginalFunctions::oPresent(swap_chain, sync_interval, flags);
            }

            // Hook WndProc after ImGui initialization
            Hooking::HookWndProc();

            global_esp_visuals.is_initialized_ = true;
            std::cout << "[SUCCESS] DirectX and ImGui fully initialized\n";
        }

        // Safety check for device context
        if (!global_esp_visuals.device_context_) {
            std::cout << "[ERROR] Device context lost, shutting down ImGui\n";
            global_esp_visuals.ShutdownImGui();
            return OriginalFunctions::oPresent(swap_chain, sync_interval, flags);
        }

        // Update ESP data if needed
        {
            std::lock_guard<std::mutex> lock_player_info(global_esp_visuals.player_info_mutex_);
            global_esp_visuals.drawing_esp_info_ = global_esp_visuals.middle_esp_info_;
        }

        // Update camera information
        Globals::UpdateCameraInfo();

        // Increment frame counter
        Globals::frameCount.fetch_add(1, std::memory_order_relaxed);

        // Begin ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Get display size and update resolution cache
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 displaySize = io.DisplaySize;

        if (Globals::screenResolution.width != displaySize.x ||
            Globals::screenResolution.height != displaySize.y) {
            Globals::screenResolution.width = displaySize.x;
            Globals::screenResolution.height = displaySize.y;
        }

        // Render configuration window
        if (config.showConfigWindow) {
            global_esp_visuals.RenderConfigWindow();
        }

        // Render player ESP using the background draw list
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        global_esp_visuals.RenderPlayerESP(drawList);

        // Render ImGui
        ImGui::Render();

        // Save original render targets
        ID3D11RenderTargetView* original_rtv = nullptr;
        ID3D11DepthStencilView* original_dsv = nullptr;
        global_esp_visuals.device_context_->OMGetRenderTargets(1, &original_rtv, &original_dsv);

        // Set our render target and draw ImGui
        global_esp_visuals.device_context_->OMSetRenderTargets(
            1, &global_esp_visuals.main_render_targetview_, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Restore original render targets if they were captured
        if (original_rtv || original_dsv) {
            global_esp_visuals.device_context_->OMSetRenderTargets(1, &original_rtv, original_dsv);
        }

        // Release the references we obtained from OMGetRenderTargets
        if (original_rtv) original_rtv->Release();
        if (original_dsv) original_dsv->Release();

        // Call original Present
        HRESULT result = OriginalFunctions::oPresent(swap_chain, sync_interval, flags);

        // Handle device loss
        if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
            std::cout << "[ERROR] Device lost/reset detected (Error: 0x" << std::hex << result
                    << std::dec << "). Shutting down ImGui.\n";
            global_esp_visuals.ShutdownImGui();
        }

        return result;
    }

    HRESULT __stdcall hkResizeBuffers(IDXGISwapChain *swap_chain,
                                     uint32_t buffer_count, uint32_t width,
                                     uint32_t height, DXGI_FORMAT new_format,
                                     uint32_t swap_chain_flags) {
        if (!swap_chain) {
            return OriginalFunctions::oResizeBuffers(swap_chain, buffer_count, width, height,
                                                    new_format, swap_chain_flags);
        }

        global_esp_visuals.resolution_ = {static_cast<float>(width), static_cast<float>(height)};
        std::cout << "[INFO] Window resized to " << width << 'x' << height << '\n';

        // Clean up render target before resize
        if (global_esp_visuals.device_context_ && global_esp_visuals.main_render_targetview_) {
            // First ensure we're not using the render target
            ID3D11RenderTargetView* null_rtv = nullptr;
            global_esp_visuals.device_context_->OMSetRenderTargets(1, &null_rtv, nullptr);

            // Then release it
            global_esp_visuals.main_render_targetview_->Release();
            global_esp_visuals.main_render_targetview_ = nullptr;
            std::cout << "[INFO] Released main RTV due to resize.\n";
        }

        // Invalidate ImGui device objects - only if ImGui is properly initialized
        if (global_esp_visuals.is_initialized_ && ImGui::GetCurrentContext() &&
            ImGui::GetIO().BackendRendererUserData) {
            std::cout << "[INFO] Invalidating ImGui device objects due to resize.\n";
            ImGui_ImplDX11_InvalidateDeviceObjects();
        }

        // Call original ResizeBuffers
        HRESULT hr = OriginalFunctions::oResizeBuffers(
            swap_chain, buffer_count, width, height, new_format, swap_chain_flags);

        // Recreate resources if successful
        if (SUCCEEDED(hr)) {
            std::cout << "[INFO] Original ResizeBuffers succeeded.\n";
            if (global_esp_visuals.is_initialized_) {
                // Recreate the render target
                global_esp_visuals.CreateMainRenderTargetView();
                if (!global_esp_visuals.main_render_targetview_) {
                    std::cout << "[ERROR] Failed to recreate render target view after resize\n";
                    global_esp_visuals.ShutdownImGui();
                    return hr;
                }

                // Recreate ImGui device objects if needed
                if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData) {
                    std::cout << "[INFO] Recreating ImGui device objects.\n";
                    ImGui_ImplDX11_CreateDeviceObjects();
                    if (!ImGui::GetCurrentContext() || !ImGui::GetIO().BackendRendererUserData) {
                        std::cout << "[ERROR] Failed to recreate ImGui device objects\n";
                        global_esp_visuals.ShutdownImGui();
                        return hr;
                    }
                }
            }
        } else {
            std::cout << "[ERROR] Original ResizeBuffers failed with error 0x"
                    << std::hex << hr << std::dec << "\n";
            global_esp_visuals.ShutdownImGui();
        }

        return hr;
    }

    double __fastcall hkCEntitySystem__Update(int64_t entity_system_param) {
        CEntitySystem *entity_system = reinterpret_cast<CEntitySystem*>(entity_system_param);
        if (entity_system && entity_system->entity_class_registry_) {
            static CEntityClass *player_entity_class =
                entity_system->entity_class_registry_->FindClass("Player");

            auto it = global_memory_player_info.begin();

            // Count variables
            size_t totalEntities = 0;
            size_t actorEntities = 0;
            size_t playerEntities = 0;
            size_t lootableEntities = 0;

            // Process all entities in the array
            for (size_t i = 0; i < static_cast<size_t>(entity_system->entity_array_.max_size_); ++i) {
                CEntity *entity = reinterpret_cast<CEntity*>(
                    ExtractLower48Bytes(entity_system->entity_array_[i]));

                if (!entity || !entity->entity_class_) continue;

                CEntityClass* entityClass = reinterpret_cast<CEntityClass*>(
                    ExtractLower48Bytes(entity->entity_class_));

                if (!entityClass || !entityClass->name_) continue;

                totalEntities++;

                // Check if entity is a player, NPC, or lootable container
                bool isPlayer = (entityClass == player_entity_class);
                bool isNPC = false;
                bool isLootable = false;

                // Get entity class name
                const char* className = entityClass->name_;

                // Check if entity is an NPC or lootable container
                if (!isPlayer && className) {
                    // Check for NPC
                    isNPC = (strstr(className, "NPC") != nullptr ||
                             strstr(className, "PU_") != nullptr);

                    // Check for lootable container
                    isLootable = (strstr(className, "Lootable_") != nullptr);
                }

                // Count different entity types
                if (isPlayer) playerEntities++;
                if (isNPC) actorEntities++;
                if (isLootable) lootableEntities++;

                // Handle entity information storage
                if ((isPlayer || isNPC || isLootable) && it == global_memory_player_info.end()) {
                    // Get world position for this entity
                    Vec3 worldPos = entity->GetWorldPos();

                    // Get entity name
                    const char* entityName = entity->name_ ? entity->name_ : "Unknown";

                    global_memory_player_info.emplace_back(
                        i, entity->id_, entityName, worldPos);
                    global_memory_player_info.back().isPlayer = isPlayer;
                    global_memory_player_info.back().isLootable = isLootable;
                    it = global_memory_player_info.end();
                    continue;
                }

                // Delete all entities with index < i
                while (it != global_memory_player_info.end() && it->index < i) {
                    it = global_memory_player_info.erase(it);
                }

                if ((isPlayer || isNPC || isLootable) && it == global_memory_player_info.end()) {
                    // Get world position for this entity
                    Vec3 worldPos = entity->GetWorldPos();

                    // Get entity name
                    const char* entityName = entity->name_ ? entity->name_ : "Unknown";

                    global_memory_player_info.emplace_back(
                        i, entity->id_, entityName, worldPos);
                    global_memory_player_info.back().isPlayer = isPlayer;
                    global_memory_player_info.back().isLootable = isLootable;
                    it = global_memory_player_info.end();
                    continue;
                }

                if (it == global_memory_player_info.end()) {
                    continue;
                }

                if (it->index == i) {
                    if (it->id == entity->id_) {
                        // Same entity at same index, just update position
                        it->pos = entity->GetWorldPos();
                        it->isPlayer = isPlayer;
                        it->isLootable = isLootable;
                        ++it;
                        continue;
                    }

                    // Different entity at same index
                    it->id = entity->id_;
                    it->name = entity->name_ ? entity->name_ : "Unknown";
                    it->pos = entity->GetWorldPos();
                    it->isPlayer = isPlayer;
                    it->isLootable = isLootable;
                    ++it;
                    continue;
                }

                if (isPlayer || isNPC || isLootable) {
                    // Insert new entity
                    Vec3 worldPos = entity->GetWorldPos();
                    const char* entityName = entity->name_ ? entity->name_ : "Unknown";

                    it = global_memory_player_info.emplace(
                        it, i, entity->id_, entityName, worldPos);
                    it->isPlayer = isPlayer;
                    it->isLootable = isLootable;
                    ++it;
                }
            }

            // Remove any remaining entities that are no longer valid
            while (it != global_memory_player_info.end()) {
                it = global_memory_player_info.erase(it);
            }

            // Update global statistics atomically
            Globals::entityCount.store(totalEntities, std::memory_order_release);
            Globals::actorCount.store(actorEntities, std::memory_order_release);
            Globals::playerCount.store(playerEntities, std::memory_order_release);

            // Update the ESP info (without smoothing)
            std::lock_guard<std::mutex> lock(global_esp_visuals.player_info_mutex_);
            global_esp_visuals.middle_esp_info_.clear();

            for (const PlayerInfo& player_info : global_memory_player_info) {
                // Create new ESP info with all data from player_info
                ESPInfo newEspInfo(player_info);
                global_esp_visuals.middle_esp_info_.push_back(newEspInfo);
            }
        }

        // Call original function
        return OriginalFunctions::oCEntitySystem__Update(entity_system_param);
    }
}

// =================================================================
// Hooking Class Implementation
// =================================================================

bool Hooking::Initialize() {
    HMODULE module = GetModuleHandleA("StarCitizen.exe");
    if (!module) {
        std::cout << "[ERROR] Target module not found\n";
        return false;
    }

    GetModuleInformation(GetCurrentProcess(), module, &mod_info_, sizeof(mod_info_));
    std::cout << "[INFO] Base: " << std::hex << reinterpret_cast<uintptr_t>(mod_info_.lpBaseOfDll)
            << " Size: " << mod_info_.SizeOfImage << std::dec << "\n";

    if (MH_Initialize() != MH_OK) {
        std::cout << "[ERROR] MinHook initialization failed.\n";
        return false;
    }

    std::cout << "[SUCCESS] Hooking system initialized\n";
    return true;
}

bool Hooking::Uninitialize() {
    // First unhook WndProc if it was hooked
    UnhookWndProc();

    if (MH_DisableHook(MH_ALL_HOOKS) != MH_OK) {
        std::cout << "[ERROR] Failed to disable hooks\n";
        return false;
    }

    if (MH_Uninitialize() != MH_OK) {
        std::cout << "[ERROR] Failed to uninitialize MinHook\n";
        return false;
    }

    std::cout << "[INFO] Hooks removed\n";
    return true;
}

template <typename TDetourFunc, typename TOrigFunc>
bool Hooking::HookFunction(uintptr_t func_addr, TDetourFunc detour_func, TOrigFunc &orig_func) {
    if (!func_addr) {
        std::cout << "[ERROR] Cannot hook null function address\n";
        return false;
    }

    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<LPVOID>(func_addr),
        reinterpret_cast<LPVOID>(detour_func),
        reinterpret_cast<LPVOID *>(&orig_func));

    if (status != MH_OK) {
        std::cout << "[ERROR] Hook creation failed: " << MH_StatusToString(status) << "\n";
        return false;
    }

    status = MH_EnableHook(reinterpret_cast<LPVOID>(func_addr));
    if (status != MH_OK) {
        std::cout << "[ERROR] Failed to enable hook: " << MH_StatusToString(status) << "\n";
        return false;
    }

    return true;
}

void Hooking::HookPresentAndResizeBuffers() {
    // Get the foreground window first
    HWND window = GetForegroundWindow();
    bool is_dummy_window_created = false;

    // Create dummy window if no foreground window available
    if (!window) {
        window = CreateWindowExA(0,
                               "STATIC",
                               "Dummy",
                               WS_OVERLAPPED,
                               0,
                               0,
                               1,
                               1,
                               NULL,
                               NULL,
                               GetModuleHandle(NULL),
                               NULL);

        if (!window) {
            printf("[ERROR] Failed to create dummy window\n");
            return;
        }
        is_dummy_window_created = true;
        std::cout << "[INFO] Created dummy window for D3D initialization\n";
    }

    // Create a D3D11 device and swap chain
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = window;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain *swap_chain = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *device_context = nullptr;

    D3D_FEATURE_LEVEL feature_level;
    const D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};

    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr,                 // Adapter (default)
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,                 // Module
        0,                       // Flags
        feature_levels,
        2,                       // Feature levels count
        D3D11_SDK_VERSION,
        &sd,
        &swap_chain,
        &device,
        &feature_level,
        &device_context);

    // Fall back to WARP if hardware device creation fails
    if (result == DXGI_ERROR_UNSUPPORTED) {
        std::cout << "[WARN] Hardware device creation failed, falling back to WARP\n";
        result = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            feature_levels,
            2,
            D3D11_SDK_VERSION,
            &sd,
            &swap_chain,
            &device,
            &feature_level,
            &device_context);
    }

    if (FAILED(result)) {
        std::cout << "[ERROR] Device creation failed with error 0x" << std::hex << result << std::dec << "\n";

        // Clean up if dummy window was created
        if (is_dummy_window_created && window) {
            DestroyWindow(window);
        }
        return;
    }

    std::cout << "[SUCCESS] D3D Device and SwapChain created successfully\n";

    // Get swap chain vtable
    void **swap_chain_vtable = *reinterpret_cast<void ***>(swap_chain);
    if (!swap_chain_vtable) {
        std::cout << "[ERROR] Failed to get SwapChain vtable\n";

        // Clean up resources
        if (device_context) device_context->Release();
        if (device) device->Release();
        if (swap_chain) swap_chain->Release();

        if (is_dummy_window_created && window) {
            DestroyWindow(window);
        }
        return;
    }

    // Get function addresses from vtable (indices: 8 for Present, 13 for ResizeBuffers)
    uintptr_t present_func_addr = reinterpret_cast<uintptr_t>(swap_chain_vtable[8]);
    uintptr_t resize_buffers_func_addr = reinterpret_cast<uintptr_t>(swap_chain_vtable[13]);

    // Clean up temporary D3D resources
    if (device_context) device_context->Release();
    if (device) device->Release();
    if (swap_chain) swap_chain->Release();

    if (is_dummy_window_created && window) {
        DestroyWindow(window);
    }

    std::cout << "[INFO] Retrieved function addresses: Present=0x" << std::hex << present_func_addr
            << ", ResizeBuffers=0x" << resize_buffers_func_addr << std::dec << "\n";

    // Hook Present
    if (HookFunction(present_func_addr, DetourFunctions::hkPresent, OriginalFunctions::oPresent)) {
        std::cout << "[SUCCESS] Hooked Present at: 0x" << std::hex << present_func_addr << std::dec << "\n";
    } else {
        std::cout << "[ERROR] Failed to hook Present\n";
        return;
    }

    // Hook ResizeBuffers
    if (HookFunction(resize_buffers_func_addr,
                   DetourFunctions::hkResizeBuffers,
                   OriginalFunctions::oResizeBuffers)) {
        std::cout << "[SUCCESS] Hooked ResizeBuffers at: 0x" << std::hex << resize_buffers_func_addr
                << std::dec << "\n";
    } else {
        std::cout << "[ERROR] Failed to hook ResizeBuffers\n";
    }
}

void Hooking::HookWndProc() {
    if (!global_esp_visuals.window_) {
        std::cerr << "[ERROR] Cannot hook WndProc: Window handle is null\n";
        return;
    }

    if (OriginalFunctions::oWndProc) {
        std::cout << "[WARN] WndProc already hooked\n";
        return;  // Already hooked
    }

    // Get and store the original WndProc
    OriginalFunctions::oWndProc =
        (WNDPROC)GetWindowLongPtr(global_esp_visuals.window_, GWLP_WNDPROC);

    if (!OriginalFunctions::oWndProc) {
        DWORD error = GetLastError();
        std::cerr << "[ERROR] Failed to get original WndProc. Error code: " << error << "\n";
        return;
    }

    // Set our hook function as the new WndProc
    LONG_PTR result = SetWindowLongPtr(
        global_esp_visuals.window_, GWLP_WNDPROC, (LONG_PTR)DetourFunctions::hkWndProc);

    if (result == 0 && GetLastError() != 0) {
        std::cerr << "[ERROR] Failed to set detour WndProc. Error code: " << GetLastError() << "\n";
        OriginalFunctions::oWndProc = nullptr;  // Reset on failure
    } else {
        std::cout << "[SUCCESS] WndProc hooked successfully\n";
    }
}

void Hooking::UnhookWndProc() {
    if (!global_esp_visuals.window_) {
        // Window handle is null, nothing to unhook
        return;
    }

    if (!OriginalFunctions::oWndProc) {
        // Not hooked or already unhooked
        return;
    }

    // Restore the original WndProc
    LONG_PTR result = SetWindowLongPtr(
        global_esp_visuals.window_, GWLP_WNDPROC, (LONG_PTR)OriginalFunctions::oWndProc);

    if (result == 0 && GetLastError() != 0) {
        // Only log as error if it's not because the window is being destroyed
        if (GetLastError() != ERROR_INVALID_WINDOW_HANDLE) {
            std::cerr << "[ERROR] Failed to restore original WndProc. Error code: "
                    << GetLastError() << "\n";
        }
    } else {
        std::cout << "[INFO] WndProc unhooked successfully\n";
    }

    // Clear the pointer even if restore fails - we no longer consider it hooked
    OriginalFunctions::oWndProc = nullptr;
}

void Hooking::HookCEntitySystem__Update() {
    // Use the address from constants
    uintptr_t entity_system__update_addr = GetFuncAddr(0x69060B0ULL);

    if (!entity_system__update_addr) {
        std::cout << "[ERROR] CEntitySystem::Update address is null\n";
        return;
    }

    if (HookFunction(entity_system__update_addr,
                    DetourFunctions::hkCEntitySystem__Update,
                    OriginalFunctions::oCEntitySystem__Update)) {
        std::cout << "[SUCCESS] Hooked CEntitySystem::Update() at: 0x" << std::hex
                << entity_system__update_addr << std::dec << "\n";
    } else {
        std::cout << "[ERROR] Failed to hook CEntitySystem::Update()\n";
    }
}

// ********************************************************
static const std::unordered_map<int, const char*> entityMap = {
    {1,  "CPhysicalEntity"},
    {2,  "CRigidEntity"},
    {3,  "CWheeledVehicleEntity"},
    {4,  "CRopeEntityEx"},
    {5,  "CParticleEntity"},
    {6,  "CArticulatedEntity"},
    {7,  "CRopeEntity"},
    {8,  "CSoftEntity"},
    {9,  "CPhysArea"},
    {10, "CSpaceshipEntity"},
    {11, "CActorEntity"},
    {12, "CPhysPlanetEntity"},
    {13, "CSoftEntityEx"},
    {14, "CHoverEntity"}
};

// ****************************************************

// #if defined(_MSC_VER)
// //  ---- MSVC / clang-cl ---------------------------------
//   #define VCALL __vectorcall
// #elif defined(__clang__)
//   // clang (non-MSVC) understands the attribute directly
//   #define VCALL __attribute__((vectorcall))
// #elif defined(__GNUC__)
//   // gcc has no vectorcall; ms_abi is the closest match
//   #define VCALL __attribute__((ms_abi))
// #else
//   #error "Unknown compiler: please add vectorcall support"
// #endif

// using tSub_146437910 =
//     void*** (VCALL*)(
//         int32_t,
//         void*,
//         uint64_t,
//         const char*,
//         __m256i,
//         __m256i,      // pass arg6 by value, not as a pointer
//         int64_t,
//         int32_t);

// static tSub_146437910 oSub_146437910 = nullptr;

// void*** VCALL hkSub_146437910(
//         int32_t   a1,
//         void*     a2,
//         uint64_t  a3,
//         const char* Source,
//         __m256i   z0,
//         __m256i   z1,
//         int64_t   extra1,
//         int32_t   extra2)
// {
//     if (a1 == 11)
//         std::cout << "[HOOK] kind=11  ctx=" << a2 << '\n';

//     void*** rv = oSub_146437910(
//                      a1, a2, a3, Source,
//                      z0, z1, extra1, extra2);

//     if (a1 == 11)
//         std::cout << "  rv=" << rv << '\n';
//     return rv;
// }

// // 4.  Member method that installs the hook ***********************************
// void Hooking::VanityHooks()
// {
//     const auto base   = reinterpret_cast<std::uintptr_t>(::GetModuleHandle(nullptr));
//     const auto addr   = base + 0x6437910ULL;     // sub_146437910

//     std::cout << "Hooking sub_146437910 at " << std::hex << addr << std::dec << '\n';

//     if (HookFunction(addr,
//                      hkSub_146437910,
//                      oSub_146437910))
//     {
//         std::cout << "[SUCCESS] Hooked sub_146437910 at 0x"
//                   << std::hex << addr << std::dec << '\n';
//     }
//     else
//     {
//         std::cout << "[ERROR] Failed to hook sub_146437910\n";
//     }
// }

// ---------------------------------------------------------------------------
//  Common helpers
static void EnsureDbgHelpInitialised()
{
    static std::once_flag initFlag;
    std::call_once(initFlag, []()
    {
        ::SymSetOptions(SYMOPT_DEFERRED_LOADS  |
                        SYMOPT_FAIL_CRITICAL_ERRORS |
                        SYMOPT_UNDNAME);
        ::SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    });
}

//  Disassemble a few instructions around `pc`
static void PrintDisassemblyAround(HANDLE hProc, DWORD64 pc,
                                   ZydisDecoder& dec, ZydisFormatter& fmt)
{
    constexpr SIZE_T  WINDOW_BYTES = 0x40;          // 64-byte window
    constexpr SIZE_T  CTXT_BYTES   = 0x20;          // decode up to ±0x20
    BYTE              buf[WINDOW_BYTES] = {};
    SIZE_T            got = 0;

    // Read target memory.  If the page is not readable, silently skip.
    if (!ReadProcessMemory(hProc,
            reinterpret_cast<LPCVOID>(pc - CTXT_BYTES),
            buf, sizeof buf, &got) || got == 0)
    {
        std::cerr << "        <unable to read memory>\n";
        return;
    }
    // Remove unused variable
    // const BYTE* base = buf;
    const BYTE* p    = buf;
    const BYTE* end  = buf + got;
    DWORD64     ip   = pc - CTXT_BYTES;

    // First pass: advance until we are within ~16 bytes before PC
    ZydisDecodedInstruction instr;
    while (p < end && ip + instr.length < pc - 15)
    {
        if (ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, p, end - p,
                                                 &instr, nullptr)))
            p  += instr.length, ip += instr.length;
        else
            ++p, ++ip;          // resync aggressively
    }

    // Second pass: print 10 instructions (5 before, 5 after)
    int printed = 0;
    while (p < end && printed < 10 &&
           ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, p, end - p,
                                                &instr, nullptr)))
    {
        char text[256];
        ZydisFormatterFormatInstruction(&fmt, &instr, nullptr, 0,
                                        text, sizeof text,
                                        ip, nullptr);

        auto old = std::cerr.flags();
        std::cerr.copyfmt(std::cerr);
        std::cerr << "        "
                  << (ip == pc ? ">> " : "   ")
                  << std::setw(16) << std::hex << ip << std::dec
                  << "  " << text << '\n';
        std::cerr.flags(old);

        p  += instr.length;
        ip += instr.length;
        ++printed;
    }
}

// ---------------------------------------------------------------- exception
static LONG WINAPI VectoredHandler(PEXCEPTION_POINTERS ei)
{
    if (!ei || !ei->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;

    auto  rec = ei->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    std::cerr << "[FATAL] Caught access violation at 0x"
              << std::hex << rec->ExceptionAddress << std::dec << '\n'
              << "  Attempted to "
              << ((rec->ExceptionInformation[0] == 0) ? "read" :
                  (rec->ExceptionInformation[0] == 1) ? "write" : "execute")
              << " address " << std::hex << rec->ExceptionInformation[1]
              << std::dec << "\n";

    // ------------------ back-trace
    EnsureDbgHelpInitialised();

    void*  stack[64];
    USHORT frames = ::CaptureStackBackTrace(0, _countof(stack), stack, nullptr);

    std::cerr << "Backtrace (" << frames << " frames):\n";

    HANDLE         hProc = GetCurrentProcess();
    ZydisDecoder   decoder;
    ZydisFormatter fmt;

    // Initialize Zydis decoder with correct machine mode and address width
    ZydisDecoderInit(&decoder,
                     ZYDIS_MACHINE_MODE_LONG_64,
                     ZYDIS_STACK_WIDTH_64);
    ZydisFormatterInit(&fmt, ZYDIS_FORMATTER_STYLE_INTEL);

    for (USHORT i = 0; i < frames; ++i)
    {
        DWORD64 addr = reinterpret_cast<DWORD64>(stack[i]);

        // --- module name
        HMODULE hMod = nullptr;
        char    modBase[MAX_PATH] = "<unknown>";
        if (::GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(addr), &hMod) && hMod)
        {
            if (::GetModuleFileNameA(hMod, modBase, sizeof modBase))
                if (char* p = strrchr(modBase, '\\')) strcpy(modBase, p + 1);
        }

        // --- symbol name
        char                symBuf[sizeof(SYMBOL_INFO) + 256] = {};
        PSYMBOL_INFO        sym = reinterpret_cast<PSYMBOL_INFO>(symBuf);
        DWORD64             disp = 0;
        bool haveSym = false;

        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = 255;
        if (::SymFromAddr(hProc, addr, &disp, sym))
            haveSym = true;

        auto old = std::cerr.flags();
        std::cerr.copyfmt(std::cerr);
        std::cerr << "  [" << std::setw(2) << i << "] "
                  << modBase << '!';
        if (haveSym)
            std::cerr << sym->Name << "+0x" << std::hex << disp << std::dec;
        else
            std::cerr << "0x" << std::hex << addr << std::dec;
        std::cerr << '\n';
        std::cerr.flags(old);

        // --- disassembly context
        PrintDisassemblyAround(hProc, addr, decoder, fmt);
    }

    //MessageBoxA(nullptr,
    //            "Caught access violation (segfault).\nSee console for details.",
    //            "Fatal Error", MB_OK | MB_ICONERROR);

    return EXCEPTION_CONTINUE_SEARCH;
}

// =================================================================
// Main Entry Point
// =================================================================

DWORD WINAPI MainThread(LPVOID lp_reserved) {
    // Setup console for logging
    AllocConsole();
    FILE *fp_in = nullptr, *fp_out = nullptr, *fp_err = nullptr;
    if (freopen_s(&fp_in, "CONIN$", "r", stdin) != 0 ||
        freopen_s(&fp_out, "CONOUT$", "w", stdout) != 0 ||
        freopen_s(&fp_err, "CONOUT$", "w", stderr) != 0) {
        MessageBoxA(NULL, "Failed to redirect console streams.", "Error", MB_OK | MB_ICONERROR);
    } else {
        std::cout << "[INFO] Console streams redirected.\n";
    }

    void* handler = AddVectoredExceptionHandler(1, VectoredHandler);

    std::cout << "[INFO] ESP System Initialized\n";

    // Initialize hooking
    Hooking hooking;
    if (!hooking.Initialize()) {
        std::cerr << "[FATAL] Failed to initialize MinHook!\n";
        if (handler) RemoveVectoredExceptionHandler(handler);
        FreeLibraryAndExitThread((HMODULE)lp_reserved, 1);
        return 1;
    }

    // Initialize game-specific function pointers
    OriginalFunctions::ProjectToScreenStub = reinterpret_cast<decltype(OriginalFunctions::ProjectToScreenStub)>(
        hooking.GetFuncAddr(0x977a60ULL));
    OriginalFunctions::CEntity__GetWorldPos_stub = reinterpret_cast<decltype(OriginalFunctions::CEntity__GetWorldPos_stub)>(
        hooking.GetFuncAddr(0x68E0330ULL));
    OriginalFunctions::CEntityClassRegistry__FindClass_stub = reinterpret_cast<decltype(OriginalFunctions::CEntityClassRegistry__FindClass_stub)>(
        hooking.GetFuncAddr(0x68D2CC0ULL));

    // Hook DirectX functions for rendering
    hooking.HookPresentAndResizeBuffers();

    //hooking.VanityHooks();

    // Hook game function to collect player information
    hooking.HookCEntitySystem__Update();

    // Main loop - just wait for termination
    while (true) {
        if (GetAsyncKeyState(VK_END) & 1) {
            break; // Press END to unload
        }
        Sleep(100);
    }

    // Clean up hooks before exiting
    hooking.Uninitialize();

    // Remove vectored exception handler
    if (handler) RemoveVectoredExceptionHandler(handler);

    // Cleanup console
    if (fp_in) fclose(fp_in);
    if (fp_out) fclose(fp_out);
    if (fp_err) fclose(fp_err);
    FreeConsole();

    FreeLibraryAndExitThread((HMODULE)lp_reserved, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
            break;
        case DLL_PROCESS_DETACH:
            break;
    }
    return TRUE;
}
