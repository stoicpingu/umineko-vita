#pragma once

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace VitaSettings {
// Adapt only the menu in memory. Save files contain absolute script offsets
// and line numbers, so every original byte position, newline and label header
// stays in place. All replacements are validated before touching the buffer.
struct Edit { size_t offset; std::string text; };
// One bounded pass replaces a full-script search for every settings edit.
// Views remain valid because edits are committed only after all validation.
struct ScriptIndex : std::string_view {
    std::unordered_map<std::string_view, size_t> labels;
    ScriptIndex(const char *buffer, size_t length) : std::string_view(buffer, length) {
        for (size_t line = 0; line < size();) {
            size_t end = find('\n', line);
            if (end == npos) break;
            size_t first = line;
            while (first < end && (buffer[first] == ' ' || buffer[first] == '\t')) ++first;
            if (substr(first, 9) == "*settings")
                labels.emplace(substr(first + 1, end - first - 1), end + 1);
            line = end + 1;
        }
    }
};
inline size_t label(const ScriptIndex &script, std::string_view name) {
    auto found = script.labels.find(name);
    return found == script.labels.end() ? script.npos : found->second;
}
inline bool body(const ScriptIndex &script, const char *name, size_t &at, std::string &text) {
    at = label(script, name);
    if (at == script.npos) return false;
    size_t end = at;
    while (end < script.size()) {
        size_t next = script.find('\n', end);
        if (next == script.npos) return false;
        size_t first = script.find_first_not_of(" \t", end);
        if (first <= next && script[first] == '*') break;
        end = next + 1;
    }
    text = std::string(script.substr(at, end - at));
    return !text.empty();
}
inline bool replaceBody(const ScriptIndex &script, std::vector<Edit> &edits,
                        const char *name, std::string_view commands, const char *until = nullptr) {
    Edit edit;
    if (!body(script, name, edit.offset, edit.text)) return false;
    if (until) {
        size_t end = label(script, until);
        if (end == script.npos || end <= edit.offset) return false;
        end = script.rfind('\n', end - 2) + 1;
        edit.text = std::string(script.substr(edit.offset, end - edit.offset));
    }
    for (size_t line = 0; line < edit.text.size();) {
        size_t end = edit.text.find('\n', line);
        if (end == edit.text.npos) return false;
        size_t first = edit.text.find_first_not_of(" \t", line);
        if (first == edit.text.npos || edit.text[first] != '*')
            std::fill(edit.text.begin() + line, edit.text.begin() + end, ' ');
        line = end + 1;
    }
    size_t slot = 0, command = 0;
    while (command < commands.size()) {
        size_t end = commands.find('\n', command);
        if (end == commands.npos) end = commands.size();
        const size_t length = end - command;
        if (length) {
            bool fitted = false;
            while (slot < edit.text.size()) {
                size_t next = edit.text.find('\n', slot);
                if (next == edit.text.npos) return false;
                size_t first = edit.text.find_first_not_of(" \t", slot);
                if (next - slot >= length && (first == edit.text.npos || edit.text[first] != '*')) {
                    edit.text.replace(slot, length, commands.substr(command, length));
                    slot = next + 1;
                    fitted = true;
                    break;
                }
                slot = next + 1;
            }
            if (!fitted) return false;
        }
        command = end + 1;
    }
    edits.push_back(std::move(edit));
    return true;
}
inline bool moveRow(const ScriptIndex &script, std::vector<Edit> &edits,
                    const char *name, const char *before, const char *after) {
    Edit edit;
    if (std::strlen(before) != std::strlen(after) || !body(script, name, edit.offset, edit.text)) return false;
    size_t count = 0;
    for (size_t at = edit.text.find(before); at != edit.text.npos; at = edit.text.find(before, at + std::strlen(after))) {
        edit.text.replace(at, std::strlen(before), after);
        ++count;
    }
    if (!count) return false;
    edits.push_back(std::move(edit));
    return true;
}
inline bool apply(char *buffer, size_t length, bool japanesePack = false, bool portuguesePack = false, bool spanishPack = false) {
    const bool languages = japanesePack || portuguesePack || spanishPack;
    const int portugueseIndex = int(japanesePack) + 1;
    const int spanishIndex = int(japanesePack) + int(portuguesePack) + 1;
    const int lastLanguage = int(japanesePack) + int(portuguesePack) + int(spanishPack);
    const ScriptIndex script(buffer, length);
    const bool japanese = script.find("\nlanguage japanese\n") != script.npos;
    // Restore the pending saved choice when opening Settings. Keep this in
    // the initialization block: older translations have short language
    // helpers whose original line/label offsets must remain unchanged.
    // Compact operators/command spacing fit the older Spanish line slots.
    std::string selection = "operate_config read,$Free3,\"game-script\"\n";
    if (japanesePack) selection += "if $Free3=\"jp.file\"mov%Free5,1\n";
    if (portuguesePack) selection += "if $Free3=\"pt.file\"mov%Free5," + std::to_string(portugueseIndex) + "\n";
    if (spanishPack) selection += "if $Free3=\"es.file\"mov%Free5," + std::to_string(spanishIndex) + "\n";
    // Adapt command strings before fitting them into the unchanged script lines.
    auto commands = [&](const char *section, std::string text) {
        auto replace = [&](const std::string &before, const std::string &after) {
            auto at = text.find(before);
            if (at != text.npos) text.replace(at, before.size(), after);
        };
        if (japanese) {
            replace("lsp 315,set_song_subtitles,135,748\n", "");
            replace("gosub *settings_op_ed_song_subtitles\n", "");
            replace("spbtn 276,76\nspbtn 277,77\n", "");
            auto begin = text.find("if %BtnRes = 76 dec");
            auto end = text.find("print 1", begin);
            if (begin != text.npos && end != text.npos) text.erase(begin, end-begin);
        }
        if (!languages) return text;
        if (std::strcmp(section, "settings_elems") == 0) {
            const auto y = japanese ? "748" : "922";
            replace("print 1", std::string("lsp 314,set_game_language,135,") + y + "\ngosub *settings_game_language\nprint 1");
        }
        if (std::strcmp(section, "settings_loop") == 0) {
            replace("btnwait %BtnRes", "spbtn 380,68\nspbtn 381,69\nbtnwait %BtnRes");
            replace("print 1", R"ONS(if %BtnRes = 68 dec %Free5
if %BtnRes = 69 inc %Free5
if %Free5 < 0 mov %Free5,%Free2
if %Free5 > %Free2 mov %Free5,0
if %BtnRes = 68 getscriptpath $Free4,%Free5 : getscriptpath $Free3,%Free5,1
if %BtnRes = 69 getscriptpath $Free4,%Free5 : getscriptpath $Free3,%Free5,1
if %BtnRes = 68 operate_config write,$Free4,"game-script" : operate_config save
if %BtnRes = 69 operate_config write,$Free4,"game-script" : operate_config save
if %BtnRes = 68 gosub *settings_game_language : lsp 373,set_restarted_apply,0,1000
if %BtnRes = 69 gosub *settings_game_language : lsp 373,set_restarted_apply,0,1000
print 1)ONS");
        }
        return text;
    };
    std::vector<Edit> edits;
    if (!languages && !replaceBody(script, edits, "settings", commands("settings", R"ONS(
trophy_open 10-%CHIRU_MODE
mov %mcurrent_page,1
mov %mcoord_add,0
gosub *update_textspeed
border_pad_push 4
)ONS"))) return false;
    if (languages && !replaceBody(script, edits, "settings", std::string(R"ONS(
trophy_open 10-%CHIRU_MODE
mov %mcurrent_page,1
mov %mcoord_add,0
mov %Free5,0
gosub *update_textspeed
border_pad_push 4
)ONS") + "mov %Free2," + std::to_string(lastLanguage) + "\n" + selection)) return false;
    if (!replaceBody(script, edits, "settings_elems", commands("settings_elems", R"ONS(
lsp 105,config_caption,20,0
lsp 451,":c;graphics\menu\config\bg_config.png",0,0
lsp 440,":a;graphics\menu\SystemBtn\black_buttons.png",0,0
lsp 307,set_bgm_volume,135,226
lsp 308,set_effect_volume,135,313
lsp 309,set_voice_volume,135,400
lsp 311,set_text_speed,135,487
lsp 318,set_automode_speed,135,574
lsp 312,set_textbox_window,135,661
lsp 315,set_song_subtitles,135,748
lsp 313,set_lip_synchronization,135,835
lsp 103,set_exit,0,0
align_buttons_r 103
gosub *settings_bgm_vol
gosub *settings_sfx_vol
gosub *settings_voice_vol
gosub *settings_textspeed
gosub *settings_automode_speed
gosub *settings_textbox
gosub *settings_op_ed_song_subtitles
gosub *settings_lips
print 1
)ONS"))) return false;
    if (!replaceBody(script, edits, "settings_loop", commands("settings_loop", R"ONS(
btndef ""
spbtn 103,373
spbtn 201,1
spbtn 202,2
spbtn 203,3
spbtn 204,4
spbtn 205,5
spbtn 206,6
spbtn 207,7
spbtn 208,8
spbtn 209,9
spbtn 210,10
spbtn 211,11
spbtn 212,12
spbtn 213,13
spbtn 214,14
spbtn 215,15
spbtn 216,16
spbtn 217,17
spbtn 218,18
spbtn 219,19
spbtn 220,20
spbtn 221,21
spbtn 222,22
spbtn 223,23
spbtn 224,24
spbtn 225,25
spbtn 226,26
spbtn 227,27
spbtn 228,28
spbtn 229,29
spbtn 230,30
spbtn 231,31
spbtn 232,32
spbtn 233,33
spbtn 247,47
spbtn 248,48
spbtn 249,49
spbtn 250,50
spbtn 251,51
spbtn 252,52
spbtn 253,53
spbtn 254,54
spbtn 255,55
spbtn 256,56
spbtn 257,57
spbtn 258,58
spbtn 259,59
spbtn 260,60
spbtn 261,61
spbtn 190,160
spbtn 191,161
spbtn 192,162
spbtn 193,163
spbtn 194,164
spbtn 195,165
spbtn 196,166
spbtn 197,167
spbtn 198,168
spbtn 199,169
spbtn 200,170
spbtn 264,64
spbtn 265,65
spbtn 276,76
spbtn 277,77
btnwait %BtnRes
if %BtnRes = -1 goto *settings_end
if %BtnRes = -10 goto *settings_end
if %BtnRes = 373 goto *settings_end
if %BtnRes = 1 mov %dlg_vol,100 : voice_vol : gosub *settings_voice_vol
if %BtnRes = 2 mov %dlg_vol,90 : voice_vol : gosub *settings_voice_vol
if %BtnRes = 3 mov %dlg_vol,80 : voice_vol : gosub *settings_voice_vol
if %BtnRes = 4 mov %dlg_vol,70 : voice_vol : gosub *settings_voice_vol
if %BtnRes = 5 mov %dlg_vol,60 : voice_vol : gosub *settings_voice_vol
if %BtnRes = 6 mov %dlg_vol,50 : voice_vol : gosub *settings_voice_vol
if %BtnRes = 7 mov %dlg_vol,40 : voice_vol : gosub *settings_voice_vol
if %BtnRes = 8 mov %dlg_vol,30 : voice_vol : gosub *settings_voice_vol
if %BtnRes = 9 mov %dlg_vol,20 : voice_vol : gosub *settings_voice_vol
if %BtnRes = 10 mov %dlg_vol,10 : voice_vol : gosub *settings_voice_vol
if %BtnRes = 11 mov %dlg_vol,0 : voice_vol : gosub *settings_voice_vol
if %BtnRes = 12 mov %sfx_vol,100 : effect_vol : gosub *settings_sfx_vol
if %BtnRes = 13 mov %sfx_vol,90 : effect_vol : gosub *settings_sfx_vol
if %BtnRes = 14 mov %sfx_vol,80 : effect_vol : gosub *settings_sfx_vol
if %BtnRes = 15 mov %sfx_vol,70 : effect_vol : gosub *settings_sfx_vol
if %BtnRes = 16 mov %sfx_vol,60 : effect_vol : gosub *settings_sfx_vol
if %BtnRes = 17 mov %sfx_vol,50 : effect_vol : gosub *settings_sfx_vol
if %BtnRes = 18 mov %sfx_vol,40 : effect_vol : gosub *settings_sfx_vol
if %BtnRes = 19 mov %sfx_vol,30 : effect_vol : gosub *settings_sfx_vol
if %BtnRes = 20 mov %sfx_vol,20 : effect_vol : gosub *settings_sfx_vol
if %BtnRes = 21 mov %sfx_vol,10 : effect_vol : gosub *settings_sfx_vol
if %BtnRes = 22 mov %sfx_vol,0 : effect_vol : gosub *settings_sfx_vol
if %BtnRes = 23 mov %bgm_vol,100 : vol_bgm %bgm_current_vol : gosub *settings_bgm_vol
if %BtnRes = 24 mov %bgm_vol,90 : vol_bgm %bgm_current_vol : gosub *settings_bgm_vol
if %BtnRes = 25 mov %bgm_vol,80 : vol_bgm %bgm_current_vol : gosub *settings_bgm_vol
if %BtnRes = 26 mov %bgm_vol,70 : vol_bgm %bgm_current_vol : gosub *settings_bgm_vol
if %BtnRes = 27 mov %bgm_vol,60 : vol_bgm %bgm_current_vol : gosub *settings_bgm_vol
if %BtnRes = 28 mov %bgm_vol,50 : vol_bgm %bgm_current_vol : gosub *settings_bgm_vol
if %BtnRes = 29 mov %bgm_vol,40 : vol_bgm %bgm_current_vol : gosub *settings_bgm_vol
if %BtnRes = 30 mov %bgm_vol,30 : vol_bgm %bgm_current_vol : gosub *settings_bgm_vol
if %BtnRes = 31 mov %bgm_vol,20 : vol_bgm %bgm_current_vol : gosub *settings_bgm_vol
if %BtnRes = 32 mov %bgm_vol,10 : vol_bgm %bgm_current_vol : gosub *settings_bgm_vol
if %BtnRes = 33 mov %bgm_vol,0  : vol_bgm %bgm_current_vol : gosub *settings_bgm_vol
if %BtnRes = 64 mov %animated_lips,1 : gosub *settings_lips
if %BtnRes = 65 mov %animated_lips,0 : lips_channel 0,"non" : lips_channel 1,"non" : lips_channel 2,"non" : lips_channel 43,"non" : lips_channel 44,"non" : lips_channel 45,"non" : lips_channel 46,"non" : gosub *settings_lips
if %BtnRes = 47 mov %msgwnd_type,2 : gosub *settings_textbox
if %BtnRes = 48 mov %msgwnd_type,1 : gosub *settings_textbox
if %BtnRes = 49 mov %msgwnd_type,0 : gosub *settings_textbox
if %BtnRes = 50 mov %msgwnd_type,3 : gosub *settings_textbox
if %BtnRes = 51 mov %textspeed2,0 : gosub *settings_textspeed
if %BtnRes = 52 mov %textspeed2,1 : gosub *settings_textspeed
if %BtnRes = 53 mov %textspeed2,2 : gosub *settings_textspeed
if %BtnRes = 54 mov %textspeed2,3 : gosub *settings_textspeed
if %BtnRes = 55 mov %textspeed2,4 : gosub *settings_textspeed
if %BtnRes = 56 mov %textspeed2,5 : gosub *settings_textspeed
if %BtnRes = 57 mov %textspeed2,6 : gosub *settings_textspeed
if %BtnRes = 58 mov %textspeed2,7 : gosub *settings_textspeed
if %BtnRes = 59 mov %textspeed2,8 : gosub *settings_textspeed
if %BtnRes = 60 mov %textspeed2,9 : gosub *settings_textspeed
if %BtnRes = 61 mov %textspeed2,10 : gosub *settings_textspeed
if %BtnRes = 160 mov %automodespeed,-116 : gosub *settings_automode_speed2
if %BtnRes = 161 mov %automodespeed,-90 : gosub *settings_automode_speed2
if %BtnRes = 162 mov %automodespeed,-70 : gosub *settings_automode_speed2
if %BtnRes = 163 mov %automodespeed,-55 : gosub *settings_automode_speed2
if %BtnRes = 164 mov %automodespeed,-43 : gosub *settings_automode_speed2
if %BtnRes = 165 mov %automodespeed,-33 : gosub *settings_automode_speed2
if %BtnRes = 166 mov %automodespeed,-26 : gosub *settings_automode_speed2
if %BtnRes = 167 mov %automodespeed,-20 : gosub *settings_automode_speed2
if %BtnRes = 168 mov %automodespeed,-16 : gosub *settings_automode_speed2
if %BtnRes = 169 mov %automodespeed,-12 : gosub *settings_automode_speed2
if %BtnRes = 170 mov %automodespeed,-9 : gosub *settings_automode_speed2
if %BtnRes = 76 dec %op_ed_song_subtitles
if %BtnRes = 77 inc %op_ed_song_subtitles
if %op_ed_song_subtitles < 1 mov %op_ed_song_subtitles,4
if %op_ed_song_subtitles > 4 mov %op_ed_song_subtitles,1
if %BtnRes = 76 gosub *settings_op_ed_song_subtitles
if %BtnRes = 77 gosub *settings_op_ed_song_subtitles
print 1
goto *settings_loop
)ONS"), "settings_move")) return false;
    if (!moveRow(script, edits, "settings_textspeed", ",340", ",514") ||
        !moveRow(script, edits, "settings_automode_speed", ",427", ",601") ||
        !moveRow(script, edits, "settings_textbox", ",497", ",671") ||
        !moveRow(script, edits, "settings_lips", ",574", ",835")) return false;
    if (languages) {
        Edit language;
        if (!body(script, "settings_game_language", language.offset, language.text)) return false;
        // Keep the script-specific arrow artwork, replacing only label mapping.
        const auto artwork = language.text.find("lsp 268,");
        if (artwork == language.text.npos) return false;
        std::string commands = "mov $Free3,\"English\"\n";
        if (japanesePack) commands += "if %Free5 = 1 mov $Free3,\"JP\"\n";
        if (portuguesePack) commands += "if %Free5 = " + std::to_string(portugueseIndex) + " mov $Free3,\"PT-BR\"\n";
        if (spanishPack) commands += "if %Free5 = " + std::to_string(spanishIndex) + " mov $Free3,\"Español\"\n";
        commands += language.text.substr(artwork);
        for (size_t at = commands.find(",226"); at != commands.npos; at = commands.find(",226", at + 4))
            commands.replace(at, 4, japanese ? ",748" : ",922");
        if (!replaceBody(script, edits, "settings_game_language", commands)) return false;
    }
    for (const auto &edit : edits) std::memcpy(buffer + edit.offset, edit.text.data(), edit.text.size());
    return true;
}
} // namespace VitaSettings
