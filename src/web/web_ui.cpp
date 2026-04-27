// HTML/CSS/JS for the web UI lives in data/index.html since v0.28.5.
// scripts/gzip_web_ui.py reads that file at build time and produces
// web_ui_gz.h, which is the symbol web_server.cpp actually serves.
// This .cpp file used to hold the 5000-LOC raw-string literal for the
// gzip script to scrape — now it just emits the legacy stub symbols
// declared in web_ui.h (kept extern for ABI compat with anything that
// might still link against them).
#include "web_ui.h"
#include "web_ui_gz.h"  // auto-generated, defines WEB_INDEX_HTML_GZ + _LEN
#include <Arduino.h>

const char WEB_INDEX_HTML[] = "";
const size_t WEB_INDEX_HTML_LEN = 0;
