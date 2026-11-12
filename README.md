# CodmExternal

External overlay APK for **Call of Duty Mobile** (`com.garena.game.codm`). Reads game state from the target process via `process_vm_readv` / `process_vm_writev` and draws ESP with ImGui on a system overlay.

## Offsets

Hardcoded **libunity.so** RVAs and field offsets from **jnmaini** dump **1.6.59** (`codm_offsets.hpp`). No runtime IL2CPP/metadata resolve for gameplay data.

**ANOGS:** Four `libanogs.so` patch sites (same RVAs as jnmaini `ImGui/hooks.h`) are written automatically when `libanogs.so` is mapped — `mov x0,#0; ret` via external write. Vendored `vendor/jnmaini_ANOGS/` is reference-only (do not modify for external logic).

## Build

```bash
./gradlew assembleRelease
```

APK: `app/build/outputs/apk/release/app-release.apk` (arm64-v8a).

## Usage

1. Install APK, grant **display over other apps** (+ notifications on API 33+).
2. Tap **Launch overlay**, then open CODM.
3. Floating icon → menu → **Find game** / enable **ESP**.
4. On successful ANOGS patch you get a **Success hook** toast and a log line.

## Layout

| Path | Role |
|------|------|
| `codm_proc.*` | PID scan, module bases |
| `codm_anogs.*` | Auto patch |
| `codm_remote.*` | Remote IL2CPP calls + W2S |
| `codm_engine.*` | Match / enemy tick |
| `codm_draw.*` | ImGui ESP |
| `ui_core.*` | Overlay UI + reader thread |
