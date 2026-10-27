#!/usr/bin/env python3
"""Prepare the same loose-file language addon layout as the Vita PT/ES packs."""
import argparse
from functools import cmp_to_key
import hashlib
import re
import shutil
import struct
import subprocess
import uuid
import zlib
from pathlib import Path

from PIL import Image
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
CN_SOURCE = '162a77eb764f181236b8044d1f75a93458ea4937'
PHP_WHITESPACE = b' \t\n\r\x00\x0b'


def script_order(left, right):
    # Match the update manager's opening-first strnatcmp ordering.
    if left[:4] == right[:4] and ('op' in left) != ('op' in right):
        return -1 if 'op' in left else 1
    left, right = re.split(r'([0-9]+)', left), re.split(r'([0-9]+)', right)
    for a, b in zip(left, right):
        if a == b:
            continue
        if a.isdigit() and b.isdigit() and not (a.startswith('0') or b.startswith('0')):
            a, b = int(a), int(b)
        return (a > b) - (a < b)
    return (len(left) > len(right)) - (len(left) < len(right))


def translate_files(game, japanese, chinese, grim=False):
    def files(directory):
        return sorted(p for p in directory.glob('*.txt') if not p.name.startswith('._'))
    # PHP pairs the lexically sorted lists before ordering the resulting pairs.
    pairs = list(zip(files(japanese), files(chinese), strict=True))
    pairs.sort(key=cmp_to_key(lambda a, b: script_order(a[0].name, b[0].name)))
    guard = uuid.uuid4().hex.encode('ascii')
    blocks = []
    for original, translation in pairs:
        before = original.read_bytes().strip(PHP_WHITESPACE)
        after = translation.read_bytes().strip(PHP_WHITESPACE)
        text = (game / original.name).read_bytes().strip(PHP_WHITESPACE)
        if grim:
            text = re.sub(rb'\[gstg [0-9]+\]', b'', text)
            text = re.sub(rb'\{c:86EF9C:(.*?)\}', rb'\1', text)
        if before and after and text:
            rows = after.split(b'\n')
            for index, line in enumerate(before.split(b'\n')):
                line = line.strip(PHP_WHITESPACE)
                replacement = rows[index].strip(PHP_WHITESPACE)
                tag = re.search(rb'\[gstg [0-9]+\]', replacement) if grim else None
                if grim:
                    replacement = re.sub(rb'\[gstg [0-9]+\]', b'', replacement)
                replacement = b'`' + guard + replacement.removeprefix(b'`')
                if not replacement.endswith(b'`'):
                    replacement += b'`'
                if tag:
                    replacement += tag[0]
                text = text.replace(line, replacement, 1)
        blocks.append(text.replace(guard, b'') + b'\n')
    return b''.join(blocks)


def build_script(source):
    revision, timestamp = subprocess.check_output(
        ['git', '-C', str(source), 'show', '-s', '--format=%H %ct', 'HEAD'], text=True).split()
    if revision != CN_SOURCE:
        raise ValueError(f'Expected umineko-scripting at {CN_SOURCE}, got {revision}')
    subprocess.run(['git', '-C', str(source), 'diff', '--quiet', 'HEAD'], check=True)
    if (source / 'current/last_cn').read_bytes().strip(PHP_WHITESPACE) != b'8':
        raise ValueError('Expected all eight Chinese episodes')
    header = (source / 'script/umi_hdr.txt').read_bytes().replace(b'\r\n', b'\n') + b'\n'
    for name, value in ((b'id', b'UminekoPS3ficationCn'), (b'date', timestamp.encode('ascii')),
                        (b'localisation', b'cn'), (b'version', b'8.3b')):
        header = header.replace(b'builder_' + name, value)
    blocks = [header]
    for episode in range(1, 9):
        story = source / f'story/ep{episode}'
        blocks.append(translate_files(source / 'game/main', story / 'jp', story / 'cn', grim=True))
    story = source / 'story/omake'
    blocks.append(translate_files(source / 'game/omake', story / 'jp', story / 'cn'))
    blocks.append((source / 'script/cn/umi_ftr.txt').read_bytes().replace(b'\r\n', b'\n'))
    script = b''.join(blocks)
    while match := re.search(rb'#locale_import "([a-z0-9_]+\.txt)"', script, re.I):
        content = (source / 'script/cn' / match[1].decode('ascii')).read_bytes().replace(b'\r\n', b'\n')
        script = script[:match.start()] + content + script[match.end():]
    if b'#locale_import' in script:
        raise ValueError('Unresolved locale import')
    return script


