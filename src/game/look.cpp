// IB3: the Look sensitivity setting (touch). Dragging to look around goes through a full-screen touch zone
// (ChoiceZone while exploring, LookZone elsewhere) whose movement the engine hands on as the MouseX / MouseY
// axes in UMobilePlayerInput::SendInputAxis; the glide after a quick drag goes the same way. Those two axes
// are scaled there by the setting, whichever zone sent them. (Pads have their own Camera speed: game/controller.cpp.)
#include "game/game.h"
#include "game/unreal.h"
#include "hook.h"
#include "macho.h"
#include "settings.h"

namespace game {

namespace {

GuestAddr g_send_axis_original = 0;
float g_scale = 1;

void on_send_axis(cpu::Thread& t) {
    static u64 mouse_x = 0, mouse_y = 0;
    if (!mouse_x) mouse_x = ue::fname(t, "MouseX"), mouse_y = ue::fname(t, "MouseY");
    u64 key = t.x(1);
    if (key == mouse_x || key == mouse_y) t.set_s(0, t.s(0) * g_scale);
    t.jump(g_send_axis_original);
}

}  // namespace

void install_look_sensitivity(const macho::Image& img) {
    int percent = settings::get().look_sensitivity;
    if (is_ib2() || percent == 100) return;
    GuestAddr send = img.find("__ZN18UMobilePlayerInput13SendInputAxisE5FNameff");
    if (!send) {
        LOG_WARN("settings: touch input axes not found; looking around keeps its speed");
        return;
    }
    g_scale = percent / 100.0f;
    g_send_axis_original = hook::install(send, "UMobilePlayerInput::SendInputAxis", on_send_axis);
    LOG_INFO("settings: look sensitivity %d%%", percent);
}

}  // namespace game
