#include "WrappedCommandList.h"
#include "WrappedDevice.h"
#include "ObjectExporter.h"
#include "OverlayUI.h"
#include "d3dx12.h"
#include <d3d12.h>
#include <chrono>
#include <mutex>

D3D12_VERTEX_BUFFER_VIEW lastVBView = {};
D3D12_INDEX_BUFFER_VIEW lastIBView = {};
std::vector<ResourceRange> g_Resources;
std::mutex g_resourcesMutex;

static std::unordered_set<size_t> g_exportedHashes;
static std::mutex g_hashesMutex;

// ── Multi-mesh accumulator ────────────────────────────────────────────────────
static std::vector<SubMeshCapture> g_pendingSubMeshes;
static std::mutex                  g_pendingMutex;

void ResetExportedHashes() {
    std::lock_guard<std::mutex> lock(g_hashesMutex);
    g_exportedHashes.clear();
    Logger::LogInfo() << "[ObjectExporter] Reset hashes.";
}

void RegisterBufferResource(void* ppvResource, UINT64 width, D3D12_RESOURCE_DIMENSION dim)
{
    if (!ppvResource || dim != D3D12_RESOURCE_DIMENSION_BUFFER)
        return;

    IUnknown* unk = reinterpret_cast<IUnknown*>(ppvResource);
    if (!unk) return;

    ID3D12Resource* res = nullptr;
    if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&res))) && res)
    {
        D3D12_GPU_VIRTUAL_ADDRESS gpuAddr = res->GetGPUVirtualAddress();
        if (gpuAddr != 0)
        {
            ResourceRange entry = {
                .base = gpuAddr,
                .size = width,
                .resource = res
            };

            {
                std::lock_guard<std::mutex> lock(g_resourcesMutex);
                g_Resources.push_back(entry);
            }

            Logger::LogInfo() << "[RegisterResource] Added buffer @ 0x"
                << std::hex << gpuAddr
                << " size: " << std::dec << width;
        }
        res->Release();
    }
}

