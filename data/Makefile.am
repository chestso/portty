iconscalabledir = $(datadir)/icons/hicolor/scalable/apps
iconscalable_DATA = icons/hicolor/scalable/apps/portty.svg

iconsymbolicdir = $(datadir)/icons/hicolor/symbolic/apps
iconsymbolic_DATA = icons/hicolor/symbolic/apps/portty-symbolic.svg

desktopdir = $(datadir)/applications
desktop_DATA = applications/portty.desktop

emacsdir = $(datadir)/emacs/site-lisp/term
emacs_DATA = portty.el

EXTRA_DIST = portty.ti portty.el \
    icons/hicolor/scalable/apps/portty.svg \
    icons/hicolor/symbolic/apps/portty-symbolic.svg \
    icons/portty.ico \
    applications/portty.desktop \
    conpty/OpenConsole.exe conpty/conpty.dll

if HOST_WINDOWS
conptydir = $(bindir)
conpty_DATA = conpty/OpenConsole.exe conpty/conpty.dll
endif

if !HOST_WINDOWS
# tic's use= lookup searches $TERMINFO, $TERMINFO_DIRS, ~/.terminfo and the
# system dirs — but NOT $datadir/terminfo for a custom prefix. portty.ti
# references coffer's entries via use=, so point TERMINFO at the install
# destination, where coffer's terminfo already lives (the monorepo installs
# coffer first; a standalone portty install requires a prior coffer install
# anyway).
install-data-local:
	TERMINFO="$(DESTDIR)$(datadir)/terminfo" \
	    tic -x -o $(DESTDIR)$(datadir)/terminfo $(srcdir)/portty.ti

uninstall-local:
	rm -f $(DESTDIR)$(datadir)/terminfo/p/portty-256color
	rm -f $(DESTDIR)$(datadir)/terminfo/p/portty-vty-256color
	rm -f $(DESTDIR)$(datadir)/terminfo/p/portty
endif

if HOST_WINDOWS
install-data-local:
	TERMINFO="$(DESTDIR)$(datadir)/terminfo" \
	    tic -x -o $(DESTDIR)$(datadir)/terminfo $(srcdir)/portty.ti \
	    || echo "  WARNING: Failed to install terminfo (is ncurses installed?)" >&2
	@powershell -NoProfile -ExecutionPolicy Bypass \
	    -File "$(top_srcdir)/scripts/install-shortcut.ps1" \
	    -ExePath "$(DESTDIR)$(bindir)/portty.exe" \
	    || echo "  WARNING: Failed to create Start Menu shortcut" >&2

uninstall-local:
	rm -f $(DESTDIR)$(datadir)/terminfo/p/portty-256color
	rm -f $(DESTDIR)$(datadir)/terminfo/p/portty-vty-256color
	rm -f $(DESTDIR)$(datadir)/terminfo/p/portty
	@powershell -NoProfile -ExecutionPolicy Bypass \
	    -File "$(top_srcdir)/scripts/uninstall-shortcut.ps1" \
	    || echo "  WARNING: Failed to remove Start Menu shortcut" >&2
endif HOST_WINDOWS
