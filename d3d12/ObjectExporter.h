#pragma once
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <Windows.h>
#include "d3dx12.h"
#include <thread>
#include <vector>
#include <unordered_set>
#include <atomic>
#include "logger.h"

extern std::atomic<bool> g_IsExporting;

const UINT64 kMinSaveSize = 2 * 1024;
const UINT64 kMaxSaveSize = 1024ull * 1024 * 1024;

void RunAsyncExport(void* vbData, UINT vbSize, UINT stride,
    void* ibData, UINT ibSize, DXGI_FORMAT format,
    UINT startIndexLocation = 0, INT baseVertexLocation = 0,
    UINT indexCountPerInstance = 0);

void ExportToObj(const void* vbData, UINT vbSize, UINT stride,
    const void* ibData, UINT ibSize, DXGI_FORMAT format,
    UINT startIndexLocation = 0, INT baseVertexLocation = 0,
    UINT indexCountPerInstance = 0);

size_t HashBuffers(const void* vbData, size_t vbSize, const void* ibData, size_t ibSize);

struct SubMeshCapture {
    std::vector<char> vbData;
    std::vector<char> ibData;
    UINT              stride               = 0;
    DXGI_FORMAT       ibFormat             = DXGI_FORMAT_UNKNOWN;
    UINT              startIndexLocation   = 0;
    INT               baseVertexLocation   = 0;
    UINT              indexCountPerInstance = 0;
};

void RunAsyncMultiMeshExport(std::vector<SubMeshCapture> subMeshes);