void STDMETHODCALLTYPE WrappedCommandList::DrawInstanced(
    UINT VertexCountPerInstance,
    UINT InstanceCount,
    UINT StartVertexLocation,
    UINT StartInstanceLocation)
{
    SHORT keyState0 = GetAsyncKeyState('0');
    static bool was0Pressed = false;
    bool is0Pressed = (keyState0 & 0x8000) != 0;

    SHORT keyStateN = GetAsyncKeyState('N');
    bool isNPressed = (keyStateN & 0x8000) != 0;
    static bool wasNPressed = false;

    SHORT keyStateC = GetAsyncKeyState('C');
    bool isCPressed = (keyStateC & 0x8000) != 0;
    static bool wasCPressed = false;

    bool manualCapture = (is0Pressed && !was0Pressed) || (isNPressed && !wasNPressed) || (isCPressed && !wasCPressed) || g_CaptureTriggered.load();
    was0Pressed = is0Pressed;
    wasNPressed = isNPressed;
    wasCPressed = isCPressed;

    bool shouldExport = manualCapture || g_AutoExport.load();

    if (!shouldExport)
    {
        m_real->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
        return;
    }

    D3D12_VERTEX_BUFFER_VIEW vbView = (m_currentVB.BufferLocation != 0 && m_currentVB.BufferLocation != ~0ull) ? m_currentVB : lastVBView;
    if (vbView.BufferLocation == 0 || vbView.BufferLocation == ~0ull)
    {
        m_real->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
        return;
    }

    ID3D12Device* device = nullptr;
    if (SUCCEEDED(m_real->GetDevice(__uuidof(ID3D12Device), (void**)&device)) && device)
    {
        ID3D12Resource* vbRes = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_resourcesMutex);
            for (const auto& entry : g_Resources)
            {
                if (vbView.BufferLocation >= entry.base &&
                    vbView.BufferLocation < entry.base + entry.size)
                {
                    vbRes = entry.resource;
                    break;
                }
            }
        }

        if (vbRes)
        {
            UINT64 vbOffset = vbView.BufferLocation - vbRes->GetGPUVirtualAddress();
            UINT vbSize = vbView.SizeInBytes;

            UINT minFilter = (UINT)g_FromFilter.load();
            UINT maxFilter = (UINT)g_ToFilter.load();
            if (minFilter < (UINT)kMinSaveSize) minFilter = (UINT)kMinSaveSize;

            if (vbSize >= minFilter && vbSize <= maxFilter &&
                vbOffset + vbSize <= vbRes->GetDesc().Width)
            {
                CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_READBACK);
                CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);

                ID3D12Resource* vbReadback = nullptr;
                if (SUCCEEDED(device->CreateCommittedResource(
                    &heapProps,
                    D3D12_HEAP_FLAG_NONE,
                    &desc,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(&vbReadback))))
                {
                    ID3D12CommandAllocator* alloc = nullptr;
                    ID3D12GraphicsCommandList* cmdList = nullptr;
                    ID3D12CommandQueue* queue = nullptr;
                    ID3D12Fence* fence = nullptr;
                    HANDLE fenceEvent = nullptr;

                    if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
                        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmdList))))
                    {
                        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                            vbRes, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                        cmdList->ResourceBarrier(1, &barrier);
                        cmdList->CopyBufferRegion(vbReadback, 0, vbRes, vbOffset, vbSize);
                        cmdList->Close();

                        D3D12_COMMAND_QUEUE_DESC qdesc = {};
                        qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                        if (SUCCEEDED(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue))) &&
                            SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
                        {
                            fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                            queue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&cmdList);
                            queue->Signal(fence, 1);
                            fence->SetEventOnCompletion(1, fenceEvent);
                            WaitForSingleObject(fenceEvent, INFINITE);

                            void* vbData = nullptr;
                            if (SUCCEEDED(vbReadback->Map(0, nullptr, &vbData)))
                            {
                                RunAsyncExport(vbData, vbSize, vbView.StrideInBytes, nullptr, 0, DXGI_FORMAT_UNKNOWN);
                                vbReadback->Unmap(0, nullptr);
                            }

                            CloseHandle(fenceEvent);
                            fence->Release();
                            queue->Release();
                        }

                        cmdList->Release();
                        alloc->Release();
                    }

                    vbReadback->Release();
                }
            }
        }

        device->Release();
    }

    m_real->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE WrappedCommandList::DrawIndexedInstanced(
    UINT IndexCountPerInstance,
    UINT InstanceCount,
    UINT StartIndexLocation,
    INT BaseVertexLocation,
    UINT StartInstanceLocation)
{
    static bool g_deviceLost = false;
    static std::chrono::steady_clock::time_point lastSaveTime = std::chrono::steady_clock::now();
    const std::chrono::milliseconds kMinInterval(200);

    if (g_deviceLost)
    {
        m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
        return;
    }

    SHORT keyStateN = GetAsyncKeyState('N');
    bool isNPressed = (keyStateN & 0x8000) != 0;
    static bool wasNPressed = false;

    SHORT keyStateC = GetAsyncKeyState('C');
    bool isCPressed = (keyStateC & 0x8000) != 0;
    static bool wasCPressed = false;

    bool manualCapture = (isNPressed && !wasNPressed) || (isCPressed && !wasCPressed) || g_CaptureTriggered.exchange(false);
    wasNPressed = isNPressed;
    wasCPressed = isCPressed;

    bool shouldExport = manualCapture || g_AutoExport.load();

    // Fast path: if not capturing, execute and return immediately with 0 overhead
    if (!shouldExport)
    {
        m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
        return;
    }

    D3D12_VERTEX_BUFFER_VIEW vbView = (m_currentVB.BufferLocation != 0 && m_currentVB.BufferLocation != ~0ull) ? m_currentVB : lastVBView;
    D3D12_INDEX_BUFFER_VIEW ibView  = (m_currentIB.BufferLocation != 0 && m_currentIB.BufferLocation != ~0ull) ? m_currentIB : lastIBView;

    if (vbView.BufferLocation == 0 || vbView.BufferLocation == ~0ull ||
        ibView.BufferLocation == 0 || ibView.BufferLocation == ~0ull)
    {
        m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
        return;
    }

    // ── Throttle only in stream/AutoExport mode ──────────────────────────────
    // In manual-capture mode we must NOT throttle: all sub-meshes of a single
    // model are drawn in the same frame and must all pass through.
    if (!manualCapture && g_AutoExport.load())
    {
        auto now = std::chrono::steady_clock::now();
        if (now - lastSaveTime < kMinInterval)
        {
            m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
            return;
        }
        lastSaveTime = now;
    }
    if (manualCapture)
        lastSaveTime = std::chrono::steady_clock::now(); // reset for next auto-export session

    ID3D12Device* device = nullptr;
    if (FAILED(m_real->GetDevice(__uuidof(ID3D12Device), (void**)&device)) || !device)
    {
        m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
        return;
    }

    if (device->GetDeviceRemovedReason() != S_OK)
    {
        Logger::LogError() << "[DrawIndexedInstanced] Device was removed.";
        g_deviceLost = true;
        device->Release();
        m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
        return;
    }

    ID3D12Resource* vbResource = nullptr;
    ID3D12Resource* ibResource = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_resourcesMutex);
        for (const auto& entry : g_Resources)
        {
            if (!vbResource && vbView.BufferLocation >= entry.base && vbView.BufferLocation < entry.base + entry.size)
                vbResource = entry.resource;
            if (!ibResource && ibView.BufferLocation >= entry.base && ibView.BufferLocation < entry.base + entry.size)
                ibResource = entry.resource;
            if (vbResource && ibResource) break;
        }
    }

    if (!vbResource || !ibResource)
    {
        Logger::LogWarning() << "[DrawIndexedInstanced] Buffers not found for VB@0x"
            << std::hex << vbView.BufferLocation << " IB@0x" << ibView.BufferLocation << std::dec;
        device->Release();
        m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
        return;
    }

    UINT64 vbOffset = vbView.BufferLocation - vbResource->GetGPUVirtualAddress();
    UINT64 ibOffset = ibView.BufferLocation - ibResource->GetGPUVirtualAddress();
    UINT vbSize = vbView.SizeInBytes;
    UINT ibSize = ibView.SizeInBytes;

    UINT minFilter = (UINT)g_FromFilter.load();
    UINT maxFilter = (UINT)g_ToFilter.load();
    if (minFilter < (UINT)kMinSaveSize) minFilter = (UINT)kMinSaveSize;

    if (vbSize < minFilter || ibSize < minFilter)
    {
        device->Release();
        m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
        return;
    }
    if (vbSize > maxFilter || ibSize > maxFilter)
    {
        device->Release();
        m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
        return;
    }

    auto vbDesc = vbResource->GetDesc();
    auto ibDesc = ibResource->GetDesc();
    if (vbOffset + vbSize > vbDesc.Width || ibOffset + ibSize > ibDesc.Width)
    {
        Logger::LogError() << "[DrawIndexedInstanced] Readback exceeds resource bounds.";
        device->Release();
        m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
        return;
    }

    // ── Draw-call-parameter hash ─────────────────────────────────────────────
    // Use draw-call params (NOT full buffer content) so that sub-meshes sharing
    // the same VB/IB buffer are each treated as a unique capture entry.
    size_t drawHash = 14695981039346656037ull;
    auto hashCombine = [&](auto v) {
        drawHash ^= std::hash<decltype(v)>{}(v);
        drawHash *= 1099511628211ull;
    };
    hashCombine(vbView.BufferLocation);
    hashCombine(ibView.BufferLocation);
    hashCombine((UINT64)StartIndexLocation);
    hashCombine((UINT64)(UINT)BaseVertexLocation);
    hashCombine((UINT64)IndexCountPerInstance);

    // ── Skip-submesh logic ───────────────────────────────────────────────────
    // s_skipCounter counts qualifying draw calls per capture session.
    // It resets to 0 whenever manualCapture fires (new session begins).
    // When Skip Submeshes is enabled, the first N draws are skipped.
    static int s_skipCounter = 0;
    if (manualCapture)
        s_skipCounter = 0; // new session — restart counter

    if (g_SkipSubmeshes.load())
    {
        int skipN = g_SkipSubmeshCount.load();
        if (s_skipCounter < skipN)
        {
            ++s_skipCounter;
            Logger::LogInfo() << "[DrawIndexedInstanced] Skip-submesh: skipping draw #"
                << s_skipCounter << " of " << skipN;
            device->Release();
            m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
            return;
        }
    }
    ++s_skipCounter;

    // ── Duplicate-draw check ─────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lock(g_hashesMutex);
        if (!g_exportedHashes.insert(drawHash).second)
        {
            Logger::LogInfo() << "[DrawIndexedInstanced] Skipped duplicate draw (startIdx="
                << StartIndexLocation << ").";
            device->Release();
            m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
            return;
        }
    }

    // ── GPU readback ─────────────────────────────────────────────────────────
    ID3D12Resource* vbReadback = nullptr;
    ID3D12Resource* ibReadback = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    bool success = true;

    CD3DX12_HEAP_PROPERTIES readbackHeap(D3D12_HEAP_TYPE_READBACK);
    CD3DX12_RESOURCE_DESC vbDescBuffer = CD3DX12_RESOURCE_DESC::Buffer(vbSize);
    CD3DX12_RESOURCE_DESC ibDescBuffer = CD3DX12_RESOURCE_DESC::Buffer(ibSize);

    if (FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &vbDescBuffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&vbReadback))))
    {
        Logger::LogError() << "[DrawIndexedInstanced] Failed to create vbReadback.";
        success = false;
    }

    if (success && FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &ibDescBuffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ibReadback))))
    {
        Logger::LogError() << "[DrawIndexedInstanced] Failed to create ibReadback.";
        success = false;
    }

    if (success && FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
    {
        Logger::LogError() << "[DrawIndexedInstanced] Failed to create command allocator.";
        success = false;
    }

    if (success && FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&cmdList))))
    {
        Logger::LogError() << "[DrawIndexedInstanced] Failed to create command list.";
        success = false;
    }

    if (success)
    {
        CD3DX12_RESOURCE_BARRIER barriers[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(vbResource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(ibResource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE)
        };
        cmdList->ResourceBarrier(2, barriers);
        cmdList->CopyBufferRegion(vbReadback, 0, vbResource, vbOffset, vbSize);
        cmdList->CopyBufferRegion(ibReadback, 0, ibResource, ibOffset, ibSize);
        cmdList->Close();
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (success && FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
    {
        Logger::LogError() << "[DrawIndexedInstanced] Failed to create command queue.";
        success = false;
    }

    if (success && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        Logger::LogError() << "[DrawIndexedInstanced] Failed to create fence.";
        success = false;
    }

    if (success)
    {
        fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent)
        {
            Logger::LogError() << "[DrawIndexedInstanced] Failed to create fence event.";
            success = false;
        }
    }

    if (success)
    {
        queue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&cmdList);
        queue->Signal(fence, 1);
        fence->SetEventOnCompletion(1, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }

    if (success && device->GetDeviceRemovedReason() != S_OK)
    {
        Logger::LogError() << "[DrawIndexedInstanced] Device lost during readback.";
        g_deviceLost = true;
        success = false;
    }

    if (success)
    {
        void* vbData = nullptr;
        void* ibData = nullptr;

        if (SUCCEEDED(vbReadback->Map(0, nullptr, &vbData)) &&
            SUCCEEDED(ibReadback->Map(0, nullptr, &ibData)))
        {
            if (g_MultiMeshExport.load())
            {
                // ── Multi-mesh accumulation mode ─────────────────────────────
                SubMeshCapture cap;
                cap.vbData                = std::vector<char>((char*)vbData, (char*)vbData + vbSize);
                cap.ibData                = std::vector<char>((char*)ibData, (char*)ibData + ibSize);
                cap.stride                = vbView.StrideInBytes;
                cap.ibFormat              = ibView.Format;
                cap.startIndexLocation    = StartIndexLocation;
                cap.baseVertexLocation    = BaseVertexLocation;
                cap.indexCountPerInstance = IndexCountPerInstance;

                std::vector<SubMeshCapture> toExport;
                {
                    std::lock_guard<std::mutex> lock(g_pendingMutex);
                    g_pendingSubMeshes.push_back(std::move(cap));

                    bool shouldFlush = false;
                    if (g_SubMeshCountEnabled.load())
                    {
                        // Flush when we have collected exactly the requested number
                        shouldFlush = ((int)g_pendingSubMeshes.size() >= g_SubMeshCount.load());
                    }
                    else
                    {
                        // Without a fixed count: flush on manual capture trigger only
                        shouldFlush = manualCapture;
                    }

                    if (shouldFlush)
                    {
                        toExport = std::move(g_pendingSubMeshes);
                        g_pendingSubMeshes.clear();
                    }
                }

                // Reset hashes outside the pending lock to avoid nested lock ordering
                if (!toExport.empty())
                {
                    {
                        std::lock_guard<std::mutex> hlock(g_hashesMutex);
                        g_exportedHashes.clear();
                    }
                    Logger::LogInfo() << "[MultiMesh] Flushing " << toExport.size() << " sub-mesh(es).";
                    RunAsyncMultiMeshExport(std::move(toExport));
                }
                else
                {
                    size_t pendingCount;
                    { std::lock_guard<std::mutex> lock(g_pendingMutex); pendingCount = g_pendingSubMeshes.size(); }
                    Logger::LogInfo() << "[MultiMesh] Accumulated sub-mesh #"
                        << pendingCount
                        << " (startIdx=" << StartIndexLocation
                        << ", baseVtx=" << BaseVertexLocation
                        << ", idxCount=" << IndexCountPerInstance << ")";
                }
            }
            else
            {
                // ── Normal single-mesh mode ──────────────────────────────────
                Logger::LogInfo() << "[DrawIndexedInstanced] Readback OK. startIdx=" << StartIndexLocation
                    << " baseVtx=" << BaseVertexLocation << " idxCount=" << IndexCountPerInstance;
                RunAsyncExport(vbData, vbSize, vbView.StrideInBytes, ibData, ibSize, ibView.Format,
                    StartIndexLocation, BaseVertexLocation, IndexCountPerInstance);
            }

            vbReadback->Unmap(0, nullptr);
            ibReadback->Unmap(0, nullptr);
        }
        else
        {
            Logger::LogError() << "[DrawIndexedInstanced] Failed to map readback buffers.";
        }
    }

    if (fenceEvent) CloseHandle(fenceEvent);
    if (fence) fence->Release();
    if (queue) queue->Release();
    if (cmdList) cmdList->Release();
    if (allocator) allocator->Release();
    if (vbReadback) vbReadback->Release();
    if (ibReadback) ibReadback->Release();

    device->Release();
    m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE WrappedCommandList::ExecuteIndirect(
    ID3D12CommandSignature* pCommandSignature,
    UINT MaxCommandCount,
    ID3D12Resource* pArgumentBuffer,
    UINT64 ArgumentBufferOffset,
    ID3D12Resource* pCountBuffer,
    UINT64 CountBufferOffset)
{
    m_real->ExecuteIndirect(pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset);

    if (lastVBView.BufferLocation == 0 || lastVBView.BufferLocation == ~0ull ||
        lastIBView.BufferLocation == 0 || lastIBView.BufferLocation == ~0ull)
        return;

    ID3D12Device* device = nullptr;
    if (FAILED(m_real->GetDevice(__uuidof(ID3D12Device), (void**)&device)) || !device)
        return;

    SHORT keyState = GetAsyncKeyState('9');
    static bool wasMPressed = false;
    bool isMDown = (keyState & 0x8000) != 0;

    if (!isMDown || wasMPressed) {
        device->Release();
        return;
    }
    wasMPressed = isMDown;

    const UINT argsSize = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) * MaxCommandCount;

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_READBACK);
    CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(argsSize);

    ID3D12Resource* readback = nullptr;
    if (FAILED(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&readback))))
    {
        device->Release();
        return;
    }

    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&cmdList))))
    {
        readback->Release();
        device->Release();
        return;
    }

    cmdList->CopyBufferRegion(readback, 0, pArgumentBuffer, ArgumentBufferOffset, argsSize);
    cmdList->Close();

    D3D12_COMMAND_QUEUE_DESC qdesc = {};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        cmdList->Release(); allocator->Release(); readback->Release(); device->Release();
        return;
    }

    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent)
    {
        fence->Release(); queue->Release(); cmdList->Release(); allocator->Release(); readback->Release(); device->Release();
        return;
    }

    queue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&cmdList);
    queue->Signal(fence, 1);
    fence->SetEventOnCompletion(1, fenceEvent);
    WaitForSingleObject(fenceEvent, INFINITE);

    void* mapped = nullptr;
    if (SUCCEEDED(readback->Map(0, nullptr, &mapped)))
    {
        D3D12_DRAW_INDEXED_ARGUMENTS* args = reinterpret_cast<D3D12_DRAW_INDEXED_ARGUMENTS*>(mapped);
        for (UINT i = 0; i < MaxCommandCount; ++i)
        {
            const auto& a = args[i];
            if (a.IndexCountPerInstance < 32 || a.InstanceCount == 0)
                continue;

            ID3D12Resource* vbRes = nullptr;
            ID3D12Resource* ibRes = nullptr;
            for (const auto& entry : g_Resources)
            {
                if (!vbRes && lastVBView.BufferLocation >= entry.base && lastVBView.BufferLocation < entry.base + entry.size)
                    vbRes = entry.resource;
                if (!ibRes && lastIBView.BufferLocation >= entry.base && lastIBView.BufferLocation < entry.base + entry.size)
                    ibRes = entry.resource;
                if (vbRes && ibRes) break;
            }

            if (!vbRes || !ibRes) continue;

            UINT64 vbOffset = lastVBView.BufferLocation - vbRes->GetGPUVirtualAddress();
            UINT64 ibOffset = lastIBView.BufferLocation - ibRes->GetGPUVirtualAddress();
            UINT vbSize = lastVBView.SizeInBytes;
            UINT ibSize = lastIBView.SizeInBytes;

            if (vbSize < kMinSaveSize || ibSize < kMinSaveSize || vbSize > kMaxSaveSize || ibSize > kMaxSaveSize)
                continue;

            ID3D12Resource* vbReadback = nullptr;
            ID3D12Resource* ibReadback = nullptr;

            CD3DX12_RESOURCE_DESC vbDesc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);
            CD3DX12_RESOURCE_DESC ibDesc = CD3DX12_RESOURCE_DESC::Buffer(ibSize);

            if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &vbDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&vbReadback))) ||
                FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &ibDesc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ibReadback))))
                continue;

            ID3D12CommandAllocator* alloc2 = nullptr;
            ID3D12GraphicsCommandList* cmd2 = nullptr;
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc2))) ||
                FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc2, nullptr, IID_PPV_ARGS(&cmd2))))
            {
                vbReadback->Release(); ibReadback->Release();
                continue;
            }

            CD3DX12_RESOURCE_BARRIER barriers2[2] = {
                CD3DX12_RESOURCE_BARRIER::Transition(vbRes, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(ibRes, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE)
            };
            cmd2->ResourceBarrier(2, barriers2);
            cmd2->CopyBufferRegion(vbReadback, 0, vbRes, vbOffset, vbSize);
            cmd2->CopyBufferRegion(ibReadback, 0, ibRes, ibOffset, ibSize);
            cmd2->Close();

            queue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&cmd2);
            queue->Signal(fence, 2);
            fence->SetEventOnCompletion(2, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);

            void* vbData = nullptr;
            void* ibData = nullptr;

            if (SUCCEEDED(vbReadback->Map(0, nullptr, &vbData)) &&
                SUCCEEDED(ibReadback->Map(0, nullptr, &ibData)))
            {
                size_t hash = HashBuffers(vbData, vbSize, ibData, ibSize);
                bool isDuplicate = false;
                {
                    std::lock_guard<std::mutex> hlock(g_hashesMutex);
                    isDuplicate = !g_exportedHashes.insert(hash).second;
                }
                if (!isDuplicate)
                {
                    RunAsyncExport(vbData, vbSize, lastVBView.StrideInBytes, ibData, ibSize, lastIBView.Format,
                        a.StartIndexLocation, a.BaseVertexLocation, a.IndexCountPerInstance);
                }
                vbReadback->Unmap(0, nullptr);
                ibReadback->Unmap(0, nullptr);
            }

            vbReadback->Release();
            ibReadback->Release();
            cmd2->Release();
            alloc2->Release();
        }

        readback->Unmap(0, nullptr);
    }

    CloseHandle(fenceEvent);
    fence->Release();
    queue->Release();
    cmdList->Release();
    allocator->Release();
    readback->Release();
    device->Release();
}

