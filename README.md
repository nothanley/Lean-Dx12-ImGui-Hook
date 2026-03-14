# Minimal DX12 Hook Project

This folder is a stripped standalone Visual Studio project for:
- DX12/DXGI hook core (`src/d3d12`, `src/dxgi`, `src/hook*`, `src/dll_*`)
- MinHook source (`third_party/minhook`)
- ImGui source (`third_party/imgui`)

## Build

1. Open `MinimalHook.sln` in Visual Studio 2022.
2. Select `Release | x64`.
3. Build `MinimalHook`.

Output:
- `minimal_hook/bin/Release/reshade.asi`

## Notes

- This project intentionally avoids the old ReShade solution structure and props/import chain.
- Only the minimal resource needed by the kept code is embedded (`IDR_MIPMAP_CS`).
