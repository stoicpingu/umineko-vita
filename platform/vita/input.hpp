#pragma once
#include <SDL2/SDL_scancode.h>
#include <cstdint>

namespace VitaInput {
// Keep Circle's identity until the engine knows whether dialogue is visible.
constexpr SDL_Scancode Circle = static_cast<SDL_Scancode>(SDL_NUM_SCANCODES + 4);

inline SDL_Scancode button(uint8_t index) {
    // Native SDL2 Vita button order. Preserve the established Vita controls,
    // including Start=system menu; desktop Tab would skip a chapter.
    static constexpr SDL_Scancode map[] = {
        SDL_SCANCODE_ESCAPE, Circle, SDL_SCANCODE_RETURN, SDL_SCANCODE_A,
        SDL_SCANCODE_H, static_cast<SDL_Scancode>(SDL_NUM_SCANCODES + 2),
        SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT,
        static_cast<SDL_Scancode>(SDL_NUM_SCANCODES + 1), SDL_SCANCODE_Z,
        SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_RCTRL, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN
    };
    return index < sizeof(map) / sizeof(map[0]) ? map[index] : SDL_SCANCODE_UNKNOWN;
}

inline SDL_Scancode action(SDL_Scancode code, bool dialogue_visible) {
    return code == Circle ? (dialogue_visible ? SDL_SCANCODE_Z : SDL_SCANCODE_ESCAPE) : code;
}
}

#include <SDL2/SDL_events.h>
#include <array>
#include <cmath>

namespace VitaInput {
// Navigation cadence is independent of rendering: fresh presses are immediate.
constexpr uint32_t NavigationRepeatDelayMs = 300;
constexpr uint32_t NavigationRepeatIntervalMs = 83;
constexpr SDL_Scancode LeftTrigger = static_cast<SDL_Scancode>(SDL_NUM_SCANCODES + 32);
constexpr SDL_Scancode RightTrigger = static_cast<SDL_Scancode>(SDL_NUM_SCANCODES + 33);
constexpr SDL_Scancode LeftStickUp = static_cast<SDL_Scancode>(SDL_NUM_SCANCODES + 34);
constexpr SDL_Scancode LeftStickDown = static_cast<SDL_Scancode>(SDL_NUM_SCANCODES + 35);
constexpr SDL_Scancode LeftStickLeft = static_cast<SDL_Scancode>(SDL_NUM_SCANCODES + 36);
constexpr SDL_Scancode LeftStickRight = static_cast<SDL_Scancode>(SDL_NUM_SCANCODES + 37);
constexpr SDL_Scancode RightStickUp = static_cast<SDL_Scancode>(SDL_NUM_SCANCODES + 38);
constexpr SDL_Scancode RightStickDown = static_cast<SDL_Scancode>(SDL_NUM_SCANCODES + 39);

// Consume native SDL joystick events once, on the event-fetch thread. Deliver
// complete press-time pulses: the interpreter acts on key-up. Physical release
// only rearms the button; held arrows repeat without depending on SDL key repeat.
class Translator {
    struct Held { bool down{false}; uint32_t since{0}; uint32_t last{0}; };
    std::array<Held, 16> buttons{};
    int leftDirection{-1}, rightDirection{0};
    float leftX{0}, leftY{0}, rightY{0};
    uint32_t leftSince{0}, leftLast{0}, rightLast{0};
    bool active{true};
    template<class Emit> void pulse(SDL_Scancode code, uint32_t now, Emit emit) {
        if (code == SDL_SCANCODE_UNKNOWN) return;
        for (auto type : {SDL_KEYDOWN, SDL_KEYUP}) {
            SDL_Event event{};
            event.type = type;
            event.key.timestamp = now;
            event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
            event.key.keysym.scancode = code;
            emit(event);
        }
    }
    static int direction(float x, float y) {
        return std::fabs(x) >= std::fabs(y) ? (x > 0 ? 3 : 2) : (y > 0 ? 1 : 0);
    }
    template<class Emit> void sticks(uint32_t now, Emit emit) {
        float magnitude = leftX * leftX + leftY * leftY;
        if (magnitude < 0.35f * 0.35f) leftDirection = -1;
        else if (leftDirection >= 0 || magnitude >= 0.5f * 0.5f) {
            int next = direction(leftX, leftY);
            if (next != leftDirection) {
                leftDirection = next; leftSince = leftLast = now;
                pulse(static_cast<SDL_Scancode>(LeftStickUp + next), now, emit);
            } else if (now - leftSince >= NavigationRepeatDelayMs && now - leftLast >= NavigationRepeatIntervalMs) {
                leftLast = now;
                pulse(static_cast<SDL_Scancode>(LeftStickUp + next), now, emit);
            }
        }
        if (std::fabs(rightY) < 0.4f) rightDirection = 0;
        else if (!rightDirection && std::fabs(rightY) >= 0.6f) {
            rightDirection = rightY < 0 ? -1 : 1; rightLast = now;
            pulse(rightDirection < 0 ? RightStickUp : RightStickDown, now, emit);
        } else if (rightDirection && now - rightLast >= 250) {
            rightLast = now;
            pulse(rightDirection < 0 ? RightStickUp : RightStickDown, now, emit);
        }
    }
public:
    template<class Emit> bool consume(const SDL_Event &event, uint32_t now, Emit emit) {
        if (event.type == SDL_WINDOWEVENT && (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                                             event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)) {
            active = event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED;
            buttons = {}; leftDirection = -1; rightDirection = 0;
            leftX = leftY = rightY = 0;
            return false;
        }
        if (event.type == SDL_JOYBUTTONDOWN || event.type == SDL_JOYBUTTONUP) {
            unsigned i = event.jbutton.button;
            if (i >= buttons.size()) return true;
            auto &held = buttons[i];
            bool down = event.type == SDL_JOYBUTTONDOWN;
            if (down && !held.down && active) {
                SDL_Scancode code = i == 4 ? LeftTrigger : i == 5 ? RightTrigger : button(i);
                pulse(code, now, emit);
                held.since = held.last = now;
            }
            held.down = down;
            return true;
        }
        if (event.type == SDL_JOYAXISMOTION) {
            float value = event.jaxis.value / 32768.0f;
            if (event.jaxis.axis == 0) leftX = value;
            else if (event.jaxis.axis == 1) leftY = value;
            else if (event.jaxis.axis == 3) rightY = value;
            if (active) sticks(now, emit);
            return true;
        }
        return event.type == SDL_JOYHATMOTION;
    }
    template<class Emit> void repeat(uint32_t now, Emit emit) {
        if (!active) return;
        for (unsigned i = 6; i <= 9; ++i) {
            auto &held = buttons[i];
            if (held.down && now - held.since >= NavigationRepeatDelayMs && now - held.last >= NavigationRepeatIntervalMs) {
                held.last = now;
                pulse(button(i), now, emit);
            }
        }
        sticks(now, emit);
    }
};
}
