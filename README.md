# RaidSimulator
No Grinds, Just Raids

The RaidSimulator is a game focus on Raids like in MMO RPGs.

The Dungeons are based on Blocks of 60 cm size. These blocks are streamed from the server in chunks.

## Client Status
- Vulkan client initialized via VoxelEngine
- REST chunk fetch (`/chunk?x=&y=&z=`) from the Ktor server
- FPS overlay rendered in the top-right corner (SMLUI + ImGui)

## Build (CMake)
From repo root:
```sh
cmake -S . -B build
cmake --build build
```

## Run
```sh
./build/RaidSimulator/RaidSimulator
```