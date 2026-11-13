<!-- SPDX-License-Identifier: OFL-1.1 -->

# Bundled fonts

Embedded in `vss_gui` through `src/theme/theme_resources.qrc` and registered by `Theme` before it resolves its fonts.
Each licence text (SIL Open Font License 1.1) sits beside the files. No CJK font is bundled.

| File | Family | Use | Source |
|---|---|---|---|
| `IBMPlexMono-Regular.ttf` | IBM Plex Mono 2.005 | Fixed font: hex, bytes, monitor | https://github.com/IBM/plex |
| `NotoSansHebrew-Regular.ttf`, `NotoSansHebrew-SemiBold.ttf` | Noto Sans Hebrew 3.001 | Hebrew and Yiddish glyphs the system font lacks | https://github.com/notofonts/hebrew |
| `NotoSansArabic-Regular.ttf`, `NotoSansArabic-Bold.ttf` | Noto Sans Arabic 2.012 | Arabic glyphs the system font lacks | https://github.com/notofonts/arabic |

The Arabic files are static instances (weight 400 and 700, width 100) of the variable Noto Sans Arabic, cut with fontTools:

```
python -m fontTools varLib.instancer NotoSansArabic.ttf wght=400 wdth=100 -o regular.ttf
python -m fontTools.subset regular.ttf --unicodes="U+0020-007E,U+00A0,U+00D7,U+2010-2015,U+2018-201F,U+2026,U+0600-06FF,U+0750-077F,U+08A0-08FF,U+FB50-FDFF,U+FE70-FEFF,U+200B-200F,U+2066-2069,U+25CC" --layout-features='*' --glyph-names --notdef-outline --name-IDs='*' --no-hinting --desubroutinize --output-file=NotoSansArabic-Regular.ttf
```
