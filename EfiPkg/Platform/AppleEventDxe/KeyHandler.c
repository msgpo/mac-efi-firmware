#include <AppleMacEfi.h>

#include <IndustryStandard/AppleHid.h>

#include <Protocol/AppleKeyMapAggregator.h>
#include <Protocol/ConsoleControl.h>

#include <Library/AppleEventLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "AppleEventInternal.h"

// KEY_STROKE_DELAY
#define KEY_STROKE_DELAY  5

// KEY_STROKE_POLL_FREQUENCY
#define KEY_STROKE_POLL_FREQUENCY  100000

// KEY_STROKE_INFORMATION
typedef struct {
  APPLE_KEY_CODE AppleKeyCode;     ///<
  UINTN          NumberOfStrokes;  ///<
  BOOLEAN        CurrentStroke;    ///<
} KEY_STROKE_INFORMATION;

// mCLockOn
STATIC BOOLEAN mCLockOn = FALSE;

// mKeyStrokePollEvent
STATIC EFI_EVENT mKeyStrokePollEvent = NULL;

// mModifiers
STATIC APPLE_MODIFIER_MAP mModifiers = 0;

// mPreviousModifiers
STATIC APPLE_MODIFIER_MAP mPreviousModifiers = 0;

// mInitialized
STATIC BOOLEAN mInitialized = FALSE;

// mKeyInformation
STATIC KEY_STROKE_INFORMATION mKeyStrokeInfo[10];

// mCLockChanged
STATIC BOOLEAN mCLockChanged = FALSE;

// mAppleKeyMapAggregator
STATIC APPLE_KEY_MAP_AGGREGATOR_PROTOCOL *mKeyMapAggregator = NULL;

// InternalGetAppleKeyStrokes
STATIC
EFI_STATUS
InternalGetAppleKeyStrokes (
  OUT APPLE_MODIFIER_MAP  *Modifiers,
  OUT UINTN               *NumberOfKeyCodes,
  OUT APPLE_KEY_CODE      **KeyCodes
  )
{
  EFI_STATUS Status;

  Status = EFI_UNSUPPORTED;

  if (mKeyMapAggregator != NULL) {
    Status = EFI_INVALID_PARAMETER;

    if ((Modifiers != NULL)
     && (NumberOfKeyCodes != NULL)
     && (KeyCodes != NULL)) {
      *NumberOfKeyCodes = 0;
      *KeyCodes         = NULL;
      Status            = mKeyMapAggregator->GetKeyStrokes (
                                               mKeyMapAggregator,
                                               Modifiers,
                                               NumberOfKeyCodes,
                                               NULL
                                               );

      if (!EFI_ERROR (Status) || Status == EFI_BUFFER_TOO_SMALL) {
        if (*NumberOfKeyCodes == 0) {
          *KeyCodes = NULL;
        } else {
          *KeyCodes = AllocatePool (
                        *NumberOfKeyCodes * sizeof (**KeyCodes)
                        );

          if (*KeyCodes == NULL) {
            *NumberOfKeyCodes = 0;
            Status        = EFI_OUT_OF_RESOURCES;
          } else {
            Status = mKeyMapAggregator->GetKeyStrokes (
                                               mKeyMapAggregator,
                                               Modifiers,
                                               NumberOfKeyCodes,
                                               *KeyCodes
                                               );

            if (EFI_ERROR (Status)) {
              FreePool ((VOID *)*KeyCodes);

              *KeyCodes         = NULL;
              *NumberOfKeyCodes = 0;
            }
          }
        }
      }
    }
  }

  return Status;
}

// InternalGetModifierStrokes
APPLE_MODIFIER_MAP
InternalGetModifierStrokes (
  VOID
  )
{
  APPLE_MODIFIER_MAP Modifiers;

  EFI_STATUS         Status;
  UINTN              NumberOfKeyCodes;
  APPLE_KEY_CODE     *KeyCodes;

  Status = InternalGetAppleKeyStrokes (
             &Modifiers,
             &NumberOfKeyCodes,
             &KeyCodes
             );

  if (!EFI_ERROR (Status)) {
    if (KeyCodes != NULL) {
      FreePool ((VOID *)KeyCodes);
    }
  } else {
    Modifiers = 0;
  }

  return Modifiers;
}

