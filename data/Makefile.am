iconscalabledir = $(datadir)/icons/hicolor/scalable/apps
iconscalable_DATA = icons/hicolor/scalable/apps/bloom-terminal.svg

iconsymbolicdir = $(datadir)/icons/hicolor/symbolic/apps
iconsymbolic_DATA = icons/hicolor/symbolic/apps/bloom-terminal-symbolic.svg

# PNG fallback for SDL_SetWindowIcon (SDL can't load SVG)
icon256dir = $(datadir)/icons/hicolor/256x256/apps
icon256_DATA = icons/hicolor/256x256/apps/bloom-terminal.png

desktopdir = $(datadir)/applications
desktop_DATA = applications/bloom-terminal.desktop

emacsdir = $(datadir)/emacs/site-lisp/term
emacs_DATA = bloom-terminal.el

EXTRA_DIST = bloom-terminal.ti bloom-terminal.el

if HOST_WINDOWS
iconwindowsdir = $(datadir)/icons
iconwindows_DATA = icons/bloom-terminal.ico

# --- gen-ico: regenerate the Windows .ico from the source SVG -----------------
#
# Renders data/icons/hicolor/scalable/apps/bloom-terminal.svg into a
# multi-resolution Windows icon (256/128/64/48/32/16) using ImageMagick.
# Requires the mingw-w64-ucrt-x86_64-imagemagick and librsvg packages
# (see README prerequisites).
ICON_SVG = $(srcdir)/icons/hicolor/scalable/apps/bloom-terminal.svg
ICO_OUTPUT = $(srcdir)/icons/bloom-terminal.ico
ICO_SIZES = 256,128,64,48,32,16

.PHONY: gen-ico
gen-ico:
	@echo "  GEN      $(ICO_OUTPUT)"
	@if ! command -v magick >/dev/null 2>&1; then \
	    echo "  ERROR: magick not found. Install the ImageMagick + librsvg packages:" >&2; \
	    echo "    pacman -S mingw-w64-ucrt-x86_64-imagemagick mingw-w64-ucrt-x86_64-librsvg" >&2; \
	    exit 1; \
	fi
	@MAGICK=`command -v magick 2>/dev/null || echo /ucrt64/bin/magick`; \
	    "$$MAGICK" -background none -density 256 "$(ICON_SVG)" \
	    -define icon:auto-resize=$(ICO_SIZES) "$(ICO_OUTPUT)" \
	    && echo "  OK       $(ICO_OUTPUT) ($(ICO_SIZES))" \
	    || { echo "  ERROR: magick failed to render ICO from $(ICON_SVG)" >&2; exit 1; }
endif HOST_WINDOWS

if !HOST_WINDOWS
install-data-local:
	tic -x -o $(DESTDIR)$(datadir)/terminfo $(srcdir)/bloom-terminal.ti

uninstall-local:
	rm -f $(DESTDIR)$(datadir)/terminfo/b/bloom-terminal-256color
	rm -f $(DESTDIR)$(datadir)/terminfo/b/bloom-terminal-vty-256color
	rm -f $(DESTDIR)$(datadir)/terminfo/b/bloom-terminal
endif

if HOST_WINDOWS
install-data-local:
	tic -x -o $(DESTDIR)$(datadir)/terminfo $(srcdir)/bloom-terminal.ti \
	    || echo "  WARNING: Failed to install terminfo (is ncurses installed?)" >&2
	@powershell -NoProfile -ExecutionPolicy Bypass \
	    -File "$(top_srcdir)/scripts/install-shortcut.ps1" \
	    -ExePath "$(DESTDIR)$(bindir)/bloom-terminal.exe" \
	    -IconPath "$(DESTDIR)$(datadir)/icons/bloom-terminal.ico" \
	    || echo "  WARNING: Failed to create Start Menu shortcut" >&2

uninstall-local:
	rm -f $(DESTDIR)$(datadir)/terminfo/b/bloom-terminal-256color
	rm -f $(DESTDIR)$(datadir)/terminfo/b/bloom-terminal-vty-256color
	rm -f $(DESTDIR)$(datadir)/terminfo/b/bloom-terminal
	@powershell -NoProfile -ExecutionPolicy Bypass \
	    -File "$(top_srcdir)/scripts/uninstall-shortcut.ps1" \
	    || echo "  WARNING: Failed to remove Start Menu shortcut" >&2
endif HOST_WINDOWS
