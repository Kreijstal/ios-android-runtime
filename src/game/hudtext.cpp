// IB2: text the game draws at a fixed pixel size while the rest of the HUD grows with the screen. The HUD is laid
// out for a 640-pixel-tall screen and scaled to the real one, so on a 1440p picture these came out at less than
// half the size they had on the iPhones the game was made on. Both go through Canvas.TextSize, hooked here:
// - The numbers that fly off the health and shield bars in fights (SwordHudBase.DrawDisplayStats) are drawn
//   with the medium font at scale 1. The function works out each number's scale (local CurScale), measures the
//   text with Canvas.TextSize, then centres and draws it at CurScale; right after the measuring, CurScale is
//   multiplied by the screen's height over 640, so the number grows and stays centred.
// - The gold amount under the bag after a fight (ShowReward's SwordDrawInventoryItem) and the other item
//   labels: the label's TextHeight is in 640-tall units (the item scales it so itself when the label sits
//   on top), but the label is sized in raw pixels (bPixelsAreScaled). While
//   SwordDrawText.UpdateDrawData measures the text (CalcTextSize -> Canvas.TextSize), its text box height
//   (local BBSize.Y, the TextHeight at that point) is scaled the same way.
#include "game/game.h"
#include "game/unreal.h"
#include "hook.h"
#include "macho.h"
#include <unordered_map>

namespace game {

namespace {

constexpr u64 kFrameNode = 0x18, kFrameObject = 0x20, kFrameLocals = 0x30, kFramePrevious = 0x38;  // FFrame
constexpr u64 kObjOuter = 0x40;
constexpr float kDesignHeight = 640;  // SwordHudBase.HiResDrawScaleY = ViewportH / 640

enum class Caller { Other, DrawStats, CalcTextSize };

GuestAddr g_text_size_original = 0;
int g_cur_scale = -1;  // DrawDisplayStats' local CurScale

// What the script function `fn` calling Canvas.TextSize is (looked up once per function).
Caller caller(cpu::Thread& t, GuestAddr fn, GuestAddr frame) {
    static std::unordered_map<GuestAddr, Caller> known;
    auto it = known.find(fn);
    if (it != known.end()) return it->second;
    std::string name = ue::object_name(t, fn);
    GuestAddr obj = *gptr<u64>(frame + kFrameObject);
    Caller c = Caller::Other;
    if (name == "DrawDisplayStats") {
        g_cur_scale = ue::param_offset(t, obj, "DrawDisplayStats", "CurScale");
        if (g_cur_scale >= 0) c = Caller::DrawStats;
        else LOG_WARN("hud text: CurScale not found in DrawDisplayStats; fight numbers keep their size");
    } else if (name == "CalcTextSize" && ue::is_a(t, obj, "SwordDrawText")) {
        c = Caller::CalcTextSize;
    }
    return known[fn] = c;
}

// Where `update` (a running UpdateDrawData) keeps its local BBSize, or -1 (subclasses' own UpdateDrawData).
int box_size_offset(cpu::Thread& t, GuestAddr update) {
    static std::unordered_map<GuestAddr, int> known;
    auto it = known.find(update);
    if (it != known.end()) return it->second;
    int off = ue::object_name(t, update) == "UpdateDrawData" ? ue::function_local_offset(t, update, "BBSize") : -1;
    return known[update] = off;
}

// The label of a SwordDrawInventoryItem (its subobject ItemLabel, whose Outer is the item class's default object),
// sized in raw pixels.
bool is_pixel_sized_item_label(cpu::Thread& t, GuestAddr label) {
    bool pixels = false;
    GuestAddr item = *gptr<u64>(label + kObjOuter);
    return item && ue::is_a(t, item, "SwordDrawInventoryItem") && ue::read_bool(t, label, "bPixelsAreScaled", pixels) && pixels;
}

// UCanvas::execTextSize(FFrame& Stack, RESULT_DECL): Stack is the calling script function's frame.
void on_text_size(cpu::Thread& t) {
    GuestAddr canvas = t.x(0), frame = t.x(1);
    t.call(g_text_size_original, {t.x(0), t.x(1), t.x(2)});
    Caller c = caller(t, *gptr<u64>(frame + kFrameNode), frame);
    if (c == Caller::Other) return;
    int height = 0;
    if (!ue::read_property(t, canvas, "SizeY", height) || height <= 0) return;
    float scale = height / kDesignHeight;
    if (c == Caller::DrawStats) {
        gptr<float>(*gptr<u64>(frame + kFrameLocals) + g_cur_scale)[0] *= scale;
        return;
    }
    GuestAddr label = *gptr<u64>(frame + kFrameObject), update = *gptr<u64>(frame + kFramePrevious);
    if (!update || !is_pixel_sized_item_label(t, label)) return;  // (the property lookups also set up function_local_offset)
    int box = box_size_offset(t, *gptr<u64>(update + kFrameNode));
    if (box < 0) return;
    float& box_height = gptr<float>(*gptr<u64>(update + kFrameLocals) + box)[1];  // BBSize.Y
    static bool logged = false;
    if (!logged) LOG_INFO("hud text: item labels scaled by %.2f (%.1f -> %.1f pixels)", scale, box_height, box_height * scale);
    logged = true;
    box_height *= scale;
}

}  // namespace

void install_fight_text_scale(const macho::Image& img) {
    if (!is_ib2()) return;
    GuestAddr text_size = img.find("__ZN7UCanvas12execTextSizeER6FFramePv");
    if (!text_size) {
        LOG_WARN("hud text: Canvas.TextSize not found; fight numbers keep their size");
        return;
    }
    g_text_size_original = hook::install(text_size, "UCanvas::execTextSize", on_text_size);
}

}  // namespace game