// InternalAppleKeyEventDataFromInputKey
STATIC
EFI_STATUS
InternalAppleKeyEventDataFromInputKey (
  OUT APPLE_EVENT_DATA  *EventData,
  IN  APPLE_KEY_CODE    *AppleKeyCode,
  IN  EFI_INPUT_KEY     *InputKey
  )
{
  EFI_STATUS           Status;

  APPLE_KEY_EVENT_DATA *KeyEventData;

  Status = EFI_INVALID_PARAMETER;

  if ((EventData != NULL) && (AppleKeyCode != NULL) && (InputKey != NULL)) {
    KeyEventData = AllocateZeroPool (sizeof (*KeyEventData));
    Status       = EFI_OUT_OF_RESOURCES;

    if (KeyEventData != NULL) {
      KeyEventData->NumberOfKeyPairs = 1;
      KeyEventData->InputKey = *InputKey;

      CopyMem (
        (VOID *)&KeyEventData->AppleKeyCode,
        (VOID *)AppleKeyCode,
        sizeof (*AppleKeyCode)
        );

      EventData->KeyData = KeyEventData;

      Status = EFI_SUCCESS;
    }
  }

  return Status;
}

// InternalGetAndRemoveReleasedKeys
STATIC
UINTN
InternalGetAndRemoveReleasedKeys (
  IN  UINTN           *NumberOfKeyCodes,
  IN  APPLE_KEY_CODE  *KeyCodes,
  OUT APPLE_KEY_CODE  **ReleasedKeys
  )
{
  UINTN          NumberOfReleasedKeys;

  UINTN          Index;
  UINTN          Index2;
  APPLE_KEY_CODE ReleasedKeysBuffer[12];
  UINTN          ReleasedKeysSize;

  NumberOfReleasedKeys = 0;

  for (Index = 0; Index < ARRAY_SIZE (mKeyStrokeInfo); ++Index) {
    for (Index2 = 0; Index2 < *NumberOfKeyCodes; ++Index2) {
      if (mKeyStrokeInfo[Index].AppleKeyCode == KeyCodes[Index2]) {
        break;
      }
    }

    if (*NumberOfKeyCodes == Index2) {
      if (mKeyStrokeInfo[Index].AppleKeyCode != 0) {
        ReleasedKeysBuffer[NumberOfReleasedKeys] = mKeyStrokeInfo[Index].AppleKeyCode;
        ++NumberOfReleasedKeys;
      }

      ZeroMem (
        &mKeyStrokeInfo[Index],
        sizeof (mKeyStrokeInfo[Index])
        );
    }
  }

  // Add CLock to released keys if applicable and set bool to FALSE.

  if (mCLockChanged) {
    for (Index = 0; Index < *NumberOfKeyCodes; ++Index) {
      if (KeyCodes[Index] == AppleHidUsbKbUsageKeyCLock) {
        break;
      }
    }

    if (*NumberOfKeyCodes == Index) {
      mCLockChanged = FALSE;

      ReleasedKeysBuffer[NumberOfReleasedKeys] = AppleHidUsbKbUsageKeyCLock;
      ++NumberOfReleasedKeys;
    }
  }

  // Allocate a heap buffer to return.

  *ReleasedKeys = NULL;

  if (NumberOfReleasedKeys > 0) {
    ReleasedKeysSize = (sizeof (**ReleasedKeys) * NumberOfReleasedKeys);
    *ReleasedKeys    = AllocatePool (ReleasedKeysSize);

    if (*ReleasedKeys != NULL) {
      CopyMem (
        (VOID *)*ReleasedKeys,
        (VOID *)&ReleasedKeysBuffer[0],
        ReleasedKeysSize
        );
    } else {
      NumberOfReleasedKeys = 0;
    }
  }

  return NumberOfReleasedKeys;
}

// InternalIsCLockOn
STATIC
BOOLEAN
InternalIsCLockOn (
  IN UINTN           *NumberOfKeyCodes,
  IN APPLE_KEY_CODE  *KeyCodes
  )
{
  BOOLEAN                CLockOn;

  UINTN                  Index;
  UINTN                  Index2;
  KEY_STROKE_INFORMATION *KeyInfo;

  //
  // Check against invalid usage
  //
  if (NumberOfKeyCodes == NULL
    || (*NumberOfKeyCodes != 0 && KeyCodes == NULL)) {
    return FALSE;
  }

  //
  // Return the previous value by default
  //
  CLockOn = mCLockOn;

  for (Index = 0; Index < *NumberOfKeyCodes; ++Index) {
    KeyInfo = NULL;

    for (Index2 = 0; Index2 < ARRAY_SIZE (mKeyStrokeInfo); ++Index2) {
      if (mKeyStrokeInfo[Index2].AppleKeyCode == KeyCodes[Index]) {
        KeyInfo = &mKeyStrokeInfo[Index2];
        break;
      }
    }

    if (KeyInfo == NULL
      && KeyCodes[Index] == AppleHidUsbKbUsageKeyCLock
      && !mCLockChanged) {
      CLockOn = !mCLockOn;
      break;
    }
  }

  return CLockOn;
}

