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
    (xterm-mouse-mode 1)))

(provide 'term/portty)

;;; portty.el ends here
