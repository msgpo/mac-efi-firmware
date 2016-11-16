/** @file
  Copyright (C) 2005 - 2015, Apple Inc.  All rights reserved.<BR>

  This program and the accompanying materials have not been licensed.
  Neither is its usage, its redistribution, in source or binary form,
  licensed, nor implicitely or explicitely permitted, except when
  required by applicable law.

  Unless required by applicable law or agreed to in writing, software
  distributed is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES
  OR CONDITIONS OF ANY KIND, either express or implied.
**/

#ifndef APPLE_EVENT_IMPL_H_
#define APPLE_EVENT_IMPL_H_

#include <IndustryStandard/AppleHid.h>

#include APPLE_PROTOCOL_PRODUCER (AppleEvent)

#include <Library/AppleDriverLib.h>

// EventImplConstructor
EFI_STATUS
EFIAPI
EventImplConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  );

// mAppleEventProtocol
extern APPLE_EVENT_PROTOCOL gAppleEventProtocol;

#endif // APPLE_EVENT_IMPL_H_