// InternalGetCurrentStroke
STATIC
KEY_STROKE_INFORMATION *
InternalGetCurrentStroke (
  VOID
  )
{
  KEY_STROKE_INFORMATION *KeyInfo;

  KEY_STROKE_INFORMATION *KeyInfoWalker;
  UINTN                  Index;

  KeyInfo       = NULL;
  KeyInfoWalker = &mKeyStrokeInfo[0];

  for (Index = 0; Index < ARRAY_SIZE (mKeyStrokeInfo); ++Index) {
    if (KeyInfo->CurrentStroke) {
      KeyInfo = KeyInfoWalker;

      break;
    }

    ++KeyInfoWalker;
  }

  return KeyInfo;
}

// InternalGetCurrentKeyStroke
STATIC
EFI_STATUS
InternalGetCurrentKeyStroke (
  IN     APPLE_MODIFIER_MAP  Modifiers,
  IN OUT UINTN               *NumberOfKeyCodes,
  IN OUT APPLE_KEY_CODE      *KeyCodes,
  IN OUT EFI_INPUT_KEY       *Key
  )
{
  EFI_STATUS             Status;

  KEY_STROKE_INFORMATION *KeyInfo;
  UINTN                  Index;
  UINTN                  Index2;
  UINTN                  NumberOfReleasedKeys;
  APPLE_KEY_CODE         *ReleasedKeys;
  BOOLEAN                CLockOn;
  APPLE_MODIFIER_MAP     AppleModifiers;
  BOOLEAN                ShiftPressed;
  APPLE_KEY_CODE         *ReleasedKeyWalker;
  EFI_INPUT_KEY          InputKey;
  APPLE_EVENT_DATA       AppleEventData;
  KEY_STROKE_INFORMATION *KeyInfoWalker;
  UINTN                  NewKeyIndex;
  BOOLEAN                AcceptStroke;
  BOOLEAN                Shifted;

  if (mModifiers != Modifiers) {
    for (Index = 0; Index < ARRAY_SIZE (mKeyStrokeInfo); ++Index) {
      mKeyStrokeInfo[Index].CurrentStroke = FALSE;
    }
  }

  NumberOfReleasedKeys = InternalGetAndRemoveReleasedKeys (
                           NumberOfKeyCodes,
                           KeyCodes,
                           &ReleasedKeys
                           );

  CLockOn = InternalIsCLockOn (NumberOfKeyCodes, KeyCodes);

  AppleModifiers = Modifiers;

  if (CLockOn) {
    AppleModifiers |= APPLE_MODIFIERS_SHIFT;
  }

  ShiftPressed      = (BOOLEAN)((AppleModifiers & APPLE_MODIFIERS_SHIFT) != 0);
  ReleasedKeyWalker = ReleasedKeys;

  for (Index = 0; Index < NumberOfReleasedKeys; ++Index) {
    EventInputKeyFromAppleKeyCode (
      *ReleasedKeyWalker,
      &InputKey,
      ShiftPressed
      );

    AppleEventData.KeyData = NULL;
    Status                 = InternalAppleKeyEventDataFromInputKey (
                               &AppleEventData,
                               ReleasedKeyWalker,
                               &InputKey
                               );

    if (Status != EFI_SUCCESS) {
      FreePool ((VOID *)ReleasedKeys);

      ReleasedKeys = NULL;
    }

    EventCreateEventQueue (
      AppleEventData,
      APPLE_EVENT_TYPE_KEY_UP,
      AppleModifiers
      );

    ++ReleasedKeyWalker;
  }

  if (ReleasedKeys != NULL) {
    FreePool ((VOID *)ReleasedKeys);
  }

  if (CLockOn != mCLockOn) {
    mCLockOn           = CLockOn;
    mCLockChanged      = TRUE;
  }

  // Increase the number of strokes for all currently pressed keys.

  for (NewKeyIndex = 0; NewKeyIndex < *NumberOfKeyCodes; ++NewKeyIndex) {
    KeyInfo       = NULL;
    KeyInfoWalker = mKeyStrokeInfo;

    for (Index2 = 0; Index2 < ARRAY_SIZE (mKeyStrokeInfo); ++Index2) {
      KeyInfo = KeyInfoWalker;
      ++KeyInfoWalker;

      if (KeyInfo->AppleKeyCode == KeyCodes[NewKeyIndex]) {
        break;
      }
    }

    // Indicates a key has been pressed which is not part of mKeyInformation.
    if ((Index2 >= ARRAY_SIZE (mKeyStrokeInfo)) || (KeyInfo == NULL)) {
      // If a new key is pressed, cancel all previous inputs.

      for (Index = 0; Index < ARRAY_SIZE (mKeyStrokeInfo); ++Index) {
        mKeyStrokeInfo[Index].CurrentStroke = FALSE;
      }

      KeyInfo = mKeyStrokeInfo;

      for (Index = 0; Index < ARRAY_SIZE (mKeyStrokeInfo); ++Index) {
        if (KeyInfo->AppleKeyCode == 0) {
          KeyInfo->AppleKeyCode    = KeyCodes[NewKeyIndex];
          KeyInfo->CurrentStroke   = TRUE;
          KeyInfo->NumberOfStrokes = 0;

          break;
        }

        ++KeyInfo;
      }

      break;
    }

    ++KeyInfo->NumberOfStrokes;
  }

  KeyInfo = InternalGetCurrentStroke ();

  Status = EFI_NOT_READY;

  if ((KeyInfo != NULL) || (mModifiers != Modifiers)) {
    mModifiers = Modifiers;

    // Verify the timeframe the key has been pressed.

    if (KeyInfo != NULL) {
      AcceptStroke = (BOOLEAN)(
                       (KeyInfo->NumberOfStrokes < (KEY_STROKE_DELAY * 10))
                         ? (KeyInfo->NumberOfStrokes == 0)
                         : ((KeyInfo->NumberOfStrokes % KEY_STROKE_DELAY) == 0)
                       );

      if (AcceptStroke) {
        *NumberOfKeyCodes = 1;
        *KeyCodes         = KeyInfo->AppleKeyCode;

        Shifted = (BOOLEAN)(
                    (IS_APPLE_KEY_LETTER (KeyInfo->AppleKeyCode) && CLockOn)
                      != ((mModifiers & APPLE_MODIFIERS_SHIFT) != 0)
                    );

        EventInputKeyFromAppleKeyCode (
          KeyInfo->AppleKeyCode,
          Key,
          Shifted
          );
      }
    } else {
      *NumberOfKeyCodes = 0;
    }

    Status = EFI_SUCCESS;
  }

  return Status;
}

