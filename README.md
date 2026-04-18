# HLA Visualizer

Real-time 3D visualization of a distributed HLA aircraft simulation running in Unreal Engine 5.5.

An Airbus A320 flies a final approach into Madrid-Barajas (LEMD) over Cesium-streamed satellite terrain.
Position and radar data are published live by a C++ HLA federation running in WSL2; this Unreal project
subscribes to that data and drives the aircraft mesh and radar range circle on every frame.

**Related project (HLA simulator):** https://github.com/RaulDiMont/hla-simulator

---

## What it demonstrates

- Full HLA 1516e federation with four federates spanning two operating systems (WSL2 + Windows)
- Lock-free multithreading: HLA callbacks on a dedicated thread, Unreal Actor updates on the GameThread
- Cesium for Unreal georeferencing: WGS84 lat/lon/altitude → Unreal world coordinates
- Procedural radar range circle anchored to LEMD (10 km radius)
- Configurable RTI address exposed through Unreal Project Settings (no source edits needed)

---

## Requirements

### WSL2 — HLA simulator

This project is the visualization client only. The simulation runs in WSL2 as a separate project:

**https://github.com/RaulDiMont/hla-simulator**

Follow that repo's README to install dependencies (OpenRTI 0.10.0, JSBSim), build, and verify
that `rtinode` and `aircraft_simulator` run correctly before continuing here.

### Windows

