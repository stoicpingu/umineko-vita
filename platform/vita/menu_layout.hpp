#pragma once
#include <string_view>

namespace VitaMenu {
enum class TitlePart { None, Button, Description, Logo, PauseButton, TitleConfirmation };
inline TitlePart titlePart(const char *filename) {
    if (!filename) return TitlePart::None;
    const std::string_view path(filename);
    if (path.find("graphics/menu") == path.npos && path.find("graphics\\menu") == path.npos)
        return TitlePart::None;
    const auto base = path.substr(path.find_last_of("/\\") + 1);
    if ((path.find("/r_click_menu/") != path.npos || path.find("\\r_click_menu\\") != path.npos) &&
        base.substr(0, 6) == "r_btn_") return TitlePart::PauseButton;
    if ((path.find("/SystemBtn/") != path.npos || path.find("\\SystemBtn\\") != path.npos) &&
        (base == "title_bg.png" || base == "yes.png" || base == "no.png"))
        return TitlePart::TitleConfirmation;
    if (path.find("/title/") != path.npos || path.find("\\title\\") != path.npos) {
        if (base.substr(0, 12) == "title1_text_") return TitlePart::Description;
        if (base == "title1_logo.png") return TitlePart::Logo;
    }
    if ((path.find("/title_menu/") != path.npos || path.find("\\title_menu\\") != path.npos) &&
        base != "yes.png" && base != "no.png" && base != "trophy_btn_n.png" &&
        base != "unlock_kaku_bg.png") return TitlePart::Button;
    return TitlePart::None;
}
inline bool settingsControl(const char *label, int id) {
    if (!label || std::string_view(label).substr(0, 9) != "settings_") return false;
    return id == 103 || (id >= 190 && id <= 233) || (id >= 247 && id <= 265) ||
           id == 268 || id == 272 || id == 276 || id == 277 ||
           (id >= 307 && id <= 318) || id == 380 || id == 381;
}
inline float imageScale(const char *filename, const char *label, int id) {
    switch (titlePart(filename)) {
        case TitlePart::Button: case TitlePart::Description: case TitlePart::PauseButton: return 1.25f;
        case TitlePart::TitleConfirmation:
            return label && std::string_view(label) == "rmenu_title_back" ? 1.6f : 1.0f;
        case TitlePart::Logo: return .65f;
        default:
            if (!settingsControl(label, id)) return 1.0f;
            return id >= 247 && id <= 250 ? 1.0f : 1.2f;
    }
}
template<class Number> inline void titlePosition(const char *filename, Number &x, Number &y, float scale = 1.0f) {
    switch (titlePart(filename)) {
        case TitlePart::Button:
            x = Number(1220 + (x - 1325) * 1.25f);
            y = Number(250 + (y - 424) * 1.25f);
            break;
        case TitlePart::Description: x = 64; y = 300; break;
        case TitlePart::Logo: x = 1220; y = 12; break;
        case TitlePart::PauseButton:
            x = 40; y = Number(20 + (y - 50) * 1.25f); break;
        case TitlePart::TitleConfirmation:
            if (scale == 1.6f) {
                x = Number(960 + (x - 944) * scale);
                y = Number(540 + (y - 484) * scale);
            }
            break;
        default: break;
    }
}
inline int settingsRow(int id, float scriptY) {
    if (id == 307 || (id >= 223 && id <= 233)) return 0;
    if (id == 308 || (id >= 212 && id <= 222)) return 1;
    if (id == 309 || (id >= 201 && id <= 211)) return 2;
    if (id == 311 || (id >= 251 && id <= 261)) return 3;
    if (id == 318 || (id >= 190 && id <= 200)) return 4;
    if (id == 312 || (id >= 247 && id <= 250)) return 5;
    if (id == 315 || id == 272 || id == 276 || id == 277) return 6;
    if (id == 313 || id == 264 || id == 265) return 7;
    // Japanese has no subtitles row and places language in that slot.
    if (id == 314 || id == 268 || id == 380 || id == 381) return scriptY < 800 ? 6 : 8;
    return -1;
}
template<class Number> inline bool settingsPosition(int id, Number &x, Number &y,
                                                    float width, float height) {
    const int row = settingsRow(id, y);
    if (row < 0) return false; // Exit remains aligned by the script.
    y = Number(220 + row * 88 - height / 2);
    if (id >= 307 && id <= 318) x = 80;
    else {
        float center = 1470;
        if ((id >= 190 && id <= 233) || (id >= 251 && id <= 261))
            center = 1140 + (x - 1250) * 1.6f;
        else if (id >= 247 && id <= 250) center = 1120 + (id - 247) * 220;
        else if (id == 276 || id == 380) center = 1160;
        else if (id == 277 || id == 381) center = 1780;
        else if (id == 264) center = 1400;
        else if (id == 265) center = 1640;
        x = Number(center - width / 2);
    }
    return true;
}
}
