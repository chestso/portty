SUBDIRS = src data tests

ACLOCAL_AMFLAGS = -I m4

dist_doc_DATA = README.md

EXTRA_DIST = \
    autogen.sh \
    build-aux/git-version.sh \
    build-aux/add-header.sh \
    build-aux/bin2c.sh \
    build-aux/descriptions.tsv \
    version \
    COPYING \
    README.md \
    BUGS.md \
    CONTRIBUTING.md \
    TODO.md \
    .clang-format \
    scripts \
    examples \
    portty-scripts \
    docs \
    contrib/plotty \
    contrib/demo-lottie.json \
    third_party/qoi/qoi.h

# The ConPTY bundle is fetched, not committed. Release tarballs carry all
# three architectures so a downstream Windows build needs no network; the
# verifier checks the PE machine types and the checksums before packaging.
dist-hook:
	"$(top_srcdir)/scripts/fetch_conpty.sh" --arch all
	mkdir -p "$(distdir)/data/conpty"
	for a in x64 arm64 x86; do \
	    mkdir -p "$(distdir)/data/conpty/$$a"; \
	    cp "$(top_srcdir)/data/conpty/$$a/conpty.dll" \
	       "$(top_srcdir)/data/conpty/$$a/OpenConsole.exe" \
	       "$(top_srcdir)/data/conpty/$$a/SHA256SUMS" \
	       "$(distdir)/data/conpty/$$a/"; \
	done
	cp "$(top_srcdir)/data/conpty/VERSION" "$(distdir)/data/conpty/"
	"$(top_srcdir)/scripts/verify_conpty_bundle.py" --arch all

# Format target — runs clang-format on C/C++, shfmt on shell, black on Python, dprint on Markdown.
.PHONY: format
format:
	@if command -v clang-format >/dev/null 2>&1; then \
	    echo "Formatting C/C++ with clang-format..."; \
	    find $(top_srcdir)/src $(top_srcdir)/tests \
	        \( -name '*.c' -o -name '*.h' \) -print0 \
	        | xargs -0 clang-format -i; \
	else \
	    echo "clang-format not found - skipping C/C++ formatting." >&2; \
	fi
	@if command -v shfmt >/dev/null 2>&1; then \
	    echo "Formatting shell scripts with shfmt..."; \
	    find $(top_srcdir)/scripts \
	        -name '*.sh' -print0 2>/dev/null \
	        | xargs -0 -r shfmt -w; \
	else \
	    echo "shfmt not found - skipping shell script formatting." >&2; \
	fi
	@if command -v black >/dev/null 2>&1; then \
	    echo "Formatting Python with black..."; \
	    find $(top_srcdir)/contrib \
	        \( -name '*.py' \) -not -path '*/.*' -print0 \
	        | xargs -0 -r black --quiet; \
	else \
	    echo "black not found - skipping Python formatting." >&2; \
	fi
	@if command -v dprint >/dev/null 2>&1; then \
	    echo "Formatting Markdown with dprint..."; \
	    find $(top_srcdir) -maxdepth 2 -name '*.md' -not -path '*/node_modules/*' -print0 \
	        | xargs -0 -r dprint fmt; \
	else \
	    echo "dprint not found - skipping Markdown formatting." >&2; \
	fi

# ConPTY bundle: refresh from the pinned NuGet package, bump the pin, or
# verify the fetched binaries. See data/conpty/README.md.
.PHONY: fetch-conpty fetch-conpty-all update-conpty check-conpty-update verify-conpty
fetch-conpty:
	@if test -n "$(CONPTY_ARCH)"; then \
	    "$(top_srcdir)/scripts/fetch_conpty.sh" --arch "$(CONPTY_ARCH)"; \
	fi

fetch-conpty-all:
	"$(top_srcdir)/scripts/fetch_conpty.sh" --arch all

update-conpty:
	"$(top_srcdir)/scripts/update_conpty.py"

check-conpty-update:
	"$(top_srcdir)/scripts/update_conpty.py" --check

verify-conpty:
	@if test -n "$(CONPTY_ARCH)"; then \
	    "$(top_srcdir)/scripts/verify_conpty_bundle.py" --arch "$(CONPTY_ARCH)"; \
	fi

# Portable ZIP target — on Windows, stages exe + DLLs + terminfo + launcher .cmd
# into a self-contained ZIP. On macOS, stages a .app bundle with bundled dylibs,
# terminfo, resources, and Info.plist into a ZIP. The ConPTY prerequisites are
# no-ops on a non-Windows host (CONPTY_ARCH is empty there).
.PHONY: dist-portable
dist-portable: fetch-conpty verify-conpty
	@sh "$(top_srcdir)/scripts/make-dist-zip.sh" "$(abs_builddir)" "$(top_srcdir)" "$(portty_backend)"

# Bear target — produce compile_commands.json for clangd by intercepting
# the build. Cleans first so bear sees every compile invocation.
.PHONY: bear
bear:
	@command -v bear >/dev/null 2>&1 || { \
	    echo "bear not installed - install via your package manager." >&2; \
	    exit 1; \
	}
	$(MAKE) clean
	bear --output $(top_srcdir)/compile_commands.json -- $(MAKE)
