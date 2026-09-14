// UE3 runtime structures for XCOM: Enemy Unknown / Enemy Within (engine 845).
//
// Every offset here was derived from the shipped executables, not assumed.
//
// FFrame, from the inlined P_GET_* macros in AUI_FxsPanel::execInvoke:
//
//     mov esi, [esp+0x44]            ; FFrame& Stack  (first stack arg)
//     mov eax, [esi + 0x18]          ; Stack.Code
//     mov ecx, [esi + 0x14]          ; Stack.Object
//     mov edx, [edx*4 + GNatives]    ; dispatch
//
// and from UObject::execGetFuncName / the ActionScript call helper:
//
//     mov esi, [esi + 0x10]          ; Stack.Node
//
// Locals is pinned by the helper's parameter fill loop, which is the exact
// walk this DLL reimplements:
//
//     mov eax, [esi + 0x60]          ; UProperty::Offset
//     add eax, [edi + 0x1c]          ; + FFrame::Locals
//     push ebx / push eax / push esi
//     call <property value -> ASValue>
//
// and its preceding count loop gives the rest:
//
//     mov ecx, [eax + 0x4c]          ; UStruct::Children
//     mov eax, [ecx + 0x48]          ; UProperty::PropertyFlags
//     and eax, 0x480                 ; CPF_Parm | CPF_ReturnParm
//     cmp eax, 0x80                  ; a parameter, and not the return value
//     mov edx, [ecx + 0x3c]          ; UField::Next
//
// UObject::Name comes from UObject::execGetFuncName, which returns
// Stack.Node's FName directly:
//
//     mov ecx, [esi + 0x2c]          ; Name.Index
//     mov edx, [esi + 0x30]          ; Name.Number

#pragma once
#include <windows.h>
#include <stdint.h>

// FFrame
#define FFRAME_NODE    0x10   // UStruct*  -- the *calling* function
#define FFRAME_OBJECT  0x14   // UObject*  -- the calling object
#define FFRAME_CODE    0x18   // BYTE*     -- bytecode cursor (never touched)
#define FFRAME_LOCALS  0x1C   // BYTE*     -- caller's evaluated parameters

// UObject
#define UOBJECT_NAME   0x2C   // FName { int Index; int Number; }

// UStruct / UField / UProperty
#define USTRUCT_CHILDREN    0x4C   // UField*
#define UFIELD_NEXT         0x3C   // UField*
#define UPROPERTY_FLAGS     0x48   // QWORD, low dword is enough for CPF_Parm
#define UPROPERTY_OFFSET    0x60   // byte offset into the owner's storage

#define CPF_PARM        0x00000080
#define CPF_RETURNPARM  0x00000400

// UE3 FString and TArray are both { void* Data; int Num; int Max; }.
// Confirmed by the 12-byte zero-init blocks preceding each P_GET_ in
// execInvoke ([esp+0x10..0x18] FString, [esp+0x1c..0x24] TArray<ASValue>).
typedef struct {
    wchar_t* Data;
    int32_t  Num;     // includes the terminating NUL
    int32_t  Max;
} FString;

typedef struct {
    void*   Data;
    int32_t Num;
    int32_t Max;
} FArray;

typedef struct {
    int32_t Index;
    int32_t Number;
} FName;

// Native exec functions are __thiscall:
//     void UObject::execFoo(FFrame& Stack, void* Result)
// expressed as __fastcall with an unused edx slot.
typedef void(__fastcall* ExecFn)(void* self, void* edx, void* stack, void* result);
