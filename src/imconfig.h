// Dear ImGui compile-time options (IMGUI_USER_CONFIG)
//
// Every translation unit that includes imgui.h sees these, the library
// and the harness alike, so they agree on the data layouts.
//
// This is free and unencumbered software released into the public domain.
#pragma once

#define IMGUI_USE_WCHAR32  // code points past U+FFFF, fixed from day one
#define IMGUI_ENABLE_FREETYPE
#define IMGUI_DISABLE_STB_TRUETYPE
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DISABLE_DEMO_WINDOWS  // imgui_demo.cpp: empty stubs only

// A failed assertion goes to prospero_imassert (app.cpp), which logs it.
// Debug builds then trap. Release builds continue: the engine shares the
// process, so one GUI mistake must not end every session.
extern "C" void prospero_imassert(char const *expr, char const *file, int line);
#define IM_ASSERT(e) ((e) ? (void)0 : prospero_imassert(#e, __FILE__, __LINE__))
