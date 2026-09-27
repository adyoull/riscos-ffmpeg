/* Host fake of <swis.h>: the SWI numbers the tests meet. */
#ifndef FAKE_SWIS_H
#define FAKE_SWIS_H
#define OS_Byte             0x06
#define OS_ReadVduVariables 0x31
#define OS_ReadMonotonicTime 0x42
#define Wimp_Initialise     0x400C0
#define Wimp_CreateWindow   0x400C1
#define Wimp_OpenWindow     0x400C5
#define Wimp_CloseWindow    0x400C6
#define Wimp_Poll           0x400C7
#define Wimp_RedrawWindow   0x400C8
#define Wimp_GetRectangle   0x400CA
#define Wimp_DeleteWindow   0x400C3
#define Wimp_ForceRedraw    0x400D1
#define Wimp_SetCaretPosition 0x400D2
#define Wimp_ProcessKey     0x400DC
#define Wimp_CloseDown      0x400DD
#define Wimp_ReadSysInfo    0x400F2
#endif