void STDMETHODCALLTYPE WrappedCommandList::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* pView) {
    if (pView) {
        m_currentIB = *pView;
        lastIBView = *pView;
    } else {
        m_currentIB = {};
    }

    m_real->IASetIndexBuffer(pView);
}

void STDMETHODCALLTYPE WrappedCommandList::IASetVertexBuffers(UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW* pViews) {
    if (pViews && NumViews > 0) {
        for (UINT i = 0; i < NumViews; ++i) {
            if (pViews[i].BufferLocation != 0 && pViews[i].StrideInBytes >= sizeof(float) * 3) {
                m_currentVB = pViews[i];
                lastVBView = pViews[i];
                break;
            }
        }
    }

    m_real->IASetVertexBuffers(StartSlot, NumViews, pViews);
}

void STDMETHODCALLTYPE WrappedCommandList::SetGraphicsRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    Logger::LogInfo() << "SetGraphicsRootShaderResourceView[" << RootParameterIndex << "] -> GPU addr: 0x" << std::hex << BufferLocation;

    for (const auto& entry : g_Resources) {
        if (BufferLocation >= entry.base && BufferLocation < entry.base + entry.size) {
            Logger::LogInfo() << "Matched resource: " << entry.resource;
        }
    }

    m_real->SetGraphicsRootShaderResourceView(RootParameterIndex, BufferLocation);
}
