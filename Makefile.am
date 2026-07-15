SUBDIRS = src data tests

ACLOCAL_AMFLAGS = -I m4

dist_doc_DATA = README.md

EXTRA_DIST = \
    autogen.sh \
    build-aux/git-version.sh \
    version \
    COPYING \
    .clang-format \
    scripts \
    examples

# Format target — runs clang-format on C/C++, shfmt on shell, black on Python, prettier on Markdown.
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
	@if command -v prettier >/dev/null 2>&1; then \
	    echo "Formatting Markdown with prettier..."; \
	    find $(top_srcdir) -maxdepth 2 -name '*.md' -print0 \
	        | xargs -0 prettier --write; \
	else \
	    echo "prettier not found - skipping Markdown formatting." >&2; \
	fi

# Portable Windows ZIP target — stages the exe, data files, runtime DLLs,
# compiled terminfo, and a launcher .cmd into a self-contained ZIP. Only
# meaningful on Windows (MSYS2 UCRT64), where ldd and tic are available.
.PHONY: dist-portable
dist-portable:
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
