#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include "base/types.h"

#define FONT_BOLD       0x0001
#define FONT_ITALIC     0x0002
#define FONT_UNDERLINE  0x0004
#define FONT_STRIKEOUT  0x0008

class FontSys {
  struct FontStruct {
    HFONT font;
    std::string face;
    int size;
    int flags;
    FontStruct() : font(nullptr) {}
    FontStruct(LOGFONT const& lf);
    FontStruct(FontStruct&& fs)
      : font(fs.font)
      , face(std::move(fs.face))
      , size(fs.size)
      , flags(fs.flags)
    {
      fs.font = nullptr;
    }
	FontStruct &operator=(FontStruct &&fs) {
		if (&fs != this) {
			font = fs.font;
			face = std::move(fs.face);
			size = fs.size;
			flags = fs.flags;
			fs.font = nullptr;
		}
		return *this;
	}
	FontStruct(FontStruct const& fs) = delete;
    ~FontStruct() {
      if (font) DeleteObject(font);
    }
  };
  friend bool operator<(FontSys::FontStruct const& lhs, FontSys::FontStruct const& rhs);
  friend bool operator==(FontSys::FontStruct const& lhs, FontSys::FontStruct const& rhs);
  std::vector<FontStruct> fonts;
  int logPixelsY;
  HDC hDC;
  FontSys();
  std::map<std::string, HFONT> byName;
  static FontSys instance;
  HFONT _getFont(int height, std::string const& face, int flags = 0);
public:
  ~FontSys();

  static HFONT getSysFont();
  static HFONT getFont(int size, std::string const& face, int flags = 0);
  static HFONT getFont(int size, int flags = 0);
  static HFONT changeSize(int size, HFONT oldFont = NULL);
  static HFONT changeFlags(int flags, HFONT oldFont = NULL);
  static void setFontName(HFONT hFont, std::string const& name);
  static HFONT getFontByName(std::string const& name);

  static HFONT getFont(LOGFONT const& lf);

  static void getLogFont(LOGFONT* lf, int size, std::string const& face, int flags = 0);

  static int getFlags(HFONT font = NULL);

  static SIZE getTextSize(HFONT font, std::string const& text);
  static SIZE getTextSize(HFONT font, char const* text, int length);
  static int getMTextHeight(HFONT font, int width, std::string const& text);
};
