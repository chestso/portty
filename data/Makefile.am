iconscalabledir = $(datadir)/icons/hicolor/scalable/apps
iconscalable_DATA = icons/hicolor/scalable/apps/portty.svg

iconsymbolicdir = $(datadir)/icons/hicolor/symbolic/apps
iconsymbolic_DATA = icons/hicolor/symbolic/apps/portty-symbolic.svg

desktopdir = $(datadir)/applications
desktop_DATA = applications/portty.desktop

emacsdir = $(datadir)/emacs/site-lisp/term
emacs_DATA = portty.el

EXTRA_DIST = portty.ti portty.el

if !HOST_WINDOWS
install-data-local:
	tic -x -o $(DESTDIR)$(datadir)/terminfo $(srcdir)/portty.ti

uninstall-local:
	rm -f $(DESTDIR)$(datadir)/terminfo/p/portty-256color
	rm -f $(DESTDIR)$(datadir)/terminfo/p/portty-vty-256color
	rm -f $(DESTDIR)$(datadir)/terminfo/p/portty
endif

if HOST_WINDOWS
install-data-local:
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
