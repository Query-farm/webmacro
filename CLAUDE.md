# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

DuckDB WebMacro Extension - allows loading DuckDB macros (scalar and table) from URLs, gists, and other remote sources. This is a DuckDB community extension.

## Build Commands

```bash
# Build release version (default)
GEN=ninja make release

# Build debug version
GEN=ninja make debug

```

## Running Tests

Tests use DuckDB's SQLLogicTest format in `test/sql/`:

```bash
# Run tests (release build)
make test

# Run tests (debug build)
make test_debug
```

When running tests use a debug build since it adds more assertions.

All extension functions should be documented inside of DuckDB with CreateScalarFunctionInfo or CreateAggregateFunctionInfo or the appropriate type for the function.  This documentation of the function should include examples, parameter types and parameter names.  The function should be categorized.


## Architecture

This is a single-file DuckDB extension following the standard extension template pattern:

- `src/webmacro_extension.cpp` - Main extension implementation
- `src/include/webmacro_extension.hpp` - Extension class header
- `extension_config.cmake` - DuckDB extension loader configuration
- `duckdb/` - DuckDB source as git submodule
- `extension-ci-tools/` - DuckDB extension CI tools submodule

The extension registers a single volatile scalar function `load_macro_from_url(url VARCHAR) -> VARCHAR` that:
1. Checks the URL: only `https://` (plain `http://` only with `SET webmacro_allow_http = true`), and refuses when
   `enable_external_access` is false
2. Fetches it through DuckDB's `HTTPUtil` (autoloads httpfs for https://), so `http_timeout`, `http_retries`,
   `http_proxy` and HTTP secrets apply. Redirects are followed by hand so every hop is re-checked; the body is capped
   at `webmacro_max_response_size` bytes (default 1 MiB)
3. Parses the content with DuckDB's parser and requires exactly one `CREATE MACRO` statement (scalar or table, TEMP ok)
4. Binds it and creates the macro through the catalog in the caller's own transaction (caller's search path,
   rollback removes it, read-only databases/transactions refuse)
5. Returns a success message with the macro name

WASM builds fetch with a synchronous XMLHttpRequest (`wasm_httplib_replacement.hpp`) instead of HTTPUtil.

## Tests

The default suite (`test/sql/*.test`) is offline. Tests that need HTTP use a local fixture server and are skipped
unless `WEBMACRO_FIXTURE_URL` is set:

```bash
python3 test/fixture_server.py 8765 &
WEBMACRO_FIXTURE_URL=http://127.0.0.1:8765 make test_release
```

`webmacro_network.test` (real HTTPS to GitHub) runs only with `WEBMACRO_NETWORK_TESTS=1` and httpfs available.
