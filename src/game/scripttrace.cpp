// Diagnostics (-scripttrace): logs each UnrealScript function the first time it runs, every state
// change and map load, and every 5 seconds the functions that ran most in that window. When the game
// sits waiting (a black screen with the music playing), the last new functions show what it did and
// the busiest ones show what it keeps polling.
#include "game/game.h"
#include "game/unreal.h"
#include "hook.h"
#include "foundation/foundation.h"
#include "macho.h"
#include "objc/runtime.h"
#include <windows.h>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace game {

namespace {

constexpr u64 kObjOuter = 0x40;
constexpr u64 kUrlMap = 0x28;  // FURL: Protocol, Host, Port, Map

GuestAddr g_call_function = 0, g_process_event = 0, g_goto_state = 0, g_browse = 0, g_load_map = 0;

std::mutex g_mutex;
std::unordered_set<GuestAddr> g_seen;
std::unordered_map<GuestAddr, u64> g_counts;  // calls in the current window
u64 g_window_start = 0;

std::string function_name(cpu::Thread& t, GuestAddr fn) {
    return ue::object_name(t, *gptr<u64>(fn + kObjOuter)) + "." + ue::object_name(t, fn);
}

void report_window(cpu::Thread& t) {
    std::vector<std::pair<u64, GuestAddr>> top;
    for (auto& [fn, n] : g_counts) top.push_back({n, fn});
    g_counts.clear();
    std::sort(top.rbegin(), top.rend());
    if (top.size() > 25) top.resize(25);
    std::string line;
    for (auto& [n, fn] : top) line += " " + function_name(t, fn) + "=" + std::to_string(n);
    LOG_INFO("script: busiest in the last 5 s:%s", line.c_str());
}

// debug.ibport.swipelog 1: every call of the fight input functions (swipes, dodges, the input gate).
bool swipe_logged(cpu::Thread& t, GuestAddr fn) {
    static std::unordered_map<GuestAddr, bool> cache;
    auto it = cache.find(fn);
    if (it != cache.end()) return it->second;
    std::string n = function_name(t, fn);
    bool yes = false;
    for (const char* p : {"SwordPC.Slash", "SwordPC.ButtonDodge", "SwordPC.ReleaseButtonDodge", "SwordPC.InputGate", "SwordPC.BlockInput",
                          "SwordPC.AcceptValidInput", "SwordPC.AllowInput", "SwordPC.ForceOpenInput", "SwordPC.SetPlayerInputFilter",
                          "SwordPlayer.OnValidInput", "StabModeMulti.OnStabPointTouched", "SwordPlayer.ClearCombo", "SwordPC.Stab", "SwordPC.Tap",
                          "SwordPlayer.OnComboHit", "DoAttackActionAnim.ComboStepMatch", "DoAttackActionAnim.UndoLastCombo"})
        if (n.rfind(p, 0) == 0) yes = true;
    return cache[fn] = yes;
}

bool swipelog_on() {
#ifdef __ANDROID__
    static int on = -1;
    if (on < 0) {
        char v[PROP_VALUE_MAX] = "";
        on = __system_property_get("debug.ibport.swipelog", v) > 0 && v[0] == '1';
    }
    return on;
#else
    return false;
#endif
}

void note_call(cpu::Thread& t, GuestAddr obj, GuestAddr fn, const char* how) {
    if (!fn) return;
    std::lock_guard lock(g_mutex);
    if (swipelog_on() && swipe_logged(t, fn)) LOG_INFO("swipelog: %s %s", how, function_name(t, fn).c_str());
    g_counts[fn]++;
    if (g_seen.insert(fn).second)
        LOG_INFO("script: %s %s on %s (%s)", how, function_name(t, fn).c_str(), ue::object_name(t, obj).c_str(),
                 ue::class_name(t, obj).c_str());
    u64 now = GetTickCount64();
    if (!g_window_start) g_window_start = now;
    if (now - g_window_start >= 5000) {
        g_window_start = now;
        report_window(t);
    }
}

std::string url_map(GuestAddr url) { return url ? ue::read_fstring(url + kUrlMap) : ""; }

}  // namespace

