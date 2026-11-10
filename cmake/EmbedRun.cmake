file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}" "extern \"C\" const char *${FN}(void) {\n  static const unsigned char d[] = {${bytes}0x00};\n  return reinterpret_cast<const char *>(d);\n}\n")
