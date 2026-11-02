#include "ObjectExporter.h"
#include <unordered_map>

void RunAsyncExport(void* vbData, UINT vbSize, UINT stride,
    void* ibData, UINT ibSize, DXGI_FORMAT format,
    UINT startIndexLocation, INT baseVertexLocation,
    UINT indexCountPerInstance)
{
    g_IsExporting.store(true);
    std::vector<char> vbCopy((char*)vbData, (char*)vbData + vbSize);
    std::vector<char> ibCopy((char*)ibData, (char*)ibData + ibSize);

    std::thread([vbCopy = std::move(vbCopy), ibCopy = std::move(ibCopy),
                 stride, format, startIndexLocation, baseVertexLocation, indexCountPerInstance]() {
        ExportToObj(vbCopy.data(), (UINT)vbCopy.size(), stride,
                    ibCopy.data(), (UINT)ibCopy.size(), format,
                    startIndexLocation, baseVertexLocation, indexCountPerInstance);
        g_IsExporting.store(false);
    }).detach();
}

void ExportToObj(const void* vbData, UINT vbSize, UINT stride,
    const void* ibData, UINT ibSize, DXGI_FORMAT format,
    UINT startIndexLocation, INT baseVertexLocation,
    UINT indexCountPerInstance)
{
    if (stride < sizeof(float) * 3) {
        Logger::LogError() << "[ExportToObj] Invalid stride: too small for position data.";
        return;
    }

    static int counter = 0;
    std::ostringstream nameStream;
    nameStream << "Exported/obj_" << std::setw(4) << std::setfill('0') << counter++ << ".obj";
    std::filesystem::create_directories("Exported");

    std::ofstream obj(nameStream.str());
    if (!obj) return;

    const char* vb = reinterpret_cast<const char*>(vbData);
    UINT totalVertexCount = vbSize / stride;

    // ----- Determine index range -----
    UINT totalIndexCount = 0;
    if (ibData && ibSize > 0) {
        if (format == DXGI_FORMAT_R16_UINT)
            totalIndexCount = ibSize / sizeof(uint16_t);
        else if (format == DXGI_FORMAT_R32_UINT)
            totalIndexCount = ibSize / sizeof(uint32_t);
        else {
            Logger::LogError() << "[ExportToObj] Unsupported index format: " << format;
            return;
        }
    }

    // If indexCountPerInstance was not provided, use all indices after startIndexLocation
    UINT useIndexCount = (indexCountPerInstance > 0)
        ? indexCountPerInstance
        : (totalIndexCount > startIndexLocation ? totalIndexCount - startIndexLocation : 0);

    // Clamp to what's actually available
    if (startIndexLocation + useIndexCount > totalIndexCount)
        useIndexCount = (totalIndexCount > startIndexLocation) ? totalIndexCount - startIndexLocation : 0;

    // ----- Collect raw indices for this sub-mesh -----
    // Raw index values already include BaseVertexLocation offset applied by GPU
    // We apply baseVertexLocation ourselves to get actual VB positions
    const char* ib = (ibData && ibSize > 0) ? reinterpret_cast<const char*>(ibData) : nullptr;

    // Map from original VB index -> remapped OBJ index (1-based)
    std::unordered_map<UINT, UINT> indexRemap;
    std::vector<UINT> faces; // triples of remapped indices
    UINT nextObjIdx = 1;

    if (ib && useIndexCount >= 3) {
        for (UINT i = 0; i + 2 < useIndexCount; i += 3) {
            UINT rawIdx = startIndexLocation + i;
            UINT r0, r1, r2;

            if (format == DXGI_FORMAT_R16_UINT) {
                const uint16_t* indices = reinterpret_cast<const uint16_t*>(ib);
                r0 = (UINT)indices[rawIdx];
                r1 = (UINT)indices[rawIdx + 1];
                r2 = (UINT)indices[rawIdx + 2];
            } else {
                const uint32_t* indices = reinterpret_cast<const uint32_t*>(ib);
                r0 = indices[rawIdx];
                r1 = indices[rawIdx + 1];
                r2 = indices[rawIdx + 2];
            }

            // Apply BaseVertexLocation (can be negative, clamped to 0)
            UINT v0 = (baseVertexLocation >= 0 || (UINT)(-baseVertexLocation) <= r0)
                ? (UINT)((INT)r0 + baseVertexLocation) : 0;
            UINT v1 = (baseVertexLocation >= 0 || (UINT)(-baseVertexLocation) <= r1)
                ? (UINT)((INT)r1 + baseVertexLocation) : 0;
            UINT v2 = (baseVertexLocation >= 0 || (UINT)(-baseVertexLocation) <= r2)
                ? (UINT)((INT)r2 + baseVertexLocation) : 0;

            if (v0 >= totalVertexCount || v1 >= totalVertexCount || v2 >= totalVertexCount) {
                Logger::LogWarning() << "[ExportToObj] Skipping face with out-of-range index: "
                    << v0 << ", " << v1 << ", " << v2
                    << " (vertexCount=" << totalVertexCount << ")";
                continue;
            }

            // Remap to compact OBJ indices
            auto remap = [&](UINT v) -> UINT {
                auto it = indexRemap.find(v);
                if (it != indexRemap.end()) return it->second;
                indexRemap[v] = nextObjIdx;
                return nextObjIdx++;
            };

            faces.push_back(remap(v0));
            faces.push_back(remap(v1));
            faces.push_back(remap(v2));
        }

        // Write vertices in remapped order
        // Build reverse map: objIdx -> vbIdx
        std::vector<UINT> orderedVerts(nextObjIdx - 1);
        for (const auto& [vbIdx, objIdx] : indexRemap)
            orderedVerts[objIdx - 1] = vbIdx;

        for (UINT i = 0; i < (UINT)orderedVerts.size(); ++i) {
            UINT vbIdx = orderedVerts[i];
            const float* pos = reinterpret_cast<const float*>(vb + vbIdx * stride);
            if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) {
                obj << "v 0 0 0\n";
            } else {
                obj << "v " << pos[0] << " " << pos[2] << " " << -pos[1] << "\n";
            }
        }

        // Write faces
        for (UINT i = 0; i + 2 < (UINT)faces.size(); i += 3) {
            obj << "f " << faces[i] << " " << faces[i + 1] << " " << faces[i + 2] << "\n";
        }
    }
    else {
        // No index buffer — write all vertices directly
        for (UINT i = 0; i < totalVertexCount; ++i) {
            const float* pos = reinterpret_cast<const float*>(vb + i * stride);
            if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) {
                obj << "v 0 0 0\n";
            } else {
                obj << "v " << pos[0] << " " << pos[2] << " " << -pos[1] << "\n";
            }
        }
    }

    obj.close();
    Logger::LogInfo() << "[ExportToObj] Saved: " << nameStream.str()
        << " (startIdx=" << startIndexLocation
        << ", baseVtx=" << baseVertexLocation
        << ", idxCount=" << useIndexCount << ")";
}


