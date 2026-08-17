# Contributing to portty

This document covers how to build, test, and hack on portty. For user-facing documentation, see [README.md](README.md).

## Building from source

The project uses GNU Autotools. From a fresh checkout:

```bash
./autogen.sh
mkdir build && cd build
../configure --prefix=$HOME/.local
make -j$(nproc)
make check
make install
```

Use `--enable-release` for an optimized build or `--enable-debug` for unsanitized debug.

## Make targets

- `make` — build everything
- `make check` — run the test suite
- `make install` — install to `$prefix` (default `$HOME/.local`); compiles terminfo via `tic`; on Windows, also creates a Start Menu shortcut
- `make format` — clang-format on `src/` and `tests/`, shfmt on `scripts/`, black on `contrib/`, prettier on Markdown
- `make bear` — produce `compile_commands.json` for clangd
- `make regen-shaders` — recompile the GPU glyph-coverage fragment shader (requires glslangValidator)
- `make regen-icon` — regenerate the embedded window icon PNG header from the source SVG (requires rsvg-convert)
- `make dist-portable` — Windows-only: stage exe + DLLs + terminfo into a self-contained ZIP

## Helper scripts

| Script                           | Purpose                                                                          |
| -------------------------------- | -------------------------------------------------------------------------------- |
| `scripts/profile.sh`             | Build with `-pg`, run a benchmark, write `profile-report.txt`                    |
| `scripts/ref-png.sh T OUT`       | Generate reference PNG of TEXT using hb-view                                     |
| `scripts/ref-layers.sh T P`      | Export each COLR v1 paint layer as `<P>_layer00.png` etc.                        |
| `scripts/colr_layers.py`         | Export individual COLR v1 paint layers as PNG (Python fonttools + blackrenderer) |
| `scripts/pty_record.py`          | Record PTY traffic from a child process (debug feature-detect probes)            |
| `scripts/build-ucrt64.sh`        | Build natively on Windows (MSYS2 UCRT64)                                         |
| `scripts/make-dist-zip.sh`       | Assemble a portable Windows ZIP (called by `make dist-portable`)                 |
| `scripts/install-shortcut.ps1`   | Install a Start Menu shortcut for Portty on Windows (called by `make install`)   |
| `scripts/uninstall-shortcut.ps1` | Uninstall the Start Menu shortcut (called by `make uninstall`)                   |

## Architecture

portty uses a modular backend abstraction design:

- **PorttyBackend**: Unified platform + rendering backend — handles windowing, input events, clipboard, PTY lifecycle, the main event loop, and graphics output.
  - **SDL3** (`backend_sdl3.c`) — uses SDL3 for both windowing and GPU rendering. Draws the frame into an `RGBA64_FLOAT` / `SRGB_LINEAR` target via SDL's GPU renderer (Vulkan/D3D12/Metal), so glyph coverage is blended in linear light and re-encoded to sRGB on present. Uses libdecor for Wayland decorations. The SDL3 renderer (`rend_sdl3.c`) is called directly — no separate renderer vtable.
    - Texture atlas with shelf packing and FNV-1a hash-based lookup; LRU eviction when the atlas fills
    - Color-glyph (emoji) texels are sRGB→linear decoded as they enter the atlas: SDL linearizes draw/vertex colors on this path but not sampled texels, so without the decode the present-time re-encode would double-encode color emoji and wash them out. White text-coverage texels are gamma-invariant, so text is unaffected

- **Terminal Backend**: Handles terminal emulation and screen state
  - Current implementation: coffer (`terminal_backend_cfr`) — external VT engine consumed via `pkg-config coffer`, bridged through `term_cfr.c` (parser, page-based grid, scrollback ring, reflow, charsets). DEC ANSI parser (Williams state machine), UAX #11 + #29 cluster widths, page-arena style/grapheme interning, scrollback page ring

- **Font Backend**: Handles font loading, shaping, and glyph rasterization
  - Current implementation: FreeType/HarfBuzz (`font_backend_ft`)
  - Custom COLR v1 paint tree traversal implemented in `src/colr.c`
  - FreeType provides COLR v1 APIs for accessing paint data; recursive evaluation, affine transforms, and Porter-Duff compositing are implemented manually
  - Supports solid fills, linear/radial/sweep gradients, transforms, glyph masking, and basic composite modes
  - Some paint semantics (extend modes, all composite operators, transform edge-cases) are best-effort

- **Font Resolver Backend**: Handles font discovery and selection
  - Linux: Fontconfig (`font_resolve_backend_fc`)
  - macOS: Core Text (`font_resolve_backend_ct`) with `CTFontCreateForString` codepoint fallback
  - Windows: Native resolver (`font_resolve_backend_w32`) — GDI enumeration + DirectWrite path resolution for UWP/Store fonts + FreeType codepoint fallback

Each backend defines a standard interface (`PorttyBackend`, `TerminalBackend`, `FontBackend`, `FontResolveBackend`) with `*_init()`/`*_destroy()` lifecycle functions, allowing implementations to be swapped without changing the core application logic. Shared app logic lives in `portty_app.c`; shared rendering helpers (atlas packing, sRGB LUT, font loading) live in `rend_common.c`.

## Running tests

```bash
cd build && make check
```

