# cmake -DIN=file -DOUT=file.c -DNAME=symbol -P embed.cmake
# Writes `const unsigned char NAME[]` and `NAME_size` holding the bytes of IN.
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" len)
math(EXPR size "${len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x..,){32})" "\\1\n" bytes "${bytes}")
file(WRITE "${OUT}" "// generated from ${IN} by cmake/embed.cmake\nconst unsigned char ${NAME}[] = {\n${bytes}\n};\nconst unsigned int ${NAME}_size = ${size};\n")
