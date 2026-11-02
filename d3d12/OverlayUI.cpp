#include "OverlayUI.h"
#include "logger.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>
#include <dxgi.h>
#include <thread>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <ctime>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Global control states shared with WrappedCommandList / ObjectExporter
std::atomic<bool> g_CaptureTriggered = false;
std::atomic<bool> g_AutoExport = false;
std::atomic<bool> g_IsExporting = false;
std::atomic<int>  g_FromFilter = 0;
std::atomic<int>  g_ToFilter = 1000000;
std::atomic<bool> g_SkipSubmeshes = false;
std::atomic<int>  g_SkipSubmeshCount = 0;
std::atomic<bool> g_MultiMeshExport = false;
std::atomic<bool> g_SubMeshCountEnabled = false;
std::atomic<int>  g_SubMeshCount = 1;

static std::thread g_guiThread;
static std::atomic<bool> g_guiRunning = false;
static HWND g_guiHwnd = nullptr;

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

static bool s_alwaysOnTop = false;
static int selectedObjIndex = -1;

static void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (SUCCEEDED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer))))
    {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

static void CleanupRenderTarget()
{
    if (g_mainRenderTargetView)
    {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

static std::time_t FileTimeToTimeT(std::filesystem::file_time_type fileTime)
{
    using namespace std::chrono;
    auto sctp = time_point_cast<system_clock::duration>(
        fileTime - std::filesystem::file_time_type::clock::now() + system_clock::now()
    );
    return system_clock::to_time_t(sctp);
}

static void ImGui_Spinner(const char* label, float radius = 8.0f, float thickness = 2.0f, const ImU32& color = IM_COL32(100, 255, 255, 255))
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float time = (float)ImGui::GetTime();

    float start_angle = time * 8.0f;
    float end_angle = start_angle + 3.14159265f * 1.5f;

    ImVec2 center = ImVec2(pos.x + radius, pos.y + radius);
    draw_list->PathArcTo(center, radius, start_angle, end_angle, 32);
    draw_list->PathStroke(color, false, thickness);
    ImGui::Dummy(ImVec2((radius + thickness) * 2, (radius + thickness) * 2));
}

static void DebugTab()
{
    static ImGuiTextFilter filter;
    static bool autoScroll = true;

    if (ImGui::Button("Clear Log")) {
        Logger::ClearLogBuffer();
    }

    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll);

    ImGui::SameLine();
    filter.Draw("Filter", 200);

    ImGui::Separator();

    ImGui::BeginChild("LogArea", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);

    const auto logLines = Logger::CopyLogBuffer();

    for (const std::string& line : logLines) {
        if (!filter.PassFilter(line.c_str()))
            continue;

        if (line.find("[ERROR]") != std::string::npos)
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 80, 80, 255));
        else if (line.find("[WARN]") != std::string::npos)
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 0, 255));
        else if (line.find("[DEBUG]") != std::string::npos)
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 150, 255, 255));
        else
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(180, 255, 180, 255)); // INFO

        ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor();
    }

    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
}

