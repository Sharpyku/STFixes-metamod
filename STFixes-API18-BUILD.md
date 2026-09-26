# STFixes Metamod API 18 port

This branch ports STFixes-metamod to Metamod:Source 2.0 plugin API 18.

It preserves the upstream GPLv3 license and attribution. The port replaces
the removed SourceHook hook declarations with KHook, builds against a current
CS2 SDK, and removes obsolete string-token imports from the old SDK.

## Reproducible build

The port was validated against the CS2 Linux `libserver.so` build used on
2026-09-26 and Metamod:Source build `05c5c63`. Build with the Steam Runtime
Sniper SDK and Clang 16. Do not load the resulting binary on a different CS2
build without rechecking its gamedata and ABI.

```bash
git clone --recurse-submodules https://github.com/SudoGool/STFixes-metamod.git
cd STFixes-metamod
git checkout metamod-api18
git -C sdk fetch origin dadd5d70170fa94cf360ed3cbfffa511dfca87b4
git -C sdk checkout --detach dadd5d70170fa94cf360ed3cbfffa511dfca87b4
```

The original upstream project is [SharpTimer/STFixes-metamod](https://github.com/SharpTimer/STFixes-metamod).
