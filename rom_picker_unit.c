/* Build glue: compile the pd-rom-picker submodule's source via the include path
 * (UINCDIR = pd-rom-picker/src) rather than copying the file or putting a path in
 * SRC. The device build (gcc + VPATH) and the simulator build (clang, which gets
 * the SRC names verbatim) both find this root file, and the #include resolves the
 * submodule source through -I. The submodule stays unmodified. */
#include "rom_picker.c"
