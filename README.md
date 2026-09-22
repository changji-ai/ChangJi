# The changji engine

The changji engine. **One binary**: interface, HTTP API, orchestration and
inference all in a single process.

This repository holds the engine and the pipelines that package it. The design
document, the web UI source, the desktop shell and the brand assets live in the
private product repository and **are not here**.

During the migration it ran alongside a Python engine, kept in step by
**response-by-response comparison**. After stage 8 the Python side and the
comparison tooling were deleted; the safety net left behind is the JSON corpus
under `tests/golden/`, exported from the real Python functions at the time and
now frozen in version control, which the unit tests read directly. So "did the
C++ break" is still answerable. "Is the Python still like this" is not — and no
longer means anything.

## Installing

```bash
curl -fsSL https://raw.githubusercontent.com/changji-ai/ChangJi/main/install.sh | bash
```

Or take a package from [Releases](https://github.com/changji-ai/ChangJi/releases)
directly. Windows / macOS / Linux, x64 and arm64, named like
`changji-linux-arm64.tar.gz`. The macOS binaries are signed with a Developer ID
and notarised.

## Building

Needs CMake ≥ 3.20 and a C++17 compiler. On Linux and macOS, cmake, ninja and
git are enough.

**`CHANGJI_LLAMA` also needs a Python interpreter** — llama.cpp is patched in
place with leejet's extensions after it is fetched, and configure stops dead if
it cannot find one. That is a build-time dependency only.

**`CHANGJI_SSL` (ON by default) needs OpenSSL ≥ 3.0, and only accepts static
libraries.** httplib uses it for https. Refusing shared libraries has a history:
a package that linked the build machine's `libssl.3.dylib` ran perfectly on the
build machine, and on somebody else's it was
`dyld: Library not loaded: /opt/homebrew/opt/openssl@3/...`.

- Linux: `apt install libssl-dev` (it carries `libssl.a`)
- macOS: `brew install openssl@3`, then
  `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)`
- If no static library is found it **stops there**; it never quietly falls back
  to dynamic linking
- Just want a fast local build with no https: `-DCHANGJI_SSL=OFF`

The released packages depend on nothing installed on any particular machine: the
`openssl` job in `.github/workflows/openssl.yml` builds a pinned static copy
itself (cached) and points `-DOPENSSL_ROOT_DIR` at it. In the same
CMakeLists, brotli and zlib are fetched and built statically, so there is
nothing to install for those either.

Windows needs **MSVC 2022 Build Tools + Ninja**. Enter the MSVC environment
first:

```powershell
cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && set' |
  ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path ("env:" + $matches[1]) -Value $matches[2] } }
```

> Skipping that step shows up as `fatal error C1083: cannot open include file:
> "algorithm"`. That is not a code problem; `INCLUDE` is simply not set.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Build options

| Option | Default | What it does |
|---|---|---|
| `CHANGJI_SD` | ON | Link stable-diffusion.cpp; images and video in-process |
| `CHANGJI_SD_CUDA` | OFF | CUDA GPU backend (NVIDIA). Needs the CUDA Toolkit, and MSBuild looks for the **versioned** `CUDA_PATH_V13_2` rather than `CUDA_PATH` — both have to be in the process environment |
| `CHANGJI_SD_HIP` | OFF | HIP / ROCm GPU backend (AMD). Needs ROCm ≥ 6.1 (Linux) or the AMD HIP SDK (Windows). The machine that runs it needs the ROCm runtime too |
| `CHANGJI_SD_SYCL` | OFF | SYCL GPU backend (Intel). Needs oneAPI's `icx`/`icpx`. **The result is not a single file**; the machine that runs it needs the oneAPI runtime |
| `CHANGJI_SD_VULKAN` | OFF | Vulkan GPU backend, **works on NVIDIA, AMD and Intel alike**. Building needs the Vulkan SDK (`glslc`); running needs only the Vulkan runtime that ships with the driver. This is the road to "download, unpack, run" on AMD and Intel |
| `CHANGJI_CUDA_ARCH` | `89` | Which NVIDIA architectures to build for, semicolon separated. The list the release packages use is in release.yml |
| `CHANGJI_CUDA_STATIC` | OFF | Link the CUDA runtime (cudart / cuBLAS / cuBLASLt) into the binary, so **the result is a single executable**. This is the shape the release packages take; CI's linux-x64-cuda cell has it on. ⚠️ **Only possible on Linux**: NVIDIA does not ship a static cuBLAS for Windows, so that package is necessarily an exe plus two DLLs |
| `CHANGJI_HIP_ARCH` | `gfx1030;gfx1100;gfx1101;gfx1102` | Which AMD architectures to build for. **A card outside the list simply will not run** — HIP has no PTX-style fallback the way CUDA does |
| `CHANGJI_LLAMA` | OFF | Link llama.cpp + mtmd for **in-process speech** (stage 9). Turning it on adds a llama.cpp and a patched ggml to a clean build |
| `CHANGJI_SSL` | ON | Statically link OpenSSL, without which httplib cannot speak https. **Static libraries only**; see above |
| `CHANGJI_BUILD_TESTS` | ON | Build `changji_tests` |
| `CHANGJI_STATIC_RUNTIME` | ON | Statically link the runtime. "No runtime dependencies" is half the reason this backend exists. **Ignored on macOS** — Apple's toolchain has no static libc++ |
| `CHANGJI_DESKTOP` | OFF | Build the Qt desktop shell. Its sources are in the product repository, so this needs both checked out |
| `CHANGJI_VERSION` | `dev` | The version baked into the binary, printed by `--version`. CI fills it in from the git tag |

The four GPU backends are **mutually exclusive**; turning on two stops
configuration. Which to pick:

| Card | First choice | Alternative |
|---|---|---|
| NVIDIA | `CHANGJI_SD_CUDA` | `CHANGJI_SD_VULKAN` |
| AMD | `CHANGJI_SD_VULKAN` (runs as-is) | `CHANGJI_SD_HIP` (faster, but ROCm has to be installed) |
| Intel | `CHANGJI_SD_VULKAN` | `CHANGJI_SD_SYCL` (needs the oneAPI runtime) |

> Vulkan is the first choice for AMD and Intel not because it is fast, but
> because the other two produce artifacts dragging a pile of runtime
> dependencies behind them — and because leejet's ggml extension patches
> **do not cover ggml-sycl** (they cover CPU / CUDA / Metal / Vulkan; see
> [patches/README.md](patches/README.md)), so fp8 weights most likely will not
> load under SYCL.

With `CHANGJI_LLAMA` on, ggml comes from llama.cpp and is **patched in place
with leejet's extensions** (`patches/apply_to_llamacpp.py`, hooked onto
FetchContent's `PATCH_COMMAND`, idempotent). sd.cpp depends on those extensions
outright — FP8 types, int8 convrot, i8 tensorwise matmul — with no `#ifdef`
guarding the call sites. The whole story is in
[verify/RESULTS.md](verify/RESULTS.md) and [patches/README.md](patches/README.md).

Two build directories side by side is normal: `build/` (default) and
`build_llama/` (`-DCHANGJI_LLAMA=ON`).

## Running

```bash
./build/changji --version      # which build this is
./build/changji --doctor       # command-line health check; a non-zero exit means something must be fixed
./build/changji --init-config  # write an annotated configuration template
./build/changji --port 8080    # start the server; the web UI is inside the binary, just open a browser
```

In-process speech (needs a binary built with `CHANGJI_LLAMA=ON`):

```bash
# The weights are not in the repository. Open the interface and pick them on
# the first-run setup page, which downloads them (about 1.34 GB), or point at
# your own files with the two flags below.
./build_llama/changji --say "He did not look back, there on the rooftop in the rain."
./build_llama/changji --say "a test line" --tts-model talker.gguf --tts-decoder tok.gguf
```

`--say` is stage 9's on-the-metal test: **no server, no project, no ffmpeg** —
one line answers whether sound comes out.

Configuration precedence: environment variables > the project directory's
`changji.toml` > the user's global configuration > built-in defaults.

Common environment variables (all prefixed `CHANGJI_`): `COMFY_BASE_URL`,
`LLM_BASE_URL`, `LLM_MODEL`, `LLM_API_KEY`, `WORKSPACE`, `FFMPEG_PATH`,
`VRAM_GB`, plus the C++-only `MODELS_DIR` / `MODELS_ENGINE` / `MODELS_VIDEO` /
`MODELS_TTS` / `MODELS_TTS_DECODER` and others. Adding one means writing it
**in two places** (the `env_mapping()` table and `apply_env()`); missing one is
not an error, and `test_models_config.cpp` has a test whose whole job is to
catch that.

## Testing

```bash
./build/changji_tests            # the unit tests
ctest --test-dir build           # the same thing, through ctest
```

**A corpus that cannot be read counts as a failure, not a skip.** A skipped test
and a passing test look identical in the total, but the first guarantees
nothing — point `CHANGJI_GOLDEN_DIR` somewhere wrong and every corpus-reading
test silently verifies nothing at all.

There is also a `verify_all.ps1` in the product repository that runs these
together with the web UI's own tests and both build configurations. It cannot
live here: the web UI is not in this repository.

## Packaging

`.github/workflows/` holds the whole release line, and it is the only copy —
the product repository has none.

| | |
|---|---|
| `release.yml` | The engine: six CPU platforms, four GPU cells, macOS signing and notarisation, publish |
| `desktop.yml` | The desktop app: three platforms, packaged, signed, notarised, and **actually opened** before publishing |
| `webapp.yml` | Build the web UI and bake it into a header (`workflow_call`; both lines use it) |
| `openssl.yml` | The pinned static OpenSSL (`workflow_call`; both lines use it) |
| `cloud-tool.yml` | The GPU-rental tool, which is entirely inside this repository |
| `install.sh` | The one-line installer, which downloads from this repository's Releases |

Every pipeline except `cloud-tool.yml` checks out the private product
repository as `changji/` and lays this repository over `changji/cpp`, because
the web UI, the desktop shell and the brand assets live there. That needs a
`CHANGJI_PRODUCT_TOKEN` secret. `tools/setup_signing_secrets.sh` sets it
together with the five macOS signing secrets, verifying each one locally before
it uploads anything.

## Layout

```
src/
├── main.cpp        the command line (--doctor / --init-config / --say / serve)
├── util/           paths, subprocesses, text, time, East Asian widths
├── config/         configuration structs, TOML, environment overrides, validation
├── models/         project / character / shot, and reading and writing the project library
├── http/           Crow routes and the API (read, edit, upload, media, LLM, run)
├── llm/            an OpenAI-compatible client
├── agent/          the agent loop and the tools it can call
├── lan/            finding other changji machines on the LAN (mDNS)
├── stages/         screenplay, bible, storyboard, prompts, first frame, render, speech
├── infer/          the sd.cpp façade, VRAM scheduling, the ggml ABI probe, in-process speech
├── net/            the protocol half of a WebSocket client (for multi-machine work; no caller yet)
├── setup/          the first-run page: model catalogue, download sources, downloader
├── media/          ffmpeg, subtitles, assembly
├── gates/          quality gates
├── pipeline/       the job table and the whole-chapter pipeline
└── doctor/         the environment health check

tests/
├── unit/           unit tests, reading the corpus in tests/golden/
└── golden/         the golden corpus, exported from the Python side back then and **frozen in version control**

prompts.toml        **every prompt** (for the LLM and for image generation). Generated into the binary at build time; edit prompts here
tools/              code generation (prompts, web UI, East Asian widths), a fake LLM, and the GPU-rental tool
patches/            the leejet/ggml extension patches and the script that applies them
verify/             the up-front verification project (one-off; conclusions in RESULTS.md)
i18n/               translation tables for eleven languages, baked into the binary
```

## Where this stands

Stages 0–9 are done. In-process speech has produced real audio, an in-process
LLM has run on a 5090, and VRAM eviction has been exercised. **Stage 8 has been
carried out**: the Python engine, the comparison tooling and the 27 scripts that
imported the engine are all deleted. What is left is this one binary plus an
optional Node BFF.

| Not yet proven | |
|---|---|
| Real images, a real finished film | The assembly half has been verified with placeholder frames. These two are exactly the hardest part of the rewrite, and the part that no longer has anything to compare against |
| macOS / arm64 | CI builds it, but no full chapter has run on real hardware |

The contract-compatibility standard was: **field names, nesting, values and
status codes identical; key order irrelevant**; **prompt concatenation identical
byte for byte**; the **wording** of a validation error is not part of the
contract. Those standards now hold only against the frozen corpus in
`tests/golden/`.
