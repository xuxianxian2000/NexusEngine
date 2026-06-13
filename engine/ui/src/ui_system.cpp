#include "nexus/ui/ui_system.h"
#include "nexus/renderer/batch_renderer_2d.h"
#include "nexus/core/log.h"

namespace nexus::ui {

// ── FocusNavigator ──────────────────────────────────────────────────────────

void FocusNavigator::rebuild() {
    focusables_.clear();
    if (root_) collect_focusables(root_);
    // Maintain focus if possible
    if (focus_index_ >= static_cast<i32>(focusables_.size())) {
        focus_index_ = focusables_.empty() ? -1 : 0;
    }
}

void FocusNavigator::collect_focusables(Widget* w) {
    if (!w->visible || w->disabled) return;
    if (w->focusable) {
        focusables_.push_back(w);
    }
    for (auto& child : w->children()) {
        collect_focusables(child.get());
    }
}

void FocusNavigator::move_next() {
    if (focusables_.empty()) return;
    if (focus_index_ >= 0 && focus_index_ < static_cast<i32>(focusables_.size())) {
        focusables_[static_cast<size_t>(focus_index_)]->focused = false;
    }
    focus_index_ = (focus_index_ + 1) % static_cast<i32>(focusables_.size());
    focusables_[static_cast<size_t>(focus_index_)]->focused = true;
}

void FocusNavigator::move_prev() {
    if (focusables_.empty()) return;
    if (focus_index_ >= 0 && focus_index_ < static_cast<i32>(focusables_.size())) {
        focusables_[static_cast<size_t>(focus_index_)]->focused = false;
    }
    focus_index_ = focus_index_ <= 0
        ? static_cast<i32>(focusables_.size()) - 1
        : focus_index_ - 1;
    focusables_[static_cast<size_t>(focus_index_)]->focused = true;
}

Widget* FocusNavigator::current() const {
    if (focus_index_ >= 0 && focus_index_ < static_cast<i32>(focusables_.size())) {
        return focusables_[static_cast<size_t>(focus_index_)];
    }
    return nullptr;
}

void FocusNavigator::focus(Widget* widget) {
    // Unfocus current
    if (focus_index_ >= 0 && focus_index_ < static_cast<i32>(focusables_.size())) {
        focusables_[static_cast<size_t>(focus_index_)]->focused = false;
    }
    // Find and focus new
    for (size_t i = 0; i < focusables_.size(); ++i) {
        if (focusables_[i] == widget) {
            focus_index_ = static_cast<i32>(i);
            widget->focused = true;
            return;
        }
    }
    focus_index_ = -1;
}

void FocusNavigator::clear() {
    if (focus_index_ >= 0 && focus_index_ < static_cast<i32>(focusables_.size())) {
        focusables_[static_cast<size_t>(focus_index_)]->focused = false;
    }
    focus_index_ = -1;
}

// ── UISystem ────────────────────────────────────────────────────────────────

UISystem::UISystem() {
    root_ = std::make_shared<Panel>();
    root_->layout.width = SizeValue::pct(100);
    root_->layout.height = SizeValue::pct(100);
    focus_nav_.set_root(root_.get());
}

UISystem::~UISystem() = default;

void UISystem::set_screen_size(float width, float height) {
    screen_size_ = {width, height};
}

void UISystem::set_theme(const UITheme& theme) {
    theme_ = theme;
}

void UISystem::process_mouse_move(Vec2 position) {
    Vec2 delta = position - mouse_pos_;
    mouse_pos_ = position;

    Widget* new_hovered = hit_test(position);
    auto prev_hovered_sp = hovered_widget_.lock();
    Widget* prev_hovered = prev_hovered_sp.get();

    // Mouse leave old widget
    if (prev_hovered && prev_hovered != new_hovered) {
        prev_hovered->hovered = false;
        UIEvent leave;
        leave.type = UIEventType::MouseLeave;
        leave.mouse_position = position;
        prev_hovered->dispatch_event(leave);
    }

    // Mouse enter new widget
    if (new_hovered && new_hovered != prev_hovered) {
        new_hovered->hovered = true;
        UIEvent enter;
        enter.type = UIEventType::MouseEnter;
        enter.mouse_position = position;
        new_hovered->dispatch_event(enter);
    }

    hovered_widget_ = new_hovered ? new_hovered->weak_from_this() : std::weak_ptr<Widget>{};

    // Drag (keep the widget alive for the duration of dispatch)
    if (auto pressed = pressed_widget_.lock()) {
        UIEvent drag;
        drag.type = UIEventType::DragMove;
        drag.mouse_position = position;
        drag.mouse_delta = delta;
        pressed->dispatch_event(drag);
    }
}

void UISystem::process_mouse_button(bool down) {
    if (down) {
        Widget* target = hit_test(mouse_pos_);
        pressed_widget_ = target ? target->weak_from_this() : std::weak_ptr<Widget>{};

        if (target) {
            target->pressed = true;
            UIEvent event;
            event.type = UIEventType::MouseDown;
            event.mouse_position = mouse_pos_;
            target->dispatch_event(event);

            // Focus
            if (target->focusable) {
                focus_nav_.focus(target);
            }
        } else {
            focus_nav_.clear();
        }
    } else {
        // Lock to keep the widget alive even if a handler removes it from the tree.
        if (auto pressed = pressed_widget_.lock()) {
            pressed->pressed = false;

            UIEvent up;
            up.type = UIEventType::MouseUp;
            up.mouse_position = mouse_pos_;
            pressed->dispatch_event(up);

            // Click if released on same widget
            Widget* release_target = hit_test(mouse_pos_);
            if (release_target == pressed.get()) {
                UIEvent click;
                click.type = UIEventType::Click;
                click.mouse_position = mouse_pos_;
                pressed->dispatch_event(click);
            }

            pressed_widget_.reset();
        }
    }
}

void UISystem::process_scroll(float delta) {
    Widget* target = hit_test(mouse_pos_);
    if (target) {
        UIEvent event;
        event.type = UIEventType::Scroll;
        event.scroll_delta = delta;
        event.mouse_position = mouse_pos_;
        target->dispatch_event(event);
    }
}

void UISystem::process_key(i32 key_code, bool down) {
    Widget* focused = focus_nav_.current();
    if (focused) {
        UIEvent event;
        event.type = down ? UIEventType::KeyDown : UIEventType::KeyUp;
        event.key_code = key_code;
        focused->dispatch_event(event);
    }
}

void UISystem::process_text_input(u32 codepoint) {
    Widget* focused = focus_nav_.current();
    if (focused) {
        UIEvent event;
        event.type = UIEventType::TextInput;
        event.character = codepoint;
        focused->dispatch_event(event);
    }
}

void UISystem::navigate_next() {
    focus_nav_.move_next();
}

void UISystem::navigate_prev() {
    focus_nav_.move_prev();
}

void UISystem::navigate_activate() {
    Widget* focused = focus_nav_.current();
    if (focused) {
        UIEvent event;
        event.type = UIEventType::Click;
        focused->dispatch_event(event);
    }
}

void UISystem::update() {
    // Layout
    Rect screen_rect = {{0.0f, 0.0f}, screen_size_};
    root_->perform_layout(screen_rect);

    // Rebuild focus list
    focus_nav_.rebuild();

    // Collect draw commands
    draw_commands_.clear();
    root_->collect_draw_commands(draw_commands_);
}

Widget* UISystem::hit_test(Vec2 pos) const {
    return hit_test_recursive(root_.get(), pos);
}

Widget* UISystem::hit_test_recursive(Widget* w, Vec2 pos) const {
    if (!w->visible || !w->interactive) return nullptr;

    // Check children in reverse order (front-to-back in draw order = last child on top)
    const auto& children = w->children();
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        Widget* hit = hit_test_recursive(it->get(), pos);
        if (hit) return hit;
    }

