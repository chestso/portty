;;; bloom-terminal.el --- terminal initialization for bloom-terminal  -*- lexical-binding:t -*-

;;; Commentary:

;; Support for the bloom-terminal terminal emulator.

;;; Code:

(require 'term/xterm)

(defun terminal-init-bloom-terminal ()
  "Terminal initialization function for bloom-terminal."
  (let ((xterm-extra-capabilities '(modifyOtherKeys setSelection)))
    (tty-run-terminal-initialization (selected-frame) "xterm"))
  ;; Fix base ANSI colors: in 24-bit mode, xterm init registers them
  ;; with packed-RGB indices via tty-color-24bit.  Some packed values
  ;; (e.g. blue=238) collide with the 256-color palette range, causing
  ;; setaf to emit wrong SGR sequences.  Re-register the first 16
  ;; colors with their ANSI indices by temporarily suppressing
  ;; tty-color-24bit.
  (when (= (display-color-cells) 16777216)
    (cl-letf (((symbol-function 'tty-color-24bit) #'ignore))
      (dolist (entry xterm-standard-colors)
        (let ((name (nth 0 entry))
              (index (nth 1 entry))
              (rgb (nth 2 entry)))
          (when (< index 16)
            (tty-color-define name index
                              (mapcar #'xterm-rgb-convert-to-16bit rgb))))))))

(provide 'term/bloom-terminal)

;;; bloom-terminal.el ends here
