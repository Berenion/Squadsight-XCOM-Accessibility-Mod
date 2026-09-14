// UE3 runtime structures for XCOM: Enemy Unknown / Enemy Within (engine 845).
//
// Every offset here was derived from the shipped executables rather than
// assumed -- see tools/native_table.py and the disassembly notes below.
//
// FFrame layout, read straight out of the inlined P_GET_* macros in
// AUI_FxsPanel::execInvoke:
//
//     mov esi, [esp+0x44]            ; FFrame& Stack  (first stack arg)
//     mov eax, [esi + 0x18]          ; Stack.Code
//     mov ecx, [esi + 0x14]          ; Stack.Object
//     mov edx, [edx*4 + GNatives]    ; dispatch
//
// and from the ActionScript call helper, which reads the *caller's* frame:
//
//     mov ecx, [edi + 0x10]          ; Stack.Node
//
// Node/Object/Code are therefore 0x10/0x14/0x18, which puts Locals at 0x1C
// under the stock UE3 FFrame layout.  RUNTIME_VERIFY_FFRAME re-checks that
// at injection time instead of trusting it.

#pragma once
#include <windows.h>
#include <stdint.h>

#define FFRAME_NODE    0x10
#define FFRAME_OBJECT  0x14
#define FFRAME_CODE    0x18
#define FFRAME_LOCALS  0x1C

// UE3 FString and TArray are both { void* Data; int Num; int Max; }.
// Confirmed by the 12-byte zero-init blocks preceding each P_GET_ in
// execInvoke ([esp+0x10..0x18] for the FString, [esp+0x1c..0x24] for the
// TArray of ASValue).
typedef struct {
    wchar_t* Data;
    int32_t  Num;
    int32_t  Max;
} FString;

typedef struct {
    void*   Data;
    int32_t Num;
    int32_t Max;
} FArray;

// Native exec functions are __thiscall:
//     void UObject::execFoo(FFrame& Stack, void* Result)
// which we express as __fastcall with an unused edx slot.
typedef void(__fastcall* ExecFn)(void* self, void* edx, void* stack, void* result);
