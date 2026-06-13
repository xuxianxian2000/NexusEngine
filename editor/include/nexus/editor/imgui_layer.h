#pragma once

// ============================================================================
// imgui_layer.h - Thin ImGui abstraction for NexusEngine editor
//
// Provides a minimal wrapper over Dear ImGui for editor panel rendering.
// When NEXUS_HAS_IMGUI is defined, these forward to real ImGui calls.
// Otherwise, they act as no-ops so the editor compiles without ImGui linked.
// ============================================================================

#include "nexus/core/types.h"
#include "nexus/core/math.h"
#include <string>
#include <functional>

namespace nexus::editor::imgui {

// ── ImGui state (set by application layer each frame) ───────────────────────

struct ImGuiContext {
    bool initialized{false};
};

inline ImGuiContext& ctx() {
    static ImGuiContext s_ctx;
    return s_ctx;
}

inline bool available() { return ctx().initialized; }
inline void set_available(bool v) { ctx().initialized = v; }

// ── Window / Layout ─────────────────────────────────────────────────────────

inline bool begin_window(const char* name, bool* open = nullptr) {
#ifdef NEXUS_HAS_IMGUI
    return ImGui::Begin(name, open);
#else
    (void)name; (void)open;
    return true; // always "open" in headless mode
#endif
}

inline void end_window() {
#ifdef NEXUS_HAS_IMGUI
    ImGui::End();
#endif
}

// ── Tree nodes ──────────────────────────────────────────────────────────────

inline bool tree_node(const char* label, bool selected = false, bool leaf = false) {
#ifdef NEXUS_HAS_IMGUI
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selected) flags |= ImGuiTreeNodeFlags_Selected;
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    return ImGui::TreeNodeEx(label, flags);
#else
    (void)label; (void)selected; (void)leaf;
    return false;
#endif
}

inline void tree_pop() {
#ifdef NEXUS_HAS_IMGUI
    ImGui::TreePop();
#endif
}

// ── Text / Labels ───────────────────────────────────────────────────────────

inline void text(const char* fmt, ...) {
#ifdef NEXUS_HAS_IMGUI
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
#else
    (void)fmt;
#endif
}

inline void text_colored(Vec4 color, const char* text_str) {
#ifdef NEXUS_HAS_IMGUI
    ImGui::TextColored(ImVec4(color.r, color.g, color.b, color.a), "%s", text_str);
#else
    (void)color; (void)text_str;
#endif
}

inline void separator() {
#ifdef NEXUS_HAS_IMGUI
    ImGui::Separator();
#endif
}

inline bool collapsing_header(const char* label) {
#ifdef NEXUS_HAS_IMGUI
    return ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
#else
    (void)label;
    return true;
#endif
}

// ── Input widgets ───────────────────────────────────────────────────────────

inline bool input_float(const char* label, float* v) {
#ifdef NEXUS_HAS_IMGUI
    return ImGui::DragFloat(label, v, 0.1f);
#else
    (void)label; (void)v;
    return false;
#endif
}

inline bool input_vec3(const char* label, Vec3* v) {
#ifdef NEXUS_HAS_IMGUI
    return ImGui::DragFloat3(label, &v->x, 0.1f);
#else
    (void)label; (void)v;
    return false;
#endif
}

inline bool input_vec2(const char* label, Vec2* v) {
#ifdef NEXUS_HAS_IMGUI
    return ImGui::DragFloat2(label, &v->x, 0.1f);
#else
    (void)label; (void)v;
    return false;
#endif
}

inline bool input_vec4(const char* label, Vec4* v) {
#ifdef NEXUS_HAS_IMGUI
    return ImGui::DragFloat4(label, &v->x, 0.1f);
#else
    (void)label; (void)v;
    return false;
#endif
}

