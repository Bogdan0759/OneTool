#ifndef MLINK_MACRO_H
#define MLINK_MACRO_H

/*
 * MLINK_MACRO(str) emits a NUL-terminated directive into the .mlink.macro
 * section of the object file. mlink concatenates all such directives from
 * every input .o and processes them at link time.
 *
 * Examples:
 *   MLINK_MACRO("ENTRY my_start");
 *   MLINK_MACRO("BASE 0x800000");
 *   MLINK_MACRO("PROVIDE __version = 0x00010203");
 *   MLINK_MACRO("DEFINE  __build_id = 0xc0ffee");
 *   MLINK_MACRO("STACK_EXEC");
 *   MLINK_MACRO("KEEP .init_array");
 *   MLINK_MACRO("ASSERT _end - __executable_start <= 0x100000, \"image too big\"");
 *
 * The section is allocated as plain data with no flags so the linker sees it
 * but it never participates in any output segment.
 */

#define MLINK_MACRO_STR_(x) #x
#define MLINK_MACRO_STR(x) MLINK_MACRO_STR_(x)

#define MLINK_MACRO(str) \
    __asm__(".pushsection .mlink.macro,\"\",@progbits\n" \
            ".asciz \"" str "\"\n" \
            ".popsection\n")

#define MLINK_PROVIDE(name, value) \
    MLINK_MACRO("PROVIDE " #name " = " MLINK_MACRO_STR(value))
#define MLINK_DEFINE(name, value) \
    MLINK_MACRO("DEFINE "  #name " = " MLINK_MACRO_STR(value))
#define MLINK_ENTRY(name)         MLINK_MACRO("ENTRY " #name)
#define MLINK_BASE(addr)          MLINK_MACRO("BASE "  MLINK_MACRO_STR(addr))
#define MLINK_STACK_EXEC()        MLINK_MACRO("STACK_EXEC")
#define MLINK_KEEP(section)       MLINK_MACRO("KEEP " #section)

#endif