void install_script_trace(const macho::Image& img) {
    auto find = [&](const char* sym) {
        GuestAddr a = img.find(sym);
        if (!a) LOG_WARN("scripttrace: %s not found", sym);
        return a;
    };
    GuestAddr call = find("__ZN7UObject12CallFunctionER6FFramePvP9UFunction");
    GuestAddr event = find("__ZN7UObject12ProcessEventEP9UFunctionPvS2_");
    GuestAddr state = find("__ZN7UObject9GotoStateE5FNamejj");
    GuestAddr browse = find("__ZN11UGameEngine6BrowseE4FURLR7FString");
    GuestAddr load = find("__ZN11UGameEngine7LoadMapERK4FURLP13UPendingLevelR7FString");
    if (call)
        g_call_function = hook::install(call, "UObject::CallFunction", [](cpu::Thread& t) {
            note_call(t, t.x(0), t.x(3), "call");
            t.jump(g_call_function);
        });
    if (event)
        g_process_event = hook::install(event, "UObject::ProcessEvent", [](cpu::Thread& t) {
            note_call(t, t.x(0), t.x(1), "event");
            t.jump(g_process_event);
        });
    if (state)
        g_goto_state = hook::install(state, "UObject::GotoState", [](cpu::Thread& t) {
            LOG_INFO("script: %s (%s) goes to state %s", ue::object_name(t, t.x(0)).c_str(), ue::class_name(t, t.x(0)).c_str(),
                     ue::name_string(t, t.x(1)).c_str());
            t.jump(g_goto_state);
        });
    if (browse)
        g_browse = hook::install(browse, "UGameEngine::Browse", [](cpu::Thread& t) {
            LOG_INFO("script: Browse to map '%s'", url_map(t.x(1)).c_str());
            t.jump(g_browse);
        });
    if (load)
        g_load_map = hook::install(load, "UGameEngine::LoadMap", [](cpu::Thread& t) {
            LOG_INFO("script: LoadMap '%s'", url_map(t.x(1)).c_str());
            t.jump(g_load_map);
        });
    // Full-screen movies: the game thread requests them and waits on -[FMovieHelper bIsPlaying].
    static GuestAddr s_play_request = 0;
    if (GuestAddr play = img.find("__ZN22FFullScreenMovieIPhone19GameThreadPlayMovieE10EMovieModePKwiii"))
        s_play_request = hook::install(play, "FFullScreenMovieIPhone::GameThreadPlayMovie", [](cpu::Thread& t) {
            std::string name;
            for (const u32* w = gptr<u32>(t.x(2)); t.x(2) && *w && name.size() < 200; w++) name += *w < 0x80 ? (char)*w : '?';
            LOG_INFO("script: GameThreadPlayMovie mode %d '%s' flags 0x%x", (int)t.x(1), name.c_str(), (unsigned)t.x(3));
            t.jump(s_play_request);
        });
    LOG_INFO("scripttrace: on");
}

}  // namespace game

namespace game {

// After class realization: the main-thread side of full-screen movies.
void install_script_trace_objc() {
    objc::Class helper = objc::class_named("FMovieHelper");
    if (!helper) return;
    static GuestAddr s_play = 0, s_stopped = 0, s_finished = 0;
    if (GuestAddr imp = objc::lookup_imp(helper, objc::sel("PlayMovie:")))
        s_play = hook::install(imp, "-[FMovieHelper PlayMovie:]", [](cpu::Thread& t) {
            LOG_INFO("script: -[FMovieHelper PlayMovie:%s]", ns::utf8(t.x(2)).c_str());
            t.jump(s_play);
        });
    if (GuestAddr imp = objc::lookup_imp(helper, objc::sel("OnMovieStopped")))
        s_stopped = hook::install(imp, "-[FMovieHelper OnMovieStopped]", [](cpu::Thread& t) {
            LOG_INFO("script: -[FMovieHelper OnMovieStopped]");
            t.jump(s_stopped);
        });
    if (GuestAddr imp = objc::lookup_imp(helper, objc::sel("OnMovieFinished:")))
        s_finished = hook::install(imp, "-[FMovieHelper OnMovieFinished:]", [](cpu::Thread& t) {
            LOG_INFO("script: -[FMovieHelper OnMovieFinished:]");
            t.jump(s_finished);
        });
}

}  // namespace game