inline bool input_color4(const char* label, Vec4* v) {
#ifdef NEXUS_HAS_IMGUI
    return ImGui::ColorEdit4(label, &v->x);
#else
    (void)label; (void)v;
    return false;
#endif
}

inline bool input_text(const char* label, std::string& buf) {
#ifdef NEXUS_HAS_IMGUI
    char tmp[256];
    std::strncpy(tmp, buf.c_str(), sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    if (ImGui::InputText(label, tmp, sizeof(tmp))) {
        buf = tmp;
        return true;
    }
    return false;
#else
    (void)label; (void)buf;
    return false;
#endif
}

inline bool checkbox(const char* label, bool* v) {
#ifdef NEXUS_HAS_IMGUI
    return ImGui::Checkbox(label, v);
#else
    (void)label; (void)v;
    return false;
#endif
}

inline bool button(const char* label) {
#ifdef NEXUS_HAS_IMGUI
    return ImGui::Button(label);
#else
    (void)label;
    return false;
#endif
}

inline void same_line() {
#ifdef NEXUS_HAS_IMGUI
    ImGui::SameLine();
#endif
}

// ── Scrolling ───────────────────────────────────────────────────────────────

inline void set_scroll_here_y() {
#ifdef NEXUS_HAS_IMGUI
    ImGui::SetScrollHereY(1.0f);
#endif
}

inline bool begin_child(const char* id, float height = 0.0f, bool border = true) {
#ifdef NEXUS_HAS_IMGUI
    return ImGui::BeginChild(id, ImVec2(0, height), border);
#else
    (void)id; (void)height; (void)border;
    return true;
#endif
}

inline void end_child() {
#ifdef NEXUS_HAS_IMGUI
    ImGui::EndChild();
#endif
}

// ── Context menu ────────────────────────────────────────────────────────────

inline bool begin_popup_context_item(const char* id = nullptr) {
#ifdef NEXUS_HAS_IMGUI
    return ImGui::BeginPopupContextItem(id);
#else
    (void)id;
    return false;
#endif
}

inline bool menu_item(const char* label) {
#ifdef NEXUS_HAS_IMGUI
    return ImGui::MenuItem(label);
#else
    (void)label;
    return false;
#endif
}

inline void end_popup() {
#ifdef NEXUS_HAS_IMGUI
    ImGui::EndPopup();
#endif
}

// ── Drag-drop ───────────────────────────────────────────────────────────────

inline bool begin_drag_source() {
#ifdef NEXUS_HAS_IMGUI
    return ImGui::BeginDragDropSource();
#else
    return false;
#endif
}

inline void set_drag_payload(const char* type, const void* data, size_t size) {
#ifdef NEXUS_HAS_IMGUI
    ImGui::SetDragDropPayload(type, data, size);
#else
    (void)type; (void)data; (void)size;
#endif
}

inline void end_drag_source() {
#ifdef NEXUS_HAS_IMGUI
    ImGui::EndDragDropSource();
#endif
}

inline bool begin_drag_target() {
#ifdef NEXUS_HAS_IMGUI
    return ImGui::BeginDragDropTarget();
#else
    return false;
#endif
}

inline const void* accept_drag_payload(const char* type) {
#ifdef NEXUS_HAS_IMGUI
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type))
        return payload->Data;
    return nullptr;
#else
    (void)type;
    return nullptr;
#endif
}

inline void end_drag_target() {
#ifdef NEXUS_HAS_IMGUI
    ImGui::EndDragDropTarget();
#endif
}

// ── Misc ────────────────────────────────────────────────────────────────────

inline bool is_item_clicked(int button = 0) {
#ifdef NEXUS_HAS_IMGUI
    return ImGui::IsItemClicked(button);
#else
    (void)button;
    return false;
#endif
}

inline float get_content_width() {
#ifdef NEXUS_HAS_IMGUI
    return ImGui::GetContentRegionAvail().x;
#else
    return 800.0f;
#endif
}

} // namespace nexus::editor::imgui
