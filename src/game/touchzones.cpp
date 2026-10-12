// IB3's centre fight button (dodge back: zone SwordDodgeCenter, key Sword_BttnCenterDodge) is
// authored at 960x640 as 220x132 like the side dodge buttons, but it takes its height from its width, so on a wide screen it grows far taller than the
// side buttons (371 px at 1920x1080 instead of 222, reaching up to 2/3 of the screen height). An
// upward swipe starting in the lower part of the screen then pressed the centre button first: a dodge
// that cleared the combo, or a swipe that did nothing. The centre button gets the side buttons' Y and height.
// debug.ibport.zonefix 0 leaves them as they are (tests).
#include "game/game.h"
#include "game/unreal.h"
#include "hook.h"
#include "macho.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace game {

namespace {

bool fix_enabled() {
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = "";
    return !(__system_property_get("debug.ibport.zonefix", v) > 0 && v[0] == '0');
#else
    return true;
#endif
}

struct Zone {
    GuestAddr obj;
    std::string key;
    float x, y, w, h, aw, ah;
};

bool read_zone(cpu::Thread& t, GuestAddr z, Zone& out) {
    u64 key = 0;
    out.obj = z;
    if (!ue::read_property(t, z, "InputKey", key) || !ue::read_property(t, z, "X", out.x) || !ue::read_property(t, z, "Y", out.y) ||
        !ue::read_property(t, z, "SizeX", out.w) || !ue::read_property(t, z, "SizeY", out.h))
        return false;
    out.aw = out.w, out.ah = out.h;
    ue::read_property(t, z, "ActiveSizeX", out.aw);
    ue::read_property(t, z, "ActiveSizeY", out.ah);
    out.key = ue::name_string(t, key);
    return true;
}

bool is_centre_key(const std::string& k) { return k == "Sword_BttnCenterDodge"; }
bool is_side_key(const std::string& k) { return k == "Sword_BttnLeftDodge" || k == "Sword_BttnRightDodge"; }

void set_float(cpu::Thread& t, GuestAddr obj, const char* name, float v) {
    int off = ue::property_offset(t, obj, name);
    if (off >= 0) *gptr<float>(obj + off) = v;
}

}  // namespace

#ifdef __ANDROID__
// debug.ibport.zonelog 1: logs every state change of the dodge buttons' zones (tests: counts presses).
void log_zone_states(cpu::Thread& t) {
    static int on = -1;
    static std::map<GuestAddr, u8> states;
    if (on < 0) {
        char v[PROP_VALUE_MAX] = "";
        on = __system_property_get("debug.ibport.zonelog", v) > 0 && v[0] == '1';
    }
    if (!on) return;
    GuestAddr input = ue::player_input(t);
    ue::TArray<u64> zones{};
    if (!input || !ue::read_property(t, input, "MobileInputZones", zones) || !zones.data || zones.num <= 0 || zones.num > 512) return;
    for (int i = 0; i < zones.num; i++) {
        GuestAddr z = zones.at(i);
        u64 key = 0;
        u8 state = 0;
        if (!z || !ue::read_property(t, z, "InputKey", key) || !ue::read_property(t, z, "State", state)) continue;
        std::string k = ue::name_string(t, key);
        if (!is_centre_key(k) && !is_side_key(k)) continue;
        auto it = states.find(z);
        if (it != states.end() && it->second != state) LOG_INFO("zonelog: %s state %d -> %d", k.c_str(), it->second, state);
        states[z] = state;
    }
}
#endif

