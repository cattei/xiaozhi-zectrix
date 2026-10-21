# Built-in fonts for the Zectrix board

## font_noto_sans_basic_24_1.c

- 内容：24 px、1 bpp（无抗锯齿）中文位图字体，覆盖 5988 个常用汉字与标点
  （字符集取自 `78__xiaozhi-fonts/charsets/common.json`，与字体 cmap 取交集）。
- 来源：由 [Noto Sans SC](https://fonts.google.com/noto/specimen/Noto+Sans+SC) 生成
  （**Medium / wght=500**，用 fontTools 从可变字体实例化）。
- 为什么用 24 px + Medium：本面板是 1 bpp，二值化会吃掉抗锯齿的灰阶。
  18 px 的 Regular 笔画只有 1 px，二值化后断断续续（"细"）；SemiBold 又会粘在一起。
  24 px 时笔画有 2 px、字内空间也更大，二值化后仍然可读。
- 许可证：Noto Sans SC 采用 SIL Open Font License 1.1（OFL-1.1）。
- 重新生成（需要 Node.js）：

  ```bash
  python -m pip install fonttools
  python - <<'PY2'
  from fontTools.ttLib import TTFont
  from fontTools.varLib import instancer
  f = TTFont("NotoSansSC-VF.ttf")
  instancer.instantiateVariableFont(f, {"wght": 500}).save("NotoSansSC-Medium.ttf")
  PY2

  npx --yes lv_font_conv@1.5.3 --no-compress --no-prefilter     --force-fast-kern-format --font NotoSansSC-Medium.ttf --autohint-off     --symbols "<common.json 与字体 cmap 的交集字符>"     --format lvgl --bpp 1 --size 24     --lv-font-name font_noto_sans_basic_24_1 -o font_noto_sans_basic_24_1.c
  ```

> 字体名格式为 `font_<family>_<size>_<bpp>`，`main/CMakeLists.txt` 会从中解析
> 字号和 bpp，所以新增字体族不需要改 CMake 逻辑。

## font_noto_emoji_96_1.c

- 内容：96 px、1 bpp（无抗锯齿）单色线条表情位图，覆盖 21 个情绪表情。
- 来源：由 [Noto Emoji](https://fonts.google.com/noto/specimen/Noto+Emoji)
  生成（单色 glyf 轮廓，**非**彩色版本）。
- 许可证：SIL Open Font License 1.1（OFL-1.1），与原字体一致；
  原始字体版权归 Google LLC 所有。
- 重新生成：

  ```bash
  # Noto Emoji 的 woff2 可从 @fontsource/noto-emoji 获取，先转成 ttf：
  python -m pip install fonttools brotli
  python - <<'PY'
  from fontTools.ttLib import TTFont
  f = TTFont("noto-emoji-emoji-400-normal.woff2"); f.flavor = None
  f.save("NotoEmoji-Regular.ttf")
  PY

  npx --yes lv_font_conv@1.5.3 --no-compress --no-prefilter     --force-fast-kern-format --font NotoEmoji-Regular.ttf     --range 0x1f602,0x1f606,...     --format lvgl --bpp 1 --size 96     --lv-font-name font_noto_emoji_96_1 -o font_noto_emoji_96_1.c
  ```

> 表情字体通过 `BUILTIN_EMOJI_FONT` 按板卡选择（默认 `font_noto_emoji_30_4`，
> 与上游一致）；本板卡用 `font_noto_emoji_96_1`。