    // Check self
    if (w->computed_rect().contains(pos) && w != root_.get()) {
        return w;
    }

    return nullptr;
}

void UISystem::render(BatchRenderer2D& renderer) const {
    for (const auto& cmd : draw_commands_) {
        Vec2 pos = {cmd.rect.position.x + cmd.rect.size.x * 0.5f,
                    cmd.rect.position.y + cmd.rect.size.y * 0.5f};
        Vec2 size = cmd.rect.size;

        switch (cmd.type) {
        case Widget::DrawCommand::Type::Rect:
            renderer.draw_quad(pos, size, cmd.color);
            // Draw border if present
            if (cmd.border_width > 0.0f) {
                renderer.draw_rect(cmd.rect.position, size,
                                   cmd.border_color, cmd.border_width);
            }
            break;

        case Widget::DrawCommand::Type::Image:
            if (cmd.texture != UI_INVALID_HANDLE) {
                renderer.draw_quad(pos, size, cmd.texture, cmd.color);
            } else {
                renderer.draw_quad(pos, size, cmd.color);
            }
            break;

        case Widget::DrawCommand::Type::NineSlice:
            if (cmd.nine_slice.texture != UI_INVALID_HANDLE) {
                renderer.draw_quad(pos, size, cmd.nine_slice.texture, cmd.color);
            } else {
                renderer.draw_quad(pos, size, cmd.color);
            }
            break;

        case Widget::DrawCommand::Type::Text:
            if (font_ && font_->is_valid() && font_atlas_ != UI_INVALID_HANDLE) {
                // Render text using the bitmap font glyph atlas
                auto vertices = font_->generate_vertices(
                    cmd.text, cmd.rect.position);
                // Each 6 vertices = 1 glyph quad (2 triangles)
                for (size_t v = 0; v + 5 < vertices.size(); v += 6) {
                    auto& v0 = vertices[v];     // top-left
                    auto& v2 = vertices[v + 2]; // bottom-right
                    Vec2 glyph_pos{v0.x, v0.y};
                    Vec2 glyph_size{v2.x - v0.x, v2.y - v0.y};
                    Vec2 uv_min{v0.u, v0.v};
                    Vec2 uv_max{v2.u, v2.v};
                    renderer.draw_glyph(glyph_pos, glyph_size,
                                        font_atlas_, uv_min, uv_max, cmd.color);
                }
            } else {
                // Fallback: draw a placeholder tinted quad
                renderer.draw_quad(pos, size, Vec4{cmd.color.r, cmd.color.g,
                                                   cmd.color.b, cmd.color.a * 0.15f});
            }
            break;
        }
    }
}

} // namespace nexus::ui
