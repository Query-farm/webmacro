<p align="center">
  <a href="https://query.farm">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="https://query.farm/media-kit/logo/wordmark-dark.svg">
      <img alt="Query.Farm" src="https://query.farm/media-kit/logo/wordmark-light.svg" height="64">
    </picture>
  </a>
</p>

# DuckDB WebMacro Extension

[![DuckDB](https://img.shields.io/badge/DuckDB-community_extension-fdf1e0?logo=duckdb&logoColor=fff000)](https://duckdb.org/community_extensions/extensions/webmacro.html)
[![v1.5 build](https://github.com/Query-farm/webmacro/actions/workflows/MainDistributionPipeline.yml/badge.svg?branch=v1.5)](https://github.com/Query-farm/webmacro/actions/workflows/MainDistributionPipeline.yml?query=branch%3Av1.5)

This extension allows loading DuckDB Macros (both scalar and table) from URLs, gists, pastes, etc.

## Documentation

Full documentation, including installation, usage, the function reference, and cookbook examples, is available at:

**[https://query.farm/products/extensions/webmacro](https://query.farm/products/extensions/webmacro)**

## Installation

```sql
INSTALL webmacro FROM community;
LOAD webmacro;
```
