// XPLogin10 - the CLSIDs Windows knows us by.
//
// Declarations only. The definitions live in Guids.cpp, which is the single
// translation unit that includes <initguid.h>. Doing it the other way round
// works by accident (DEFINE_GUID uses __declspec(selectany)) but it also
// defines every other GUID in every header that follows, which is a good way
// to collide with the SDK.
//
// These two values must match the strings in RegistrationPlan.h exactly.
// Guids.cpp asserts that at compile time.
#pragma once

#include <guiddef.h>

// {6E2B1FA0-71D4-4C57-9E4A-3B2D5F8C1A77}
EXTERN_C const GUID CLSID_XPLoginProvider;

// {9C1D3E82-40B6-4F1D-8A57-2E9C6B04D311}
EXTERN_C const GUID CLSID_XPLoginFilter;
