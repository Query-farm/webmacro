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

The extension registers a single scalar function `load_macro_from_url(VARCHAR) -> VARCHAR` that:
1. Fetches SQL from a URL using httplib
2. Validates the content contains a macro definition
3. Checks for dangerous SQL commands (DELETE, DROP, etc.)
4. Executes the macro creation in DuckDB
5. Returns success message with the macro name

WASM builds use a replacement HTTP library (`wasm_httplib_replacement.hpp`) instead of OpenSSL-based httplib.
