// Accessibility: Dear ImGui's side
//
// ImGui keeps no tree of what it shows, so this builds one each frame,
// while an assistive tool is listening, from the test engine's item
// hooks (IMGUI_ENABLE_TEST_ENGINE, imconfig.h), and the text fields'
// own reports from the patched InputTextEx (imgui_a11y.h).
//
// This is free and unencumbered software released into the public domain.

// The test engine's hooks, which ImGui calls only while its context has
// TestEngineHookItems set.
void ImGuiTestEngineHook_ItemAdd(ImGuiContext *, ImGuiID, ImRect const &,
                                 ImGuiLastItemData const *)
{
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext *, ImGuiID, char const *, ImGuiItemStatusFlags)
{
}

void ImGuiTestEngineHook_Log(ImGuiContext *, char const *, ...)
{
}

char const *ImGuiTestEngine_FindItemDebugLabel(ImGuiContext *, ImGuiID)
{
    return 0;
}

// The patched InputTextEx's halves (imgui_a11y.h).
extern "C" int prospero_axedit(unsigned, char const *, int, ProsperoAxEdit *)
{
    return 0;
}

extern "C" void prospero_axfield(ProsperoAxField const *)
{
}