| Test                      | What it covers                                                        |
| ------------------------- | --------------------------------------------------------------------- |
| `test_atlas`              | Texture atlas: insert/lookup, shelf packing, eviction                 |
| `test_unicode`            | Emoji detection, ZWJ, skin tones, UTF-8 decoding                      |
| `test_conf`               | Config parser: fonts, geometry, hinting, booleans                     |
| `test_term_cfr`           | Terminal backend bridging to coffer                                   |
| `test_osc52`              | OSC 52 clipboard set sequences                                        |
| `test_clipboard_deferred` | Wayland clipboard deferred-free invariant (use-after-free regression) |
| `test_altscreen_mouse`    | Altscreen/mouse-mode state and dispatch logic                         |
| `test_sixel`              | Sixel image end-to-end through host bridge                            |
| `test_lottie`             | Lottie animation bridge to coffer                                     |
| `test_paste_normalize`    | Clipboard line-ending normalization (CRLF→LF fix for Windows paste)   |
| `test_diag`               | Diagnostics report generation                                         |
| `test_rend_common`        | Shared rendering helpers (atlas packing, sRGB LUT)                    |
| `test_portty_app`         | Shared app logic and state management                                 |
| `test_timer`              | Timer wheel and callback scheduling                                   |
| `test_pty_pause`          | PTY pause/resume during selection (SDL3 + POSIX only)                 |
| `test_debug_script`       | Debug script parser (all command types, edge cases, escapes)          |
| `test_path_compat`        | MSYS2/Unix → Windows path conversion                                  |
| `test_boxdraw`            | Procedural box-drawing alignment                                      |
| `test_cursor_render`      | Cursor-only render regression                                         |
| `test_scroll`             | Mouse-wheel scroll logic: sub-tick accumulation                       |

The test suite runs 20 tests on SDL3 (POSIX) — `test_pty_pause` is SDL3+POSIX only.

Run individual tests with `-v` for verbose output, e.g. `./build/tests/test_atlas -v`.

## Code formatting

Run `make format` (from `build/`) to format all source files. This requires:

- **clang-format** — C source and headers (`src/`, `tests/`)
- **black** — Python files (`contrib/`)
- **shfmt** — shell scripts (`scripts/`)
- **prettier** — Markdown files

```bash
# Fedora 41+
sudo dnf install clang-tools-extra black shfmt
npm install --prefix ~/.local prettier
```

## Platform-specific build notes

### Windows native build (MSYS2/UCRT64)

portty builds natively on Windows using MSYS2 with the UCRT64 environment. The Windows build uses ConPTY for terminal emulation.

#### Prerequisites

Install [MSYS2](https://www.msys2.org/), then in a UCRT64 shell (`msys2_shell.cmd -ucrt64`):

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-sdl3 \
      mingw-w64-ucrt-x86_64-freetype mingw-w64-ucrt-x86_64-harfbuzz \
      mingw-w64-ucrt-x86_64-libpng mingw-w64-ucrt-x86_64-autotools

# Optional: only needed to regenerate the Windows .ico (see below)
pacman -S mingw-w64-ucrt-x86_64-imagemagick mingw-w64-ucrt-x86_64-librsvg
```

coffer is not in the official pacman repo — build and install it separately (`make install` in the coffer repo, or ensure `PKG_CONFIG_PATH` points at its build output).

#### Building

The easiest way is the helper script, which can be run from any shell (git-bash, cmd, PowerShell, or an MSYS2 shell). It re-execs into a real MSYS2 UCRT64 shell, applies the `sh` workaround (see below), and runs the full build. It assumes the prerequisites are already installed:

```bash
./scripts/build-ucrt64.sh            # autoreconf + configure + make + check
./scripts/build-ucrt64.sh --install  # build, then make install
./scripts/build-ucrt64.sh --enable-release  # extra args forwarded to configure
```

Or build manually from a UCRT64 shell:

```bash
./autogen.sh   # or: aclocal && autoheader && automake && autoconf
mkdir build && cd build
../configure
make -j$(nproc)
make check
```

The default build mode skips ASan/UBSan (not available on MinGW) and uses unsanitized debug flags automatically.

#### `autoreconf` fails on scoop-installed MSYS2 (sh workaround)

On some MSYS2 installs (notably scoop-managed), `/usr/bin/sh` and `/usr/bin/bash` are the same binary. When bash is invoked as `sh` — as `libtoolize` does via `#!/usr/bin/env sh` — its POSIX-mode `test -d` builtin intermittently fails to see `/ucrt64` paths (a mount-table visibility race), aborting `autoreconf` with:

```
libtoolize: error: $pkgauxdir is not a directory: '/ucrt64/share/libtool/build-aux'
```

`build-ucrt64.sh` applies the workaround automatically. To do it manually, shadow `sh` with a bash copy earlier on `PATH` before running `autoreconf`:

```bash
FIXSH=$(mktemp -d /tmp/portty-fixsh.XXXXXX)
cp /usr/bin/bash "$FIXSH/sh"
export PATH="$FIXSH:$PATH"
./autogen.sh          # now succeeds
rm -rf "$FIXSH"
```

#### Regenerating the Windows icon

The committed `data/icons/portty.ico` is a multi-resolution icon (256/128/64/48/32/16) generated from the source SVG via a 2-pass process: `rsvg-convert` renders each size individually, then `icotool` combines them. To regenerate after changing the SVG:

```bash
SVG=data/icons/hicolor/scalable/apps/portty.svg
OUTDIR=data/icons
for size in 16 32 48 64 128 256; do
    rsvg-convert -w $size -h $size -f png "$SVG" -o "$OUTDIR/portty-$size.png"
done
icotool -c -o "$OUTDIR/portty.ico" "$OUTDIR"/portty-{16,32,48,64,128,256}.png
rm "$OUTDIR"/portty-{16,32,48,64,128,256}.png
```

Requires `rsvg-convert` (librsvg) and `icotool` (icoutils).

To regenerate the embedded window icon (used by `SDL_SetWindowIcon`):

```bash
make -C build/src regen-icon    # needs rsvg-convert
```

## Submitting changes

- Open a pull request against `master`
- Ensure `make check` passes
- Run `make format` before committing
