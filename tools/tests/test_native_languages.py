"""Compile the production language entry points and test real ONS2 scripts."""
import argparse
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]


def decode(path, engine):
    data = path.read_bytes()
    if data[:4] != b'ONS2':
        raise ValueError(f'Expected an ONS2 script: {path}')
    compressed, length, version = struct.unpack_from('<III', data, 4)
    assert compressed == len(data) - 16 and version == 110
    table = [int(n, 16) for n in re.search(
        r'CompressedConversionTable\[256\]\{(.*?)\};', engine, re.S)[1].split(',') if n.strip()]
    assert len(table) == 256
    def transform(payload, name):
        a, b = (int(n.strip(), 0) for n in re.search(name + r'\[2\]\{([^}]+)\}', engine)[1].split(','))
        return payload.translate(bytes(table[n ^ a] ^ b for n in range(256)))
    result = transform(zlib.decompress(transform(data[16:], 'CompressedCrcA')), 'CompressedCrcB')
    assert len(result) == length
    return result.replace(b'\r', b'') + b'\n'


def check_generator(source, reference):
    sys.path.insert(0, str(ROOT / 'tools'))
    from prepare_cn_assets import build_script, encode_script, script_order, translate_files
    from functools import cmp_to_key

    raw = build_script(source)
    assert raw == build_script(source), 'Script generation is not reproducible'
    assert b';gameid UminekoPS3ficationCn' in raw
    assert b'stralias localisation,"cn"' in raw
    for episode in range(1, 9):
        for label in (f'umi{episode}_op', f'ura_{episode}_end'):
            assert re.search(rb'^\*' + label.encode() + rb'\s*$', raw, re.M), label
    encoded = encode_script(raw)
    assert encoded == encode_script(raw), 'ONS2 encoding is not reproducible'
    engine = (ROOT / 'upstream/onscripter-ru/Engine/Handlers/Script.cpp').read_text()
    with tempfile.TemporaryDirectory(prefix='umineko-generator-') as temporary:
        directory = Path(temporary)
        binary = directory / 'cn.file'
        binary.write_bytes(encoded)
        assert decode(binary, engine) == raw.replace(b'.txt', b'.file').replace(b'\r', b'') + b'\n'
        if reference:
            normalise = lambda data: re.sub(rb'(?m)^numalias releasedate,[0-9]+', b'numalias releasedate,DATE', data)
            assert normalise(raw) == normalise((reference / 'cn.txt').read_bytes()), 'PHP text mismatch'
            assert normalise(decode(binary, engine)) == normalise(decode(reference / 'cn.file', engine)), 'PHP ONS2 content mismatch'

        game, japanese, chinese = (directory / name for name in ('game', 'jp', 'cn'))
        for folder in (game, japanese, chinese):
            folder.mkdir()
            (folder / '._ignore.txt').write_bytes(b'Not a script')
        originals = {'umi1_2.txt': b'`a`\n`b`', 'umi1_10.txt': b'`old`', 'umi1_op.txt': b'`opening`'}
        translations = {'a.txt': ' \t\u3000new\u3000 \r\n'.encode(), 'b.txt': b'`b`\n`c`', 'c.txt': b'`intro`'}
        for name, data in originals.items():
            (japanese / name).write_bytes(data)
        for name, data in translations.items():
            (chinese / name).write_bytes(data)
        for name, data in {'umi1_2.txt': b'`a` `b` `a`', 'umi1_10.txt': b'`old` `old`', 'umi1_op.txt': b'`opening`'}.items():
            (game / name).write_bytes(data)
        assert translate_files(game, japanese, chinese) == '`intro`\n`b` `c` `a`\n`\u3000new\u3000` `old`\n'.encode()
        (game / 'umi1_10.txt').write_bytes(b'{c:86EF9C:`old`}[gstg 1]')
        (chinese / 'a.txt').write_bytes(b'`new`[gstg 2]')
        assert translate_files(game, japanese, chinese, grim=True).endswith(b'`new`[gstg 2]\n')
        names = ['umi1_10.txt', 'umi1_2.txt', 'umi1_02.txt', 'umi1_op.txt']
        assert sorted(names, key=cmp_to_key(script_order)) == ['umi1_op.txt', 'umi1_02.txt', 'umi1_2.txt', 'umi1_10.txt']
    print('Verified reproducible script generation, ONS2 round-trip and replacement edge cases'
          + ('; PHP text and decoded payload match' if reference else ''))