static void GeometryTab()
{
    try {
        std::error_code ec;
        if (!std::filesystem::exists("Exported")) {
            std::filesystem::create_directory("Exported", ec);
        }

        bool capturePending = g_CaptureTriggered.load();
        if (ImGui::Button(capturePending ? "CAPTURE: PENDING... (C / N)" : "CAPTURE (C / N)")) {
            g_CaptureTriggered.store(true);
        }

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Click to capture geometry (Hotkeys: C or N)");
        }

        if (g_IsExporting.load()) {
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3);
            ImGui_Spinner("ExportSpinner", 8.0f, 2.0f, IM_COL32(100, 255, 255, 255));
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 1.0f, 1.0f), "Exporting mesh...");
        }

        // Experimental: Skip submeshes
        bool skipEnabled = g_SkipSubmeshes.load();
        if (ImGui::Checkbox("Skip Submeshes [Exp.]", &skipEnabled)) {
            g_SkipSubmeshes.store(skipEnabled);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Experimental: If enabled, skips the first N qualifying submeshes during capture to reach subsequent submeshes of a multi-part model.");
        }

        ImGui::SameLine();
        int skipVal = g_SkipSubmeshCount.load();
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputInt("N", &skipVal, 1, 5)) {
            if (skipVal < 0) skipVal = 0;
            g_SkipSubmeshCount.store(skipVal);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Number of submeshes (N) to skip");
        }

        ImGui::Separator();

        bool autoExp = g_AutoExport.load();
        if (ImGui::Button(autoExp ? "STREAM EXPORT: [ON]" : "STREAM EXPORT: [OFF]")) {
            g_AutoExport.store(!autoExp);
        }

        ImGui::Text("Filter");
        int fromVal = g_FromFilter.load();
        int toVal = g_ToFilter.load();
        if (ImGui::SliderInt("Object From Size", &fromVal, 0, 1000000))
            g_FromFilter.store(fromVal);
        if (ImGui::SliderInt("Object To Size", &toVal, 0, 1000000))
            g_ToFilter.store(toVal);

        ImGui::Separator();

        if (ImGui::Button("Reset Hashes")) {
            ResetExportedHashes();
        }

        ImGui::Separator();

        enum SortMode { ByName, BySize, ByDate };
        static SortMode sortMode = ByName;
        ImGui::Combo("Sort by", (int*)&sortMode, "Name\0Size\0Date\0");

        struct FileInfo {
            std::string name;
            uintmax_t size;
            std::filesystem::file_time_type time;
        };

        std::vector<FileInfo> fileInfos;

        if (std::filesystem::exists("Exported")) {
            for (const auto& entry : std::filesystem::directory_iterator("Exported")) {
                if (entry.path().extension() == ".obj") {
                    FileInfo info;
                    info.name = entry.path().filename().string();
                    info.size = std::filesystem::file_size(entry.path(), ec);
                    info.time = std::filesystem::last_write_time(entry.path(), ec);
                    fileInfos.push_back(info);
                }
            }
        }

        std::sort(fileInfos.begin(), fileInfos.end(), [&](const FileInfo& a, const FileInfo& b) {
            if (sortMode == ByName)
                return a.name < b.name;
            if (sortMode == BySize)
                return a.size > b.size;
            if (sortMode == ByDate)
                return a.time > b.time;
            return false;
        });

        ImGui::Text("Exported .obj files (%zu):", fileInfos.size());
        if (ImGui::BeginListBox("##obj_listbox", ImVec2(-FLT_MIN, 220))) {
            for (size_t i = 0; i < fileInfos.size(); ++i) {
                const bool is_selected = (selectedObjIndex == static_cast<int>(i));

                std::time_t t = FileTimeToTimeT(fileInfos[i].time);
                char timeBuf[32];
                ctime_s(timeBuf, sizeof(timeBuf), &t);
                timeBuf[strcspn(timeBuf, "\n")] = 0;

                std::string label = fileInfos[i].name +
                    " | " + std::to_string(fileInfos[i].size / 1024) + " KB" +
                    " | " + timeBuf;

                if (ImGui::Selectable(label.c_str(), is_selected)) {
                    selectedObjIndex = static_cast<int>(i);
                }
                if (is_selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndListBox();
        }

        if (selectedObjIndex >= 0 && static_cast<size_t>(selectedObjIndex) < fileInfos.size()) {
            const std::string& selectedName = fileInfos[selectedObjIndex].name;
            std::string fullPath = std::filesystem::absolute("Exported/" + selectedName).string();

            if (ImGui::Button("Open in Explorer")) {
                std::string explorerArg = "/select,\"" + fullPath + "\"";
                ShellExecuteA(nullptr, "open", "explorer.exe", explorerArg.c_str(), nullptr, SW_SHOWNORMAL);
            }

            ImGui::SameLine();

            if (ImGui::Button("Open in 3DViewer.net")) {
                ShellExecuteA(nullptr, "open", "https://3dviewer.net", nullptr, nullptr, SW_SHOWNORMAL);
            }

            ImGui::SameLine();

            if (ImGui::Button("Delete Selected")) {
                std::filesystem::remove("Exported/" + selectedName, ec);
                selectedObjIndex = -1;
            }
        }

        if (ImGui::Button("Delete All")) {
            for (const auto& info : fileInfos) {
                std::filesystem::remove("Exported/" + info.name, ec);
            }
            selectedObjIndex = -1;
        }
    }
    catch (const std::exception& e) {
        Logger::LogInfo() << "[GeometryTab] Exception: " << e.what();
    }
    catch (...) {
        Logger::LogInfo() << "[GeometryTab] Unknown exception occurred.";
    }
}