size_t HashBuffers(const void* vbData, size_t vbSize, const void* ibData, size_t ibSize)
{
    const uint8_t* vbBytes = reinterpret_cast<const uint8_t*>(vbData);
    const uint8_t* ibBytes = reinterpret_cast<const uint8_t*>(ibData);

    size_t hash = 14695981039346656037ull; // FNV-1a 64-bit offset basis
    for (size_t i = 0; i < vbSize; ++i)
        hash = (hash ^ vbBytes[i]) * 1099511628211ull;
    for (size_t i = 0; i < ibSize; ++i)
        hash = (hash ^ ibBytes[i]) * 1099511628211ull;
    return hash;
}

void RunAsyncMultiMeshExport(std::vector<SubMeshCapture> subMeshes)
{
    g_IsExporting.store(true);
    std::thread([subMeshes = std::move(subMeshes)]() mutable {
        static int multiCounter = 0;
        std::ostringstream nameStream;
        nameStream << "Exported/multi_" << std::setw(4) << std::setfill('0') << multiCounter++ << ".obj";
        std::filesystem::create_directories("Exported");

        std::ofstream obj(nameStream.str());
        if (!obj) {
            g_IsExporting.store(false);
            return;
        }

        UINT globalVertexOffset = 0;

        for (const auto& cap : subMeshes) {
            if (cap.vbData.empty() || cap.ibData.empty() || cap.stride < sizeof(float) * 3)
                continue;

            const char* vb = cap.vbData.data();
            const char* ib = cap.ibData.data();
            UINT vbSize    = (UINT)cap.vbData.size();
            UINT ibSize    = (UINT)cap.ibData.size();

            UINT totalVertexCount = vbSize / cap.stride;
            UINT totalIndexCount  = 0;
            if (cap.ibFormat == DXGI_FORMAT_R16_UINT)
                totalIndexCount = ibSize / sizeof(uint16_t);
            else if (cap.ibFormat == DXGI_FORMAT_R32_UINT)
                totalIndexCount = ibSize / sizeof(uint32_t);
            else
                continue;

            UINT useIndexCount = (cap.indexCountPerInstance > 0)
                ? cap.indexCountPerInstance
                : (totalIndexCount > cap.startIndexLocation ? totalIndexCount - cap.startIndexLocation : 0);

            if (cap.startIndexLocation + useIndexCount > totalIndexCount)
                useIndexCount = (totalIndexCount > cap.startIndexLocation) ? totalIndexCount - cap.startIndexLocation : 0;

            // Write vertices
            for (UINT i = 0; i < totalVertexCount; ++i) {
                const float* pos = reinterpret_cast<const float*>(vb + i * cap.stride);
                if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2]))
                    obj << "v 0 0 0\n";
                else
                    obj << "v " << pos[0] << " " << pos[2] << " " << -pos[1] << "\n";
            }

            // Write faces with global vertex offset
            for (UINT i = 0; i + 2 < useIndexCount; i += 3) {
                UINT rawIdx = cap.startIndexLocation + i;
                UINT r0, r1, r2;
                if (cap.ibFormat == DXGI_FORMAT_R16_UINT) {
                    const uint16_t* idx = reinterpret_cast<const uint16_t*>(ib);
                    r0 = idx[rawIdx]; r1 = idx[rawIdx + 1]; r2 = idx[rawIdx + 2];
                } else {
                    const uint32_t* idx = reinterpret_cast<const uint32_t*>(ib);
                    r0 = idx[rawIdx]; r1 = idx[rawIdx + 1]; r2 = idx[rawIdx + 2];
                }
                UINT v0 = (UINT)((INT)r0 + cap.baseVertexLocation);
                UINT v1 = (UINT)((INT)r1 + cap.baseVertexLocation);
                UINT v2 = (UINT)((INT)r2 + cap.baseVertexLocation);
                if (v0 >= totalVertexCount || v1 >= totalVertexCount || v2 >= totalVertexCount)
                    continue;
                // OBJ indices are 1-based + global offset
                obj << "f " << (globalVertexOffset + v0 + 1)
                    << " "  << (globalVertexOffset + v1 + 1)
                    << " "  << (globalVertexOffset + v2 + 1) << "\n";
            }

            globalVertexOffset += totalVertexCount;
        }

        obj.close();
        Logger::LogInfo() << "[MultiMesh] Saved: " << nameStream.str()
            << " (" << subMeshes.size() << " sub-mesh(es))";
        g_IsExporting.store(false);
    }).detach();
}