def check(paths):
    loader = (ROOT / 'upstream/onscripter-ru/Engine/Core/Loader.cpp').read_text()
    core = (ROOT / 'upstream/onscripter-ru/Engine/Core/ONScripter.cpp').read_text()
    handler = (ROOT / 'upstream/onscripter-ru/Engine/Handlers/Script.cpp').read_text()
    select = loader.split('parseOptions(ons.argc, ons.argv, hasArchivePath);\n#ifdef VITA\n', 1)[1].split('\n#endif', 1)[0]
    start = loader.index('\tons.setSavePath(VitaPlatform::saveRoot());\n\t// Translation scripts')
    saves = loader[start:loader.index('\t// The installed optimized asset set', start)]
    start = core.index('\tscript_list.emplace_back("en.file");')
    listing = core[start:core.index('\n#else', start)]
    start = handler.index('\tconst bool japanesePack =\n')
    end = handler.index('\tif (VitaSettings::apply(', start)
    flags = handler[start:end]
    apply = re.search(r'if \((VitaSettings::apply\([^\n]+\))\)', handler[end:])[1]
    start = handler.index('const char *ScriptHandler::getSavePath(')
    save_reader = handler[start:handler.index('\n}\n', start) + 3]
    baseline = subprocess.check_output(
        ['git', 'show', 'v1.4:platform/vita/settings_script.hpp'], cwd=ROOT, text=True)

    harness = r'''
#include "platform/vita/settings_script.hpp"
#include "baseline.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
enum class FileType { File };
namespace VitaPlatform {
const char *gameRoot() { return "game/"; }
const char *saveRoot() { return "game/save/"; }
}
namespace FileIO {
std::set<std::string> files, invalid;
std::string relative(std::string name) {
    if (name.compare(0, 5, "game/") == 0) name.erase(0, 5);
    return name;
}
bool accessFile(const std::string &name, FileType) { return files.count(relative(name)); }
bool accessFile(const char *name, const char *root, FileType type) {
    return accessFile(std::string(root) + name, type);
}
}
struct ScriptHandler {
    const char *save_path = nullptr;
    const char *savedir = nullptr;
    const char *getSavePath(const char *filename);
    static bool isScript(const std::string &name) {
        return FileIO::files.count(FileIO::relative(name)) && !FileIO::invalid.count(FileIO::relative(name));
    }
} script_h;
bool isScript(const std::string &name) { return script_h.isScript(name); }
bool equalstr(const char *a, const char *b) { return std::strcmp(a, b) == 0; }
void copystr(char *to, const char *from, size_t size) { std::snprintf(to, size, "%s", from); }
''' + save_reader + r'''
struct Engine {
    bool script_is_set = false;
    char game_script[64]{};
    const char *script_path = "game/";
    std::map<std::string, std::string> ons_cfg_options;
    std::string save;
    void setSavePath(const char *path) { save = path; script_h.save_path = save.c_str(); }
} ons;
const char *script_path = "game/";
std::vector<std::string> script_list;
void selectScript() {
''' + select + r'''
}
void setSavePath() {
''' + saves + r'''
}
void listScripts() {
''' + listing + r'''
}
bool patchMenu(std::string &text) {
    char *script_buffer = text.data();
    size_t script_buffer_length = text.size();
''' + flags + '\nreturn ' + apply + r''';
}
const char *scripts[]{"jp.file", "pt.file", "es.file", "cn.file"};
const char *configs[]{"jp.cfg", "pt.cfg", "es.cfg", "cn.cfg"};
const char *markers[]{"jp/fonts_jp/default.ttf", "language_pt/graphics/system/wnd/msgwnd_pt.png",
                      "language_es/graphics/system/loading_es.png", "language_cn/fonts_cn/default.ttf"};
void install(unsigned mask) {
    FileIO::files = {"en.file"}; FileIO::invalid.clear(); ons = Engine{}; script_list.clear();
    for (unsigned i = 0; i < 4; ++i) if (mask & (1u << i))
        for (const char *file : {scripts[i], configs[i], markers[i]}) FileIO::files.insert(file);
}
std::string body(const VitaSettings::ScriptIndex &index, const char *label) {
    size_t offset; std::string result;
    assert(VitaSettings::body(index, label, offset, result)); return result;
}
int main(int argc, char **argv) {
    for (unsigned mask = 0; mask < 16; ++mask) {
        install(mask); listScripts();
        std::vector<std::string> expected{"en.file"};
        for (unsigned i = 0; i < 4; ++i) if (mask & (1u << i)) expected.emplace_back(scripts[i]);
        assert(script_list == expected);
        selectScript();
        assert(ons.game_script == (expected.size() == 2 ? expected[1] : expected[0]));
        assert(ons.ons_cfg_options.at("game-script") == ons.game_script);
        for (const auto &script : expected) {
            ons.script_is_set = true; copystr(ons.game_script, script.c_str(), sizeof(ons.game_script));
            selectScript(); assert(ons.game_script == script);
        }
    }
    for (const char *missing : {scripts[3], configs[3], markers[3]}) {
        install(8); FileIO::files.erase(missing); listScripts(); selectScript();
        assert(script_list == std::vector<std::string>{"en.file"});
        assert(equalstr(ons.game_script, "en.file"));
    }
    install(8); FileIO::invalid.insert("cn.file"); listScripts(); selectScript();
    assert(script_list == std::vector<std::string>{"en.file"});
    assert(equalstr(ons.game_script, "en.file"));
    for (const auto &entry : std::map<std::string, std::string>{
         {"en.file", ""}, {"jp.file", "jp/"}, {"jp_blank.file", "jp/"},
         {"pt.file", "pt/"}, {"es.file", "es/"}, {"cn.file", "cn/"}}) {
        copystr(ons.game_script, entry.first.c_str(), sizeof(ons.game_script)); setSavePath();
        assert(ons.save == std::string(VitaPlatform::saveRoot()) + entry.second);
        assert(std::string(script_h.getSavePath("save0.dat")) == ons.save);
        assert(std::string(script_h.getSavePath("caches/native-font-cache.txt")) == ons.save);
    }
    for (int arg = 1; arg < argc; ++arg) {
        std::ifstream input(argv[arg]); std::string original((std::istreambuf_iterator<char>(input)), {});
        assert(!original.empty());
        std::vector<std::pair<size_t, std::string>> labels;
        for (size_t line = 0; line < original.size();) {
            const auto end = original.find('\n', line);
            if (end == original.npos) break;
            const auto first = original.find_first_not_of(" \t", line);
            if (first < end && original[first] == '*') labels.emplace_back(first, original.substr(first, end - first));
            line = end + 1;
        }
        const bool chinese = original.find("\nstralias localisation,\"cn\"\n") != original.npos;
        const bool subtitles = original.find("\nstralias set_song_subtitles,") != original.npos;
        for (unsigned mask = 0; mask < 16; ++mask) {
            install(mask); std::string patched = original;
            if (!patchMenu(patched)) {
                std::cerr << argv[arg] << ": menu does not fit, pack mask " << mask << std::endl;
                return 1;
            }
            assert(patched.size() == original.size());
            for (size_t i = 0; i < original.size(); ++i)
                assert((patched[i] == '\n') == (original[i] == '\n'));
            for (const auto &label : labels) assert(patched.compare(label.first, label.second.size(), label.second) == 0);
            if (!chinese && !(mask & 8)) {
                std::string previous = original;
                assert(BaselineSettings::apply(previous.data(), previous.size(), mask & 1, mask & 2, mask & 4));
                const VitaSettings::ScriptIndex index(patched.data(), patched.size());
                const auto first = VitaSettings::label(index, "settings");
                const auto last = VitaSettings::label(index, "settings_elems");
                assert(patched.compare(0, first, previous, 0, first) == 0);
                assert(patched.compare(last, patched.npos, previous, last, previous.npos) == 0);
            }
            const VitaSettings::ScriptIndex index(patched.data(), patched.size());
            const auto elements = body(index, "settings_elems");
            assert((elements.find("lsp 315,set_song_subtitles") != elements.npos) == subtitles);
            assert((elements.find("lsp 314,set_game_language") != elements.npos) == bool(mask));
            if (mask) {
                const auto settings = body(index, "settings");
                const auto languages = body(index, "settings_game_language");
                const auto count = unsigned(bool(mask & 1)) + unsigned(bool(mask & 2)) + unsigned(bool(mask & 4));
                assert(settings.find("mov %Free2," + std::to_string(count + unsigned(bool(mask & 8)))) != settings.npos);
                assert(settings.find("operate_config read,$Free3,\"game-script\"") != settings.npos);
                assert(settings.find("for %Free5=%Free2 to 1 step -1") != settings.npos);
                assert(settings.find("getscriptpath $Free4,%Free5") != settings.npos);
                assert(settings.find("if $Free3=$Free4 break") != settings.npos);
                assert(settings.find("%Free6") == settings.npos);
                assert((languages.find("ZH-CN") != languages.npos) == bool(mask & 8));
                assert(languages.find(subtitles ? ",922" : ",748") != languages.npos);
                const auto loopStart = VitaSettings::label(index, "settings_loop");
                const auto loop = index.substr(loopStart, VitaSettings::label(index, "settings_move") - loopStart);
                assert(loop.find("operate_config write,$Free4,\"game-script\"") != loop.npos);
                assert(loop.find("getscriptpath $Free4,%Free5") != loop.npos);
                assert(loop.find("set_restarted_apply") != loop.npos);
            }
        }
        std::cout << argv[arg] << ": 16 pack combinations, stable save offsets and language switching verified" << std::endl;
    }
}
'''
    with tempfile.TemporaryDirectory(prefix='umineko-languages-') as temporary:
        directory = Path(temporary)
        (directory / 'baseline.hpp').write_text(baseline.replace('namespace VitaSettings {', 'namespace BaselineSettings {'))
        source = directory / 'test.cpp'
        source.write_text(harness)
        binary = directory / 'test'
        sanitizers = os.environ.get('SANITIZERS', 'address,undefined')
        subprocess.run([*shlex.split(os.environ.get('CXX', 'c++')), '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                        f'-fsanitize={sanitizers}', '-fno-omit-frame-pointer', '-I', str(ROOT),
                        str(source), '-o', str(binary)], check=True)
        fixtures = []
        for index, path in enumerate(paths):
            fixture = directory / f'{index}-{path.stem}.txt'
            fixture.write_bytes(decode(path, handler))
            fixtures.append(str(fixture))
        subprocess.run([str(binary), *fixtures], check=True, timeout=180)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, help='Also check the pinned local umineko-scripting checkout')
    parser.add_argument('--reference', type=Path, help='Original PHP output directory containing cn.txt and cn.file')
    parser.add_argument('scripts', type=Path, nargs='+', help='Real en/jp/pt/es/cn.file scripts')
    args = parser.parse_args()
    if args.reference and not args.source:
        parser.error('--reference requires --source')
    if args.source:
        check_generator(args.source, args.reference)
    check(args.scripts)