static void TexturesTab()
{
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Direct3D 12 Texture Export");
    ImGui::Separator();
    ImGui::TextWrapped("Direct3D 12 texture extraction is currently in development. Geometry mesh capture is fully active.");
}

static void RenderCompanionUI(HWND hwnd)
{
    ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f), "Voxel Direct3D 12 Control Panel");
    ImGui::SameLine();

    if (ImGui::Checkbox("Always on Top", &s_alwaysOnTop)) {
        SetWindowPos(hwnd, s_alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }

    ImGui::SameLine();
    ImGui::TextDisabled("| (Press F11 to Show/Hide)");

    ImGui::Separator();

    if (ImGui::BeginTabBar("Tabs")) {
        if (ImGui::BeginTabItem("Geometry")) {
            GeometryTab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Textures")) {
            TexturesTab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Debug")) {
            DebugTab();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

static LRESULT WINAPI CompanionWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_CLOSE:
        // Hide on close instead of shutting down the process
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ToggleCompanionGUIVisibility()
{
    if (g_guiHwnd)
    {
        if (IsWindowVisible(g_guiHwnd))
            ShowWindow(g_guiHwnd, SW_HIDE);
        else
        {
            ShowWindow(g_guiHwnd, SW_SHOW);
            SetForegroundWindow(g_guiHwnd);
        }
    }
}

static void CompanionGUIThread()
{
    WNDCLASSEXW wc = {
        sizeof(wc),
        CS_CLASSDC,
        CompanionWndProc,
        0L,
        0L,
        GetModuleHandle(nullptr),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        L"VoxelD3D12CompanionWindow",
        nullptr
    };
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"Voxel - Direct3D 12 Control Panel",
        WS_OVERLAPPEDWINDOW,
        80, 80, 720, 680,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (!hwnd)
    {
        Logger::LogError() << "[CompanionGUI] Failed to create companion window.";
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return;
    }

    g_guiHwnd = hwnd;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice,
        &featureLevel, &g_pd3dDeviceContext
    );

    if (FAILED(res))
    {
        // Fallback to WARP (software rasterizer) if hardware D3D11 is blocked
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice,
            &featureLevel, &g_pd3dDeviceContext
        );
    }

    if (FAILED(res))
    {
        Logger::LogError() << "[CompanionGUI] Failed to create D3D11 device: 0x" << std::hex << res;
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        g_guiHwnd = nullptr;
        return;
    }

    CreateRenderTarget();

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // Avoid cluttering game folder with imgui.ini
    ImGui::StyleColorsDark();

    if (std::filesystem::exists("C:\\Windows\\Fonts\\Arial.ttf"))
    {
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\Arial.ttf", 18.0f);
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    Logger::LogInfo() << "[CompanionGUI] Direct3D 12 Companion Window initialized successfully.";

    while (g_guiRunning.load())
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                g_guiRunning.store(false);
        }

        if (!g_guiRunning.load())
            break;

        static bool wasF11 = false;
        bool isF11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (isF11 && !wasF11)
        {
            ToggleCompanionGUIVisibility();
        }
        wasF11 = isF11;

        if (IsIconic(hwnd) || !IsWindowVisible(hwnd))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RECT rect;
        GetClientRect(hwnd, &rect);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)(rect.right - rect.left), (float)(rect.bottom - rect.top)));

        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;

        if (ImGui::Begin("Voxel Companion Window", nullptr, windowFlags))
        {
            RenderCompanionUI(hwnd);
        }
        ImGui::End();

        ImGui::Render();
        const float clearColor[4] = { 0.10f, 0.10f, 0.10f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0); // VSync

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    g_guiHwnd = nullptr;
}

void StartCompanionGUI()
{
    if (!g_guiRunning.load())
    {
        g_guiRunning.store(true);
        g_guiThread = std::thread(CompanionGUIThread);
    }
}

void StopCompanionGUI()
{
    if (g_guiRunning.load())
    {
        g_guiRunning.store(false);
        if (g_guiHwnd)
            PostMessageW(g_guiHwnd, WM_CLOSE, 0, 0);

        if (g_guiThread.joinable())
            g_guiThread.detach(); // Detach on unload to prevent loader lock deadlock
    }
}

