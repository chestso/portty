;;; portty.el --- terminal initialization for portty  -*- lexical-binding:t -*-

;;; Commentary:

;; Support for the portty terminal emulator.

;;; Code:

(require 'term/xterm)

(defun terminal-init-portty ()
  "Terminal initialization function for portty."
  (let ((xterm-extra-capabilities '(modifyOtherKeys setSelection)))
    (tty-run-terminal-initialization (selected-frame) "xterm"))
  ;; Enable xterm mouse tracking. Emacs 31+ auto-enables xterm-mouse-mode
  ;; for a hardcoded allowlist of terminal names (Konsole, VTE, WezTerm,
  ;; iTerm2, kitty, foot) after probing XTVERSION, but portty isn't in that
  ;; list and Emacs 30 has no auto-enable at all. Since portty is the
  ;; terminal here, turn it on so the mouse works in `emacs -nw`.
  ;;
  ;; Emacs 31 tracks whether the user already toggled the mode with
  ;; `xterm-mouse-mode-called'; respect that so an explicit
  ;; `(xterm-mouse-mode -1)' in their init file still wins. On Emacs 30
  ;; that variable does not exist, so the mode is enabled unconditionally;
  ;; Emacs 30 users who want to opt out can add
  ;; `(add-hook 'tty-setup-hook (lambda () (xterm-mouse-mode -1)))'.
  (unless (and (boundp 'xterm-mouse-mode-called)
               xterm-mouse-mode-called)
    (xterm-mouse-mode 1))
  ;; Fix emoji / VS16 (U+FE0F) width mismatch. Emacs's char-width-table
  ;; treats U+FE0F as width 0 and most dual text/emoji base characters
  ;; (e.g. U+2328) as width 1, but portty renders base+VS16 as a 2-column
  ;; wide glyph. Without this fix, Emacs's cursor position drifts from
  ;; the terminal's and the display garbles on movement/redisplay.
  ;;
  ;; Emacs <= 31: no built-in fix. Tell Emacs U+FE0F occupies a column
  ;; (base 1 + VS16 1 = 2 = portty) and disable `auto-composition-mode'
  ;; so ZWJ emoji sequences don't composite into a single cell the
  ;; terminal may render wider.
  ;;
  ;; Emacs >= 32: `tty-display-emoji-force-wide' (default t) handles this
  ;; natively via `produce_composite_glyph', so no action is needed — and
  ;; `auto-composition-mode' must stay ON for that to work.
  ;;
  ;; This file only loads via `tty-run-terminal-initialization', so we're
  ;; already in a terminal frame; the `display-graphic-p' guard is
  ;; self-documenting belt-and-suspenders.
  (when (and (not (display-graphic-p))
             (< emacs-major-version 32))
    (set-char-table-range char-width-table #xFE0F 1)
    (setq-default auto-composition-mode nil)))

(provide 'term/portty)

;;; portty.el ends here
