/* MinGW builds only: ISPC's Windows objects call MSVC's __chkstk; MinGW's runtime names it ___chkstk_ms */
    .text
    .globl __chkstk
__chkstk:
    jmp ___chkstk_ms
