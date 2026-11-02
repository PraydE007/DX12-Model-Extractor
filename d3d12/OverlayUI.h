#pragma once

#include <Windows.h>
#include <atomic>
#include <string>
#include <vector>

extern std::atomic<bool> g_CaptureTriggered;
extern std::atomic<bool> g_AutoExport;
extern std::atomic<bool> g_IsExporting;
extern std::atomic<int>  g_FromFilter;
extern std::atomic<int>  g_ToFilter;
extern std::atomic<bool> g_SkipSubmeshes;
extern std::atomic<int>  g_SkipSubmeshCount;
extern std::atomic<bool> g_MultiMeshExport;
extern std::atomic<bool> g_SubMeshCountEnabled;
extern std::atomic<int>  g_SubMeshCount;

void StartCompanionGUI();
void StopCompanionGUI();
void ToggleCompanionGUIVisibility();
void ResetExportedHashes();

