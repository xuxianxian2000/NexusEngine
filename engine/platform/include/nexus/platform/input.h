#pragma once

#include "nexus/core/math.h"
#include <array>

struct GLFWwindow;

namespace nexus {

// Key codes (matching GLFW)
enum class Key : int {
    Space = 32, Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47,
    Num0 = 48, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Semicolon = 59, Equal = 61,
    A = 65, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    LeftBracket = 91, Backslash, RightBracket, GraveAccent = 96,
    Escape = 256, Enter, Tab, Backspace, Insert, Delete,
    Right, Left, Down, Up,
    PageUp, PageDown, Home, End,
    CapsLock = 280, ScrollLock, NumLock, PrintScreen, Pause,
    F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    LeftShift = 340, LeftControl, LeftAlt, LeftSuper,
    RightShift, RightControl, RightAlt, RightSuper,
    Menu,
    MaxKeys = 512
};

enum class MouseButton : int {
    Left = 0, Right = 1, Middle = 2,
    Button4, Button5, Button6, Button7, Button8,
    MaxButtons = 8
};

class Input {
public:
    static void init(GLFWwindow* window);
    static void update();

    // Keyboard
    [[nodiscard]] static bool key_down(Key key);
    [[nodiscard]] static bool key_pressed(Key key);
    [[nodiscard]] static bool key_released(Key key);

    // Mouse
    [[nodiscard]] static bool mouse_down(MouseButton button);
    [[nodiscard]] static bool mouse_pressed(MouseButton button);
    [[nodiscard]] static bool mouse_released(MouseButton button);
    [[nodiscard]] static Vec2 mouse_position();
    [[nodiscard]] static Vec2 mouse_delta();
    [[nodiscard]] static float scroll_delta();

    /// Feed a scroll event (called from the windowing layer's scroll callback).
    /// Accumulated until the next update() latches it into scroll_delta().
    static void on_scroll(double yoffset);

private:
    static GLFWwindow* s_window;
    static std::array<bool, static_cast<size_t>(Key::MaxKeys)> s_keys;
    static std::array<bool, static_cast<size_t>(Key::MaxKeys)> s_prev_keys;
    static std::array<bool, static_cast<size_t>(MouseButton::MaxButtons)> s_buttons;
    static std::array<bool, static_cast<size_t>(MouseButton::MaxButtons)> s_prev_buttons;
    static Vec2 s_mouse_pos;
    static Vec2 s_prev_mouse_pos;
    static float s_scroll_delta; // value visible this frame
    static float s_scroll_accum; // accumulated by on_scroll between updates
};

} // namespace nexus
