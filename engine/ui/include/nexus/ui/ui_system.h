#pragma once

#include "nexus/ui/ui_core.h"
#include "nexus/ui/widgets.h"
#include "nexus/ui/bitmap_font.h"
#include <vector>
#include <memory>

namespace nexus { class BatchRenderer2D; }

namespace nexus::ui {

// ─────────────────────────────────────────────────────────────────────────────
// FocusNavigator — keyboard/gamepad navigation among focusable widgets
// ─────────────────────────────────────────────────────────────────────────────

class FocusNavigator {
public:
    void set_root(Widget* root) { root_ = root; }

    /// Rebuild the focusable list by DFS traversal.
    void rebuild();

    /// Navigate focus in a direction.
    void move_next();
    void move_prev();

    /// Get the currently focused widget.
    Widget* current() const;

    /// Set focus to a specific widget.
    void focus(Widget* widget);

    /// Clear focus.
    void clear();

    const std::vector<Widget*>& focusable_list() const { return focusables_; }

private:
    void collect_focusables(Widget* w);

    Widget* root_{nullptr};
    std::vector<Widget*> focusables_;
    i32 focus_index_{-1};
};

// ─────────────────────────────────────────────────────────────────────────────
// UISystem — manages the UI tree, input, layout, and rendering
// ─────────────────────────────────────────────────────────────────────────────

class UISystem {
public:
    UISystem();
    ~UISystem();

    /// Set the screen size for layout.
    void set_screen_size(float width, float height);

    /// Set the active theme.
    void set_theme(const UITheme& theme);
    const UITheme& theme() const { return theme_; }

    /// Root widget — add all top-level widgets here.
    WidgetPtr root() const { return root_; }

    /// Process mouse input.
    void process_mouse_move(Vec2 position);
    void process_mouse_button(bool down);
    void process_scroll(float delta);

    /// Process keyboard input.
    void process_key(i32 key_code, bool down);
    void process_text_input(u32 codepoint);

    /// Process gamepad navigation.
    void navigate_next();
    void navigate_prev();
    void navigate_activate();

    /// Run layout and collect draw commands.
    void update();

    /// Get draw commands for rendering.
    const std::vector<Widget::DrawCommand>& draw_commands() const { return draw_commands_; }

    /// Focus management.
    FocusNavigator& focus() { return focus_nav_; }

    /// Hit test — find the deepest widget at a screen position.
    Widget* hit_test(Vec2 pos) const;

    /// Set the bitmap font for text rendering.
    void set_font(BitmapFont* font) { font_ = font; }
    void set_font_atlas_texture(TextureHandle tex) { font_atlas_ = tex; }

    /// Render all collected draw commands via BatchRenderer2D.
    void render(BatchRenderer2D& renderer) const;

private:
    Widget* hit_test_recursive(Widget* w, Vec2 pos) const;

    WidgetPtr root_;
    UITheme theme_;
    FocusNavigator focus_nav_;
    std::vector<Widget::DrawCommand> draw_commands_;

    Vec2 screen_size_{1280.0f, 720.0f};
    Vec2 mouse_pos_{0.0f};
    // weak_ptr so a widget removed/freed from the tree between mouse events does
    // not leave these dangling; lock() before use to keep it alive during dispatch.
    std::weak_ptr<Widget> hovered_widget_;
    std::weak_ptr<Widget> pressed_widget_;

    BitmapFont* font_{nullptr};
    TextureHandle font_atlas_{UI_INVALID_HANDLE};
};

} // namespace nexus::ui
