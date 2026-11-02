workspace "Voxel"
    configurations { "Debug", "Release" }
    platforms { "Win64" }
    location "build"
    objdir ("build/obj")
    buildlog ("build/log/%{prj.name}.log")
    buildoptions { "-std:c++latest" }

---------------------------
-- Project: d3d12
---------------------------
project "d3d12"
    kind "SharedLib"
    language "C++"
    targetname "d3d12"
    targetextension ".dll"
    characterset ("MBCS")
    staticruntime "On"

    files {
        "d3d12/*.h",
        "d3d12/*.cpp",
        "d3d12/*.rc",
        "d3d12/*.def",
        "d3d12/*.asm",
        "imgui/*.cpp",
        "imgui/backends/imgui_impl_dx11.cpp",
        "imgui/backends/imgui_impl_win32.cpp",
        "imgui/*.h",
        "imgui/backends/*.h"
    }

    removefiles {
        "imgui/imgui_demo.cpp"
    }

    vpaths {
        ["d3d12"]           = { "d3d12/**.cpp", "d3d12/**.h" },
        ["imgui"]           = { "imgui/**.cpp", "imgui/**.h" },
        ["ASM Files"]       = { "d3d12/**.asm" },
        ["Resource Files"]  = { "d3d12/**.rc", "d3d12/**.def" }
    }

    includedirs {
        "d3d12",
        "imgui",
        "imgui/backends"
    }

    links {
        "d3d11",
        "dxgi"
    }

    defines {
        "rsc_FileDescription=\"Direct3D12 Proxy DLL\"",
        "rsc_OriginalFilename=\"d3d12.dll\""
    }

    filter "configurations:Debug"
        defines { "DEBUG" }
        symbols "On"

    filter "configurations:Release"
        defines { "NDEBUG" }
        optimize "On"

    filter "platforms:Win64"
        architecture "x64"
        targetdir "data/x64"