def encode_script(script):
    # Invert the engine's decoder rather than duplicate its conversion table.
    engine = (ROOT / 'upstream/onscripter-ru/Engine/Handlers/Script.cpp').read_text()
    values = re.search(r'CompressedConversionTable\[256\]\{(.*?)\};', engine, re.S)[1]
    table = [int(value.strip(), 0) for value in values.split(',') if value.strip()]
    def inverse(name):
        a, b = (int(value.strip(), 0) for value in re.search(name + r'\[2\]\{([^}]+)\}', engine)[1].split(','))
        result = bytearray(256)
        for value in range(256):
            result[table[value ^ a] ^ b] = value
        return bytes(result)
    script = script.replace(b'.txt', b'.file')
    payload = zlib.compress(script.translate(inverse('CompressedCrcB')))
    payload = payload.translate(inverse('CompressedCrcA'))
    return struct.pack('<4sIII', b'ONS2', len(payload), len(script), 110) + payload


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def prepare(pc, source, output):
    language = pc / 'language_cn'
    images = sorted(p for p in (language / 'graphics').rglob('*.png') if not p.name.startswith('._'))
    fonts = sorted(p for p in (language / 'fonts_cn').iterdir()
                   if re.fullmatch(r'(default|font[1-9])\.(ttf|otf)', p.name))
    if not images or {p.stem for p in fonts} != {'default', *(f'font{i}' for i in range(1, 10))}:
        raise ValueError('Expected the complete PC language_cn graphics and ten font slots.')
    if (source / 'misc/cn.cfg').read_text().splitlines() != ['lang-dir=language_cn', 'font-dir=fonts_cn']:
        raise ValueError('Expected the original SNS Team cn.cfg.')
    for font in fonts:
        if not 0 < font.stat().st_size <= 32 * 1024 * 1024:
            raise ValueError(f'Font exceeds the Vita loader size limit: {font.name}')
        with TTFont(font) as face:
            if not face.getBestCmap():
                raise ValueError(f'Font has no Unicode mapping: {font.name}')
    script = encode_script(build_script(source))

    output.mkdir(parents=True, exist_ok=False)
    game = output / 'umineko'
    target = game / 'language_cn'
    (target / 'fonts_cn').mkdir(parents=True)
    sizes = []
    for font in fonts:
        shutil.copyfile(font, target / 'fonts_cn' / font.name)
    for index, path in enumerate(images, 1):
        relative = path.relative_to(language)
        destination = target / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        with Image.open(path) as image:
            width, height = image.size
            sizes.append(f'{relative.as_posix()}\t{width}\t{height}\n')
            # Match existing Vita packs: half-size storage, original-size metadata.
            # The engine keeps tiny markers/fills at their original physical size.
            if min(width, height) <= 4:
                shutil.copyfile(path, destination)
            else:
                mode = 'RGBA' if 'A' in image.getbands() or 'transparency' in image.info else 'RGB'
                image.convert(mode).resize((max(1, width // 2), max(1, height // 2)),
                                           Image.Resampling.LANCZOS).save(destination, compress_level=9)
        if index % 100 == 0:
            print(f'Converted {index}/{len(images)} PNGs', flush=True)
    (target / 'native-image-sizes.tsv').write_text(''.join(sizes), encoding='utf-8')
    (target / 'render_scale.txt').write_text('0.5000000\n', encoding='ascii')
    (game / 'cn.file').write_bytes(script)
    shutil.copyfile(source / 'misc/cn.cfg', game / 'cn.cfg')
    shutil.copyfile(source / 'LICENSE.txt', output / 'SCRIPT-LICENSE.txt')
    provenance = (
        'PC Chinese assets: https://snsteam.club/download/port\n'
        'Source: official complete PC package with official resource.zip updates applied.\n'
        'Fonts: original default/font1-font9 files, byte-for-byte copies; no substitution or subsetting.\n'
        f'PNG files: {len(images)}; font files: {len(fonts)}.\n'
        'Reference layout: stoicpingu/umineko-vita PT-BR language addon, 2026-10-09.\n'
        'Image storage: 0.5 scale, floor rounding; tiny markers unchanged.\n'
        'native-image-sizes.tsv records the original PC dimensions.\n'
        'Shared backgrounds, sprites, sound and video are supplied by the existing Vita base + Hotfix v2.\n'
        'No VPK, engine files, native preferences, save data or media files are included.\n'
        '\nScript source: https://github.com/Umineko-Project-CN/umineko-scripting\n'
        f'Commit: {CN_SOURCE}\n'
        'Locale: cn; episodes: 1-8 and omake.\n'
        'Generator: tools/prepare_cn_assets.py; matches the original update-manager.php passes.\n'
        'Release timestamp: source commit date.\n'
    )
    (output / 'SOURCE.txt').write_text(provenance, encoding='utf-8')
    (output / 'README.txt').write_text(
        '海猫 Vita 简体中文资源增量包\n\n'
        '结构与现有 Vita 语言插件一致：umineko/cn.file、cn.cfg、language_cn/。\n'
        '使用现有 Vita 底座及 Hotfix v2；中文字体和图片独立放置，不覆盖其他语言。\n'
        '文件最终对应 ux0:data/umineko/；使用支持中文的引擎，在设置中选择 ZH-CN 后重启。\n'
        '本包不含中文视频或歌曲字幕，不含预生成存档缓存。\n'
        '图片、字体和游戏素材的权利保留给各原作者；SCRIPT-LICENSE.txt 仅适用于脚本。\n'
        'SOURCE.txt 记录来源；SHA256SUMS 可用于核对全部文件。\n', encoding='utf-8')
    files = sorted(p for p in output.rglob('*') if p.is_file())
    (output / 'SHA256SUMS').write_text(''.join(f'{sha256(p)}  {p.relative_to(output).as_posix()}\n' for p in files), encoding='utf-8')
    print(f'Prepared {len(images)} PNGs and {len(fonts)} fonts in {output}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pc', type=Path, help='Updated PC game directory containing language_cn/')
    parser.add_argument('source', type=Path, help=f'Local umineko-scripting checkout at {CN_SOURCE}')
    parser.add_argument('output', type=Path, help='A new output directory')
    args = parser.parse_args()
    prepare(args.pc, args.source, args.output)
