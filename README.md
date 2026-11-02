# DX12 Model Extractor

**DX12 Model Extractor** is a lightweight 64-bit DirectX 12 proxy tool designed for **real-time 3D mesh extraction**, stream export, geometry filtering, and live debugging with a built-in ImGui companion control panel in Direct3D 12 games.

<p align="center">
  <b>💖 Підтримати проєкт / Support the project:</b><br><br>
  <a href="https://send.monobank.ua/jar/5uPPFW5FqV" target="_blank" rel="noopener noreferrer">
    <img src="./icons/UAH.png" alt="UAH" width="128" height="128" style="vertical-align: middle; margin-right: 6px;" />
  </a>
  &nbsp;&nbsp;&nbsp;&nbsp;|&nbsp;&nbsp;&nbsp;&nbsp;
  <a href="https://send.monobank.ua/jar/5uPPFW5FqV" target="_blank" rel="noopener noreferrer">
    <img src="./icons/DOLLAR.png" alt="USD" width="128" height="128" style="vertical-align: middle; margin-right: 6px;" />
  </a>
  &nbsp;&nbsp;&nbsp;&nbsp;|&nbsp;&nbsp;&nbsp;&nbsp;
  <a href="https://send.monobank.ua/jar/5uPPFW5FqV" target="_blank" rel="noopener noreferrer">
    <img src="./icons/EURO.png" alt="EUR" width="128" height="128" style="vertical-align: middle; margin-right: 6px;" />
  </a>
</p>

> [!Caution]
> This tool is intended solely for research, reverse engineering, and educational purposes.  
> It allows exploration of level geometry and 3D scenes for study.  
> It is not designed for piracy or unauthorized commercial asset redistribution.

---

## 📸 Preview

![Preview Animation](https://github.com/IlanVinograd/Voxel/blob/main/Img/Animation4.gif)
![Portal Gun Example](https://github.com/IlanVinograd/Voxel/blob/main/Img/img.png)

---

## ✨ Features

- **DirectX 12 64-bit Proxy (`d3d12.dll`)**: Intercepts Direct3D 12 device and command list calls seamlessly with minimal performance overhead.
- **Real-Time 3D Geometry Extraction**: Dumps currently rendered meshes directly into standard Wavefront `.obj` files.
- **Convenient Hotkeys**: Trigger mesh capture at any moment during gameplay by pressing **`C`** or **`N`**.
- **Companion Control Panel (ImGui)**:
  - Toggle UI visibility in-game at any time via **`F11`**.
  - **Always on Top** pinning mode.
  - Interactive **Geometry Tab** with one-click **CAPTURE**, Stream Export, and object filters.
  - Built-in `.obj` file manager: sort exports by Name, Size, or Date, reveal in Windows Explorer, preview directly on [3DViewer.net](https://3dviewer.net), or delete files.
  - Live **Debug Tab** with real-time log monitoring, message filtering (INFO / WARN / ERROR / DEBUG), and auto-scroll.
- **Advanced Geometry Filtering**:
  - Filter meshes by size range (`From Size` to `To Size`).
  - **Skip Submeshes [Experimental]**: Skip the first *N* qualifying submeshes to capture deeper components of complex multi-part models.
  - Hash-based deduplication to avoid exporting identical redundant meshes.
  - **Reset Hashes** button to clear export cache on demand.
- **Stream Export Mode**: Automatically captures and exports newly encountered geometry as you move through scenes.

---

## 🎮 Controls & Hotkeys

| Hotkey / Control | Action |
| :--- | :--- |
| **`C`** or **`N`** | Capture and export the currently rendered 3D mesh geometry to `.obj` |
| **`F11`** | Show / Hide the Companion Control Panel window |
| **CAPTURE (Button)** | Trigger mesh capture directly from the UI |
| **STREAM EXPORT** | Toggle continuous automatic mesh extraction |
| **Skip Submeshes [Exp.]** | Skip *N* qualifying submeshes during capture |
| **Reset Hashes** | Reset the hash cache to allow re-exporting previously captured geometry |

---

## 🚀 Usage Guide

1. **Build the project** using Visual Studio 2022 (x64) or obtain the pre-built `d3d12.dll` from `data/x64/`.
2. **Copy `d3d12.dll`** into the game folder, placing it next to the game's 64-bit executable (`.exe`).
3. **Launch the game** – the DX12 companion window will automatically initialize in the background.
4. Press **`F11`** to show or hide the control panel window as needed.
5. Press **`C`** or **`N`** (or click **CAPTURE** in the control panel) to export geometry from the current frame.
6. Check your extracted models inside the **`Exported/`** folder created in the game directory.

---

## 📁 Output Directory

- **Geometry**: Exported `.obj` files are saved to the `Exported/` directory next to the game `.exe`.
- **Logs**: Output to `d3d12.log` / `Voxel.log` and visible in real-time under the **Debug** tab of the control panel.

---

## 🛠️ Planned Features

- Direct3D 12 bound texture extraction (`.png`)
- Export to `.glTF` / `.glb` format
- Embedded in-game swapchain overlay rendering
- Standalone injector launcher (bypassing manual DLL placement)

---

## 🔨 Build Instructions

This project requires **Microsoft Visual Studio 2022** with the **Desktop development with C++** workload (64-bit only).

### 1. Clone the Repository

```bash
git clone https://github.com/IlanVinograd/DX12-Model-Extractor.git
cd DX12-Model-Extractor
```

### 2. Open the Solution

1. Open **Visual Studio 2022**.
2. Open `build/Voxel.sln`.

### 3. Configure and Build

1. Set the configuration to **Release** and platform to **x64** (or **Win64**).
2. Build the solution (**Build → Build Solution** or `Ctrl+Shift+B`).
3. The compiled DLL will be located at:
   ```text
   data/x64/d3d12.dll
   ```

### (Optional) Regenerate Solution with Premake5

To regenerate the Visual Studio solution and project files:

```cmd
premake5.bat
```
or:
```cmd
premake5.exe vs2022
```

---

## 📜 Credits

- Special thanks to **Elisha Riedlinger** for foundational proxy architecture references.
- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut for the UI framework.