| Requirement | Notes |
|---|---|
| Unreal Engine 5.5.4 | Install via Epic Games Launcher |
| Visual Studio 2022 | Required by Unreal Build Tool. Workloads: **Desktop development with C++** and **Game development with C++** |
| CMake 3.28+ | To build OpenRTI for Windows. Available via VS 2022 installer or [cmake.org](https://cmake.org/download/) |
| Cesium for Unreal plugin | Latest for UE 5.5 — Epic Launcher → Library → Vault → install to UE 5.5 |
| JetBrains Rider | Recommended IDE (optional — VS works too) |

### Windows — OpenRTI compiled binaries (not in git — must be built once locally)

The OpenRTI DLLs are gitignored because they are compiled binaries. Build them once with CMake + MSVC:

```bat
git clone https://github.com/onox/OpenRTI.git --branch OpenRTI-0.10.0
cd OpenRTI && mkdir build && cd build

cmake .. -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_INSTALL_PREFIX=C:/OpenRTI-0.10.0-win64 ^
    -DOPENRTI_BUILD_SHARED=ON ^
    -DCMAKE_CXX_STANDARD=17

cmake --build . --config Release --target install
```

> **`-DCMAKE_CXX_STANDARD=17` is mandatory.** Without it, CMake defaults to C++14 and
> `createRTIambassador()` is compiled returning `std::auto_ptr` instead of `std::unique_ptr`,
> causing an `LNK2019` linker error when Unreal (C++17) tries to link.

Then copy the outputs into the project:

```
C:/OpenRTI-0.10.0-win64/lib/  →  ThirdParty/OpenRTI/lib/Win64/   (rti1516e.lib, fedtime1516e.lib)
C:/OpenRTI-0.10.0-win64/bin/  →  ThirdParty/OpenRTI/bin/Win64/   (librti1516e.dll, libfedtime1516e.dll, OpenRTI.dll)
```

Final layout expected by `HLAVisualizer.Build.cs`:

```
ThirdParty/OpenRTI/
├── lib/Win64/
│   ├── rti1516e.lib
│   └── fedtime1516e.lib
└── bin/Win64/
    ├── librti1516e.dll
    ├── libfedtime1516e.dll
    └── OpenRTI.dll
```

### Cesium ion account

A free account at [cesium.com/ion](https://cesium.com/ion) is required to stream World Terrain.
Generate an access token, then paste it via **Window → Cesium** (the Cesium panel) inside the Unreal Editor.

> In Cesium for Unreal v2.x the token is set in the Cesium panel, **not** in Project Settings.

Each contributor needs their own token. `Content/CesiumSettings/` is gitignored to prevent tokens
from being committed accidentally.

---

## Running the demo

### 1 — Start the HLA federation in WSL2

Follow the hla-simulator README (two terminals). Once both processes are running, get the
WSL2 virtual IP — you will need it in step 3:

```bash
ip addr show eth0 | grep "inet "
# Example output: inet 172.26.53.127/20 ...
```

> **Windows Firewall:** if the Unreal client cannot reach `rtinode`, allow inbound TCP on
> port 14321 in Windows Defender Firewall → Inbound Rules → New Rule → Port → TCP 14321.

### 2 — Open the project in Unreal Editor

1. Open `HLAVisualizer.uproject` (right-click → Open with Unreal Engine 5.5)
2. If prompted to compile modules, click **Yes**
3. Wait for shaders to compile on first launch

### 3 — Set the RTI address

Go to **Edit → Project Settings → Plugins → HLA Visualizer** and update:

| Setting | Value |
|---|---|
| RTI Address | `rti://<WSL2_IP>:14321` (use the IP from step 1) |
| Federation Name | `AircraftSimulation` |

> The WSL2 IP changes on reboot. Update this setting each session.

### 4 — Press Play

Click **Play** in the Unreal Editor toolbar.

The UnrealFederate will attempt to join the HLA federation. It retries for up to 60 seconds,
so you can press Play before the simulator is fully started. Watch the Output Log for:

```
UnrealFederate: joined 'AircraftSimulation' at rti://172.x.x.x:14321
```

The A320 mesh will begin moving over Madrid terrain as JSBSim publishes position updates.
The radar range circle appears immediately at LEMD coordinates.

---

## Level setup (if starting from an empty level)

The main persistent level should contain:

| Actor | Details |
|---|---|
| `CesiumGeoreference` | Origin: Lat 40.4939 / Lon -3.5672 / Height 600 m (LEMD threshold) |
| Cesium World Terrain + Bing Maps Aerial | Added via the Cesium panel |
| `AUnrealFederateActor` | Assign an A320 Static Mesh asset to the **Aircraft Mesh** slot in Details |
| `ARadarVisualizationActor` | Assign a translucent material to **Ring Material** in Details |

---

## Project structure

```
Source/HLAVisualizer/
├── Types/                       FAircraftState, FRadarContact structs
├── UnrealFederate/              AUnrealFederateActor, FHLAFederateRunnable, FHLAAmbassador
├── Radar/                       ARadarVisualizationActor (procedural ring mesh)
└── Settings/                    UHLASettings (Project Settings integration)

ThirdParty/OpenRTI/
├── include/                     HLA 1516e headers (tracked in git)
├── lib/Win64/                   Import libraries (gitignored — build locally)
└── bin/Win64/                   Runtime DLLs (gitignored — build locally)
```

For architecture details, threading model, and design decisions see [PROJECT_SUMMARY.md](PROJECT_SUMMARY.md).

---

## Building from CLI

```
"C:\Program Files\Epic Games\UE_5.5\Engine\Build\BatchFiles\Build.bat" ^
    HLAVisualizerEditor Win64 Development ^
    "C:\Repos\hla_visualizer\HLAVisualizer\HLAVisualizer.uproject"
```

After adding new `.h`/`.cpp` files or editing `.Build.cs`, regenerate project files first:

```
"C:\Program Files\Epic Games\UE_5.5\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" ^
    -projectfiles ^
    -project="C:\Repos\hla_visualizer\HLAVisualizer\HLAVisualizer.uproject" ^
    -game -rocket -progress
```

---

## Known limitations

- The WSL2 virtual IP changes on every reboot — update Project Settings each session
- The radar circle is flat (not terrain-conforming) — deferred post-MVP
- No HLA Time Management — position updates arrive at wall-clock pacing
- UnrealFederate does not reconnect if the simulator is restarted while Unreal is in Play mode