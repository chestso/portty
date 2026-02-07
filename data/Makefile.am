iconscalabledir = $(datadir)/icons/hicolor/scalable/apps
iconscalable_DATA = icons/hicolor/scalable/apps/bloom-terminal.svg

iconsymbolicdir = $(datadir)/icons/hicolor/symbolic/apps
iconsymbolic_DATA = icons/hicolor/symbolic/apps/bloom-terminal-symbolic.svg

# PNG fallback for SDL_SetWindowIcon (SDL can't load SVG)
icon256dir = $(datadir)/icons/hicolor/256x256/apps
icon256_DATA = icons/hicolor/256x256/apps/bloom-terminal.png

desktopdir = $(datadir)/applications
desktop_DATA = applications/bloom-terminal.desktop

EXTRA_DIST = bloom-terminal.ti

install-data-local:
	tic -x -o $(DESTDIR)$(datadir)/terminfo $(srcdir)/bloom-terminal.ti

uninstall-local:
	rm -f $(DESTDIR)$(datadir)/terminfo/b/bloom-terminal
