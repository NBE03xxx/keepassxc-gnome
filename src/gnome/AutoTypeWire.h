#pragma once

#include "AutoTypeSequence.h"

namespace AutoTypeWire
{
constexpr char Ready = 'R';
constexpr char ReadyWithIme = 'I';
constexpr char Triggered = 'T';
constexpr int PermissionOrShortcut = 3;
constexpr int UnsupportedLayout = 4;
constexpr int Interrupted = 5;
constexpr int ModifiersHeld = 6;
constexpr int ImeUnavailable = 7;
constexpr int ImeRestoreFailed = 8;
constexpr unsigned KindShift = 24;
constexpr std::uint32_t ValueMask = (1U << KindShift) - 1;
}