// IB3's swipes (UMobileInputZoneSwipe, e.g. CenterSwipeHoldZone with 12 directions: straight up and
// down only within 10 degrees) are decided in DetectSwipe as soon as the finger's last step is fast
// enough (MinSwipeSpeed 500 a second), from the swipe so far: on a fast frame that is only a few
// points (whole numbers) of the first wobbly movement, so a straight swipe often came out diagonal,
// more so at 120 fps (half the movement per frame). While the finger moves, the speed check now waits
// until the swipe is kMinDecideLength long; a lifted finger and the distance check are unchanged.
// debug.ibport.swipefix 0 turns this off, debug.ibport.swipelog 1 logs each decision (tests).
namespace {

constexpr float kMinDecideLength = 30;
GuestAddr g_detect_swipe = 0, g_process_touch = 0;
int g_touch_type = -1;  // ETouchType of the swipe zone touch being processed (1: moved)
bool g_swipe_fix = true, g_swipe_log = false;

bool prop_is(const char* name, char value) {
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = "";
    return __system_property_get(name, v) > 0 && v[0] == value;
#else
    (void)name, (void)value;
    return false;
#endif
}

void log_directions(cpu::Thread& t, GuestAddr z, float min_dist, float min_speed) {
    static std::map<GuestAddr, bool> dumped;
    if (dumped[z]) return;
    dumped[z] = true;
    s32 n = *gptr<s32>(z + 0x288);
    GuestAddr a = *gptr<u64>(z + 0x280);
    LOG_INFO("swipelog: zone %s min distance %.1f min speed %.1f, %d directions:", ue::object_name(t, z).c_str(), min_dist, min_speed, n);
    for (int i = 0; i < n && i < 16; i++) {
        GuestAddr e = a + i * 24;
        LOG_INFO("swipelog:   %s %.0f-%.0f degrees", ue::name_string(t, *gptr<u64>(e)).c_str(), *gptr<float>(e + 8), *gptr<float>(e + 12));
    }
}

void on_detect_swipe(cpu::Thread& t) {
    GuestAddr z = t.x(0);
    float dt = t.s(0);
    if (*gptr<u64>(z + 0x290) == 0) {  // no swipe decided yet for this touch
        float sx = *gptr<float>(z + 0x158), sy = *gptr<float>(z + 0x15c), cx = *gptr<float>(z + 0x160), cy = *gptr<float>(z + 0x164);
        float px = *gptr<float>(z + 0x178), py = *gptr<float>(z + 0x17c);
        float min_dist = *gptr<float>(z + 0x278), min_speed = *gptr<float>(z + 0x274);
        float dx = cx - sx, dy = cy - sy, len = std::sqrt(dx * dx + dy * dy);
        float step = std::sqrt((cx - px) * (cx - px) + (cy - py) * (cy - py));
        float speed = dt > 0 ? step / dt : INFINITY;
        bool by_distance = len > min_dist, by_speed = speed > min_speed;
        if (by_distance || by_speed) {
            bool wait = g_swipe_fix && !by_distance && g_touch_type == 1 && len < kMinDecideLength;
            if (g_swipe_log) {
                log_directions(t, z, min_dist, min_speed);
                LOG_INFO("swipelog: %s %s (%s): vector (%.0f, %.0f) length %.0f angle %.1f, step %.1f in %.4f s", ue::object_name(t, z).c_str(),
                         wait ? "waits" : "fires", by_distance ? "distance" : "speed", dx, dy, len, std::atan2(-dy, dx) * 57.29578f, step, dt);
            }
            if (wait) {
                t.set_x(0, 0);
                return;  // not yet: the next move decides, with a longer swipe
            }
        }
    }
    t.jump(g_detect_swipe);
}

void on_process_touch(cpu::Thread& t) {
    g_touch_type = (int)(t.x(2) & 0xff);
    t.jump(g_process_touch);
}

}  // namespace

void install_touchzones(const macho::Image& img) {
    if (is_ib2()) return;
    g_swipe_fix = !prop_is("debug.ibport.swipefix", '0');
    g_swipe_log = prop_is("debug.ibport.swipelog", '1');
    GuestAddr detect = img.find("__ZN21UMobileInputZoneSwipe11DetectSwipeEfjf");
    GuestAddr touch = img.find("__ZN21UMobileInputZoneSwipe12ProcessTouchEfj10ETouchType9FVector2Dfff");
    if (!detect || !touch || (!g_swipe_fix && !g_swipe_log)) {
        LOG_INFO("touchzones: swipe direction fix %s", detect && touch ? "off" : "not available");
        return;
    }
    g_process_touch = hook::install(touch, "UMobileInputZoneSwipe::ProcessTouch", on_process_touch);
    g_detect_swipe = hook::install(detect, "UMobileInputZoneSwipe::DetectSwipe", on_detect_swipe);
    LOG_INFO("touchzones: swipe direction fix %s%s", g_swipe_fix ? "on" : "off (debug.ibport.swipefix 0)", g_swipe_log ? ", logging" : "");
}

void touchzones_tick(cpu::Thread& t) {
    static u64 last = 0;
    static std::map<GuestAddr, std::string> logged;  // zone -> geometry last logged
    if (is_ib2()) return;
#ifdef __ANDROID__
    log_zone_states(t);
#endif
    u64 now = GetTickCount64();
    if (now - last < 250) return;
    last = now;
    GuestAddr input = ue::player_input(t);
    ue::TArray<u64> zones{};
    if (!input || !ue::read_property(t, input, "MobileInputZones", zones) || !zones.data || zones.num <= 0 || zones.num > 512) return;
    std::vector<Zone> all;
    const Zone* side = nullptr;
    for (int i = 0; i < zones.num; i++) {
        Zone z{};
        if (GuestAddr obj = zones.at(i); obj && read_zone(t, obj, z)) all.push_back(z);
    }
    for (auto& z : all) {
        char geo[160];
        snprintf(geo, sizeof geo, "%s at (%.0f, %.0f) %.0fx%.0f, active %.0fx%.0f", z.key.c_str(), z.x, z.y, z.w, z.h, z.aw, z.ah);
        if (prop_is("debug.ibport.zonelog", '1') && logged[z.obj] != geo) {
            logged[z.obj] = geo;
            LOG_INFO("touchzones: %s (%s) %s", ue::object_name(t, z.obj).c_str(), ue::class_name(t, z.obj).c_str(), geo);
        }
        if (is_side_key(z.key) && z.w > 0 && z.h > 0) side = &z;
    }
    if (!side || !fix_enabled()) return;
    for (auto& z : all) {
        if (!is_centre_key(z.key) || (z.h == side->h && z.y == side->y && z.ah == side->ah)) continue;
        set_float(t, z.obj, "Y", side->y);
        set_float(t, z.obj, "SizeY", side->h);
        set_float(t, z.obj, "ActiveSizeY", side->ah);
        LOG_INFO("touchzones: %s (%s) y %.0f height %.0f -> y %.0f height %.0f like %s", ue::object_name(t, z.obj).c_str(), z.key.c_str(), z.y,
                 z.h, side->y, side->h, side->key.c_str());
    }
}

}  // namespace game
