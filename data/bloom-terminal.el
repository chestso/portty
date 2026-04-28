;;; bloom-terminal.el --- terminal initialization for bloom-terminal  -*- lexical-binding:t -*-

;;; Commentary:

;; Support for the bloom-terminal terminal emulator.

;;; Code:

(require 'term/xterm)

(defun terminal-init-bloom-terminal ()
  "Terminal initialization function for bloom-terminal."
  (let ((xterm-extra-capabilities '(modifyOtherKeys setSelection)))
    (tty-run-terminal-initialization (selected-frame) "xterm")))

(provide 'term/bloom-terminal)

;;; bloom-terminal.el ends here
