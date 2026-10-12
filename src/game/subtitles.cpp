// IB3: the Subtitle size setting. The engine's subtitles (FSubtitleManager, cutscenes and fight talk) are
// drawn with a font scale kept in the manager (+0x50, USwordGameViewportClient::SetSubtitleFontScale writes
// it); DisplaySubtitle multiplies it into the text size. While the subtitles are drawn the scale is the
// game's own times the setting, then the game's own again, so whatever the game sets stays its value.
#include "game/game.h"
#include "hook.h"
#include "macho.h"
#include "settings.h"

namespace game {

namespace {

constexpr u32 kFontScale = 0x50;
GuestAddr g_display_original = 0;

void on_display_subtitles(cpu::Thread& t) {
    GuestAddr manager = t.x(0);
    float& scale = gptr<float>(manager + kFontScale)[0];
    float own = scale;
    scale = own * settings::get().subtitle_size / 100.0f;
    t.call(g_display_original, {t.x(0), t.x(1), t.x(2)});
    scale = own;
}

}  // namespace

void install_subtitles(const macho::Image& img) {
    if (is_ib2() || settings::get().subtitle_size == 100) return;
    GuestAddr display = img.find("__ZN16FSubtitleManager16DisplaySubtitlesEP7FCanvasR8FIntRect");
    if (!display) {
        LOG_WARN("settings: subtitle drawing not found; subtitles keep their size");
        return;
    }
    g_display_original = hook::install(display, "FSubtitleManager::DisplaySubtitles", on_display_subtitles);
    LOG_INFO("settings: subtitles at %d%%", settings::get().subtitle_size);
}

}  // namespace game