// CreateAppleKeyCodeDescriptorsFromKeyStrokes
STATIC
EFI_STATUS
InternalAppleEventDataFromCurrentKeyStroke (
  IN OUT APPLE_EVENT_DATA    *EventData,
  IN OUT APPLE_MODIFIER_MAP  *Modifiers
  )
{
  EFI_STATUS                      Status;

  EFI_INPUT_KEY                   InputKey;
  APPLE_KEY_CODE                  *KeyCodes;
  APPLE_MODIFIER_MAP              AppleModifiers;
  UINTN                           NumberOfKeyCodes;
  EFI_CONSOLE_CONTROL_PROTOCOL    *ConsoleControl;
  EFI_CONSOLE_CONTROL_SCREEN_MODE Mode;
  UINTN                           Index;

  ZeroMem (&InputKey, sizeof (InputKey));

  KeyCodes = NULL;

  Status = EFI_UNSUPPORTED;

  if ((mKeyMapAggregator != NULL)
   && (EventData != NULL)
   && (Modifiers != NULL)) {
    AppleModifiers   = 0;
    NumberOfKeyCodes = 0;

    InternalGetAppleKeyStrokes (
      &AppleModifiers,
      &NumberOfKeyCodes,
      &KeyCodes
      );

    Mode   = EfiConsoleControlScreenGraphics;
    Status = gBS->LocateProtocol (
                    &gEfiConsoleControlProtocolGuid,
                    NULL,
                    (VOID *)&ConsoleControl
                    );

    if (!EFI_ERROR (Status)) {
      ConsoleControl->GetMode (ConsoleControl, &Mode, NULL, NULL);
    }

    if (Mode == EfiConsoleControlScreenGraphics) {
      for (Index = 0; Index < (NumberOfKeyCodes + 1); ++Index) {
        Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &InputKey);

        if (EFI_ERROR (Status)) {
          break;
        }
      }
    }

    *Modifiers = AppleModifiers;
    Status     = InternalGetCurrentKeyStroke (
                   AppleModifiers,
                   &NumberOfKeyCodes,
                   KeyCodes,
                   &InputKey
                   );

    if (!EFI_ERROR (Status)) {
      Status = EFI_SUCCESS;
      if (NumberOfKeyCodes > 0) {
        InternalAppleKeyEventDataFromInputKey (EventData, KeyCodes, &InputKey);
      }
    }

    if (KeyCodes) {
      FreePool (KeyCodes);
    }
  }

  return Status;
}

