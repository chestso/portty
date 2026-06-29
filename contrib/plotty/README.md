# plotty

A Python TUI application that plays Lottie animations in [portty](https://github.com/nicm/portty),
using the bloom-vt Lottie APC protocol.

## Installation

```bash
pip install .
```

## Usage

```bash
plotty [options] <file.json>
```

### Options

| Flag | Description |
|------|-------------|
| `-l`, `--loop` | Loop playback (default) |
| `-L`, `--no-loop` | Play once and exit |
| `-s`, `--speed <rate>` | Playback speed multiplier (default: 1.0) |
| `--bg` | Render as background layer |
| `--opacity <val>` | Opacity 0.0–1.0 (default: 1.0) |

### Controls

| Key | Action |
|-----|--------|
| `space` | Pause / resume |
| `←`/`→` | Seek −5 / +5 frames |
| `+`/`−` | Speed up / slow down |
| `r` | Restart from frame 0 |
| `L` | Toggle loop |
| `b` | Toggle background/foreground layer |
| `[`/`]` | Decrease / increase opacity (10% steps) |
| `q`/`Esc` | Quit |
