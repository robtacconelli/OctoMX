# Classic Windows UI fonts (from Wine)

Copied from the Ubuntu 25.10 package `fonts-wine` (Wine 10.0, `/usr/share/wine/fonts/`).
These are Wine's own free re-implementations (built from Wine's `fonts/*.sfd` sources),
**not** Microsoft's fonts. Licence: **LGPL-2.1-or-later**, Copyright (c) 1993-2022 Wine
project authors (see https://gitlab.winehq.org/wine/wine/-/blob/master/AUTHORS);
full text in `COPYING.LGPL-2.1`. Redistribution together with the app is fine as long as
this notice + licence ship along and the fonts stay replaceable (they are separate files).

| file               | family          | use                                                  |
|--------------------|-----------------|------------------------------------------------------|
| tahoma.ttf         | Tahoma          | metric-compatible Tahoma, WinMX 3.53 UI font, 8 pt  |
| tahomabd.ttf       | Tahoma Bold     | bold variant                                         |
| ms_sans_serif.ttf  | MS Sans Serif   | Win9x dialog font (outline version of the bitmap)    |
| small_fonts.ttf    | Small Fonts     | tiny captions                                        |
| marlett.ttf        | Marlett         | Windows caption-button / checkmark glyphs            |

Usage in Qt: `QFontDatabase::addApplicationFont(path)` then
`QApplication::setFont(QFont("Tahoma", 8))`. On Windows the real Tahoma exists anyway.
