# Known Issues

## Cursor Position Offset After Resize (Line-Based Shells)

### Symptoms

When using line-based shells (bash, zsh, fish) in bloom-terminal:

1. Resize the window narrower so the shell prompt wraps to multiple lines
2. Resize the window wider so the prompt would fit on one line
3. The cursor appears in the wrong position (often in the middle of the prompt)
4. Typed characters appear at the cursor position but may be overwritten by the prompt

### Cause

This is a fundamental incompatibility between **libvterm with reflow disabled** and **bash/readline**:

- **libvterm (reflow disabled)**: When the terminal is resized, wrapped text stays at the same row/column positions. The terminal content is not reorganized to fit the new width.

- **bash/readline**: Assumes the terminal reflowed content during resize. It recalculates cursor position based on how the prompt _would_ wrap at the new terminal width, not how it _actually_ appears on screen.

The result is readline sending cursor positioning commands that put the cursor in the wrong place.

### Why Reflow is Disabled

Reflow is disabled by default due to a [libvterm bug](https://github.com/neovim/neovim/issues/25234) that can cause crashes during extreme window resizes. This affects Neovim and other libvterm-based applications.

### Affected Use Cases

| Use Case                            | Status                                         |
| ----------------------------------- | ---------------------------------------------- |
| Full-screen apps (vim, htop, less)  | Works correctly - they manage their own screen |
| Line-based shells (bash, zsh, fish) | Cursor glitches after resize                   |
| Running commands with output        | Works correctly                                |

### Workarounds

1. **Type `reset`** after resize to reset the terminal state

2. **Press Ctrl+L** to clear and redraw (may not fully fix the issue)

3. **Enable reflow** with the `--reflow` flag (at risk of crashes on extreme resize):

   ```bash
   ./bloom-terminal --reflow
   ```

4. **Avoid resizing** while at a shell prompt - resize while a full-screen app is running or before starting the shell
