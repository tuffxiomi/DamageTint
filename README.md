# Damage Tint

Standalone Levi/Preloader Android visual mod. When the local player is hurt, a smooth red screen tint appears.

## Files

Only 4 uploadable files are needed:
- `src/DamageTint.cpp`
- `xmake.lua`
- `.github/workflows/build.yml`
- `README.md`

No BedrockTools files are included.

## Build

GitHub Actions builds ARM64 (`arm64-v8a`) and produces both `libDamageTint.so` and `DamageTint.levipack`.

The implementation uses the inspected BedrockTools ARM64 `ClientInstanceUpdate` signature, `ClientInstance` vtable index `32`, and `Actor::mHurtTime` offset `0x194` from the supplied game binary. These values are build-specific and may need updating after a Minecraft update.
