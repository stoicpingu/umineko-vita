#!/usr/bin/env python3
"""Prepare the same loose-file language addon layout as the Vita PT/ES packs."""
import argparse
import hashlib
import re
import shutil
from pathlib import Path

from PIL import Image
from fontTools.ttLib import TTFont


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def prepare(pc, scripts, output):
    language = pc / 'language_cn'
    images = sorted((language / 'graphics').rglob('*.png'))
    fonts = sorted(p for p in (language / 'fonts_cn').iterdir()
                   if re.fullmatch(r'(default|font[1-9])\.(ttf|otf)', p.name))
    if not images or {p.stem for p in fonts} != {'default', *(f'font{i}' for i in range(1, 10))}:
        raise ValueError('Expected the complete PC language_cn graphics and ten font slots.')
    for name in ('cn.file', 'cn.cfg', 'LICENSE.txt', 'SOURCE.txt'):
        if not (scripts / name).is_file():
            raise ValueError(f'Missing prepared script input: {name}')
    if (scripts / 'cn.cfg').read_text().splitlines() != ['lang-dir=language_cn', 'font-dir=fonts_cn']:
        raise ValueError('Expected the original SNS Team cn.cfg.')
    for font in fonts:
        if not 0 < font.stat().st_size <= 32 * 1024 * 1024:
            raise ValueError(f'Font exceeds the Vita loader size limit: {font.name}')
        with TTFont(font) as face:
            if not face.getBestCmap():
                raise ValueError(f'Font has no Unicode mapping: {font.name}')

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
    for name in ('cn.file', 'cn.cfg'):
        shutil.copyfile(scripts / name, game / name)
    shutil.copyfile(scripts / 'LICENSE.txt', output / 'SCRIPT-LICENSE.txt')
    source = (
        'PC Chinese assets: https://snsteam.club/download/port\n'
        'Source: official complete PC package with official resource.zip updates applied.\n'
        'Fonts: original default/font1-font9 files, byte-for-byte copies; no substitution or subsetting.\n'
        f'PNG files: {len(images)}; font files: {len(fonts)}.\n'
        'Reference layout: stoicpingu/umineko-vita PT-BR language addon, 2026-10-09.\n'
        'Image storage: 0.5 scale, floor rounding; tiny markers unchanged.\n'
        'native-image-sizes.tsv records the original PC dimensions.\n'
        'Shared backgrounds, sprites, sound and video are supplied by the existing Vita base + Hotfix v2.\n'
        'No VPK, engine files, native preferences, save data or media files are included.\n'
        '\nScript provenance:\n' + (scripts / 'SOURCE.txt').read_text()
    )
    (output / 'SOURCE.txt').write_text(source, encoding='utf-8')
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
    parser.add_argument('scripts', type=Path, help='The prepared simplified Chinese script package')
    parser.add_argument('output', type=Path, help='A new output directory')
    args = parser.parse_args()
    prepare(args.pc, args.scripts, args.output)
