# Anatomical Engine (v9.0)
**Dependency-Free C++17 Spatial SDF Engine, Spectral World Compiler & Headless CPU Raycaster**

![Anatomical Engine Showcase](showcase_render.png)

**Anatomical Engine** is an experimental R&D spatial engine and procedural terrain pipeline built from scratch in **C++17, OpenGL 3.3, GLSL, HLSL, and Python**. It explores replacing heavy polygon meshes and gigabyte heightmap textures with compact 2D Fourier spectral harmonics, non-periodic $SO(3)$ Escher rotation matrices, and $O(1)$ spatial hash grids for real-time destructible worlds.

---

## Key Engineering Benchmarks

| Subsystem | Metric / Performance | Technical Implementation |
| :--- | :--- | :--- |
| **Spectral World Compiler** | **927 : 1 Compression** (`5.49 MB` OBJ $\rightarrow$ `6.06 KB` `.acont`) | Extracts top-24 2D Fourier harmonics per sector from heightmaps/images (`world_compiler.py`) |
| **Continent Streaming** | **12 × 12 km** seamless terrain at **100+ FPS** | Dynamic `3×3` active chunk window (`216` harmonics on GPU) with `[R]` Hot-Reload |
| **Headless CPU Raycaster** | **~2.98 Million rays/sec** (Single-process C-API) | Pure C++17 analytical sphere-tracing (`AnatomicalCore.hpp`) decoupled from GPU |
| **Destructible Voxel-Free Grid** | **$O(1)$ Spatial Lookup** (Zero boundary seams) | 3D `SpatialHashGrid` with `3×3×3` cross-cell Halo Insertion |
| **Network State Sync** | **0 Bytes/sec** idle traffic / **256-Byte** edit packets | 3-tier `HierarchicalHashRouter` (Merkle tree) + FNV-1a checksummed `ESNPacket256` |

---

## Core Architecture & Modules

### 1. Header-Only C++17 Core (`AnatomicalCore.hpp`)
* **Zero External Dependencies:** Pure standard C++17 (`<cmath>`, `<vector>`, `<cstdint>`) with strict `NaN`/`Inf` and zero-division guards.
* **Analytical SDF & Smooth CSG:** Primitives (Spheres, Boxes, Gyroids), polynomial smooth unions/subtractions, and non-periodic $SO(3)$ orthogonal rotation matrices for rock micro-detail.
* **Seamless `SpatialHashGrid`:** Stores real-time boolean craters across 500m cells using `3×3×3` neighborhood Halo Insertion to eliminate chunk-border artifacts.
* **Deterministic Network & Merkle Router:** `ESNPacket256` with FNV-1a integrity verification paired with a 256-region Merkle hash tree (`uint64_t` root state verification).

### 2. Spectral World Compiler & Mesher (`world_compiler.py` & `continent_to_obj.py`)
* **Recipe-Driven Pipeline (`world_recipe.json`):** Slices large heightfields or images into chunk grids, compresses each sector into 24 spectral harmonics, and packs them into a binary `.acont` atlas.
* **Industry Interoperability (`continent_to_obj.py`):** Reconstructs any `.acont` binary world into a standard Wavefront `.obj` triangle mesh with smooth vertex normals and UVs for **Blender**, **Unreal Engine 5**, or **Unity**.

### 3. Headless AI / RL Bridge (`AnatomicalCAPI.cpp` & `ai_agent_bridge.py`)
* Exposes an `extern "C"` ABI (`AnatomicalCore.dll`) allowing Python reinforcement learning agents, autonomous drones, or game servers to run batch LiDAR raycasts and SDF collision queries without a GPU.

### 4. Interactive GPU Sandbox (`main.cpp`)
* Real-time OpenGL 3.3 dual-pass raymarcher featuring planetary-scale horizons, 105 km underground gyroid caverns, secondary reflections on liquid metal oceans, procedural audio synthesis, and UDP loopback multiplayer.

---

## Quick Start & Verification

### 1. Run Automated Safety & Integrity Unit Tests
```bash
g++ -O3 -std=c++17 run_tests.cpp -o run_tests.exe
./run_tests.exe
```
*Verifies `NaN`/`Inf` rejection, cross-cell Halo Insertion continuity, FNV-1a packet tamper detection, and analytical raycast precision.*

### 2. Compile a Spectral Continent & Export to `.OBJ`
```bash
pip install pillow
python world_compiler.py world_recipe.json
python continent_to_obj.py compiled_continent.acont continent_export.obj
```

### 3. Build the Interactive GPU Sandbox (Windows / MinGW)
```bash
g++ -O3 -std=c++17 main.cpp glad.c -Iinclude -Llib -lglfw3 -lopengl32 -lgdi32 -lws2_32 -lwinmm -o anatomical_sandbox.exe
./anatomical_sandbox.exe
```
*Controls: `WASD` Move | `Mouse` Look | `LMB` Carve Crater | `M` Toggle Surface/Caverns | `R` Hot-Reload `.acont` World*

---

## Repository Structure
* `AnatomicalCore.hpp` — Production-hardened C++17 spatial math, SDF, hash grid, physics, and Merkle router core.
* `run_tests.cpp` — Automated unit & integrity test suite.
* `main.cpp` — Interactive OpenGL 3.3 raymarching sandbox with `3×3` chunk streaming and UDP sync.
* `world_compiler.py` / `world_recipe.json` — 2D Fourier spectral terrain compiler.
* `continent_to_obj.py` — Wavefront `.OBJ` mesh exporter for Blender / UE5.
* `AnatomicalCAPI.cpp` / `ai_agent_bridge.py` — Native C-API and Python bridge for headless CPU raycasting.
* `AnatomicalUE5Component.h` / `AnatomicalUE5_Raymarch.hlsl` — Unreal Engine 5 integration adapter.
