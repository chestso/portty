;;; portty.el --- terminal initialization for portty  -*- lexical-binding:t -*-

;;; Commentary:

;; Support for the portty terminal emulator.

;;; Code:

(require 'term/xterm)

(defun terminal-init-portty ()
  "Terminal initialization function for portty."
  (let ((xterm-extra-capabilities '(modifyOtherKeys setSelection)))
    (tty-run-terminal-initialization (selected-frame) "xterm")))

(provide 'term/portty)

;;; portty.el ends here