// InternalKeyStrokePollNotifyFunction
STATIC
VOID
EFIAPI
InternalKeyStrokePollNotifyFunction (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS         Status;

  APPLE_EVENT_DATA   EventData;
  APPLE_MODIFIER_MAP Modifiers;
  APPLE_MODIFIER_MAP PartialModifers;

  EventData.KeyData = NULL;
  Modifiers         = 0;
  Status            = InternalAppleEventDataFromCurrentKeyStroke (
                        &EventData,
                        &Modifiers
                        );

  if (!EFI_ERROR (Status)) {
    if (EventData.KeyData != NULL) {
      EventCreateEventQueue (EventData, APPLE_EVENT_TYPE_KEY_DOWN, Modifiers);
    }

    if (mPreviousModifiers != Modifiers) {
      PartialModifers = ((mPreviousModifiers ^ Modifiers) & mPreviousModifiers);

      if (PartialModifers != 0) {
        EventData.KeyData = NULL;

        EventCreateEventQueue (
          EventData,
          APPLE_EVENT_TYPE_MODIFIER_UP,
          PartialModifers
          );
      }

      PartialModifers = ((mPreviousModifiers ^ Modifiers) & Modifiers);

      if (PartialModifers != 0) {
        EventData.KeyData = NULL;

        EventCreateEventQueue (
          EventData,
          APPLE_EVENT_TYPE_MODIFIER_DOWN,
          PartialModifers
          );
      }

      mPreviousModifiers = Modifiers;
    }
  }
}

// InternalInitializeKeyHandler
STATIC
VOID
InternalInitializeKeyHandler (
  VOID
  )
{
  if (!mInitialized) {
    mInitialized = TRUE;

    ZeroMem ((VOID *)&mKeyStrokeInfo[0], sizeof (mKeyStrokeInfo));

    mModifiers         = 0;
    mCLockOn           = FALSE;
    mCLockChanged      = FALSE;
  }
}

// EventCreateKeyStrokePollEvent
EFI_STATUS
EventCreateKeyStrokePollEvent (
  VOID
  )
{
  EFI_STATUS Status;

  Status = gBS->LocateProtocol (
                  &gAppleKeyMapAggregatorProtocolGuid,
                  NULL,
                  (VOID **)&mKeyMapAggregator
                  );

  if (!EFI_ERROR (Status)) {
    InternalInitializeKeyHandler ();

    mKeyStrokePollEvent = EventLibCreateNotifyTimerEvent (
                            InternalKeyStrokePollNotifyFunction,
                            NULL,
                            KEY_STROKE_POLL_FREQUENCY,
                            TRUE
                            );

    Status = ((mKeyStrokePollEvent == NULL)
               ? EFI_OUT_OF_RESOURCES
               : EFI_SUCCESS);
  }

  return Status;
}

// EventCancelKeyStrokePollEvent
VOID
EventCancelKeyStrokePollEvent (
  VOID
  )
{
  EventLibCancelEvent (mKeyStrokePollEvent);

  mKeyStrokePollEvent = NULL;
}

// EventIsCapsLockOnImpl
/** Retrieves the state of the CapsLock key.

  @param[in, out] CLockOn  This parameter indicates the state of the CapsLock
                           key.

  @retval EFI_SUCCESS            The CapsLock state was successfully returned
                                 in CLockOn.
  @retval EFI_INVALID_PARAMETER  CLockOn is NULL.
**/
EFI_STATUS
EFIAPI
EventIsCapsLockOnImpl (
  IN OUT BOOLEAN  *CLockOn
  )
{
  EFI_STATUS Status;

  Status = EFI_INVALID_PARAMETER;

  if (CLockOn != NULL) {
    *CLockOn = mCLockOn;

    Status = EFI_SUCCESS;
  }

  return Status;
}
