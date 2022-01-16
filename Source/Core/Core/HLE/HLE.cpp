// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HLE/HLE.h"

#include <algorithm>
#include <array>
#include <map>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/GeckoCode.h"
#include "Core/HLE/HLE_Misc.h"
#include "Core/HLE/HLE_OS.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/ES/ES.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"

namespace HLE
{
// Map addresses to the HLE hook index
static std::map<u32, u32> s_hooked_addresses;

// clang-format off
constexpr std::array<Hook, 23> os_patches{{
    // Placeholder, os_patches[0] is the "non-existent function" index
    {"FAKE_TO_SKIP_0",               HLE_Misc::UnimplementedFunction,       HookType::Replace, HookFlag::Generic},

    // Name doesn't matter, installed in CBoot::BootUp()
    {"HBReload",                     HLE_Misc::HBReload,                    HookType::Replace, HookFlag::Fixed},

    // Debug/OS Support
    {"OSPanic",                      HLE_OS::HLE_OSPanic,                   HookType::Replace, HookFlag::Debug},

    // This needs to be put before vprintf (because vprintf is called indirectly by this)
    {"JUTWarningConsole_f",          HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},

    {"OSReport",                     HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"DEBUGPrint",                   HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"WUD_DEBUGPrint",               HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"__DSP_debug_printf",           HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"vprintf",                      HLE_OS::HLE_GeneralDebugVPrint,        HookType::Start,   HookFlag::Debug},
    {"printf",                       HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"vdprintf",                     HLE_OS::HLE_LogVDPrint,                HookType::Start,   HookFlag::Debug},
    {"dprintf",                      HLE_OS::HLE_LogDPrint,                 HookType::Start,   HookFlag::Debug},
    {"vfprintf",                     HLE_OS::HLE_LogVFPrint,                HookType::Start,   HookFlag::Debug},
    {"fprintf",                      HLE_OS::HLE_LogFPrint,                 HookType::Start,   HookFlag::Debug},
    {"nlPrintf",                     HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"DWC_Printf",                   HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"RANK_Printf",                  HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"puts",                         HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug}, // gcc-optimized printf?
    {"___blank",                     HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug}, // used for early init things (normally)
    {"__write_console",              HLE_OS::HLE_write_console,             HookType::Start,   HookFlag::Debug}, // used by sysmenu (+more?)

    {"GeckoCodehandler",             HLE_Misc::GeckoCodeHandlerICacheFlush, HookType::Start,   HookFlag::Fixed},
    {"GeckoHandlerReturnTrampoline", HLE_Misc::GeckoReturnTrampoline,       HookType::Replace, HookFlag::Fixed},
    {"AppLoaderReport",              HLE_OS::HLE_GeneralDebugPrint,         HookType::Replace, HookFlag::Fixed} // apploader needs OSReport-like function
}};
// clang-format on

void Patch(u32 addr, std::string_view func_name)
{
  for (u32 i = 1; i < os_patches.size(); ++i)
  {
    if (os_patches[i].name == func_name)
    {
      s_hooked_addresses[addr] = i;
      PowerPC::ppcState.iCache.Invalidate(addr);
      return;
    }
  }
}

void PatchFixedFunctions()
{
  // MIOS puts patch data in low MEM1 (0x1800-0x3000) for its own use.
  // Overwriting data in this range can cause the IPL to crash when launching games
  // that get patched by MIOS. See https://bugs.dolphin-emu.org/issues/11952 for more info.
  // Not applying the Gecko HLE patches means that Gecko codes will not work under MIOS,
  // but this is better than the alternative of having specific games crash.
  if (SConfig::GetInstance().m_is_mios)
    return;

  // HLE jump to loader (homebrew).  Disabled when Gecko is active as it interferes with the code
  // handler
  if (!Config::Get(Config::MAIN_ENABLE_CHEATS))
  {
    Patch(0x80001800, "HBReload");
    Memory::CopyToEmu(0x00001804, "STUBHAXX", 8);
  }

  // Not part of the binary itself, but either we or Gecko OS might insert
  // this, and it doesn't clear the icache properly.
  Patch(Gecko::ENTRY_POINT, "GeckoCodehandler");
  // This has to always be installed even if cheats are not enabled because of the possiblity of
  // loading a savestate where PC is inside the code handler while cheats are disabled.
  Patch(Gecko::HLE_TRAMPOLINE_ADDRESS, "GeckoHandlerReturnTrampoline");
}

void PatchFunctions()
{
  // Remove all hooks that aren't fixed address hooks
  for (auto i = s_hooked_addresses.begin(); i != s_hooked_addresses.end();)
  {
    if (os_patches[i->second].flags != HookFlag::Fixed)
    {
      PowerPC::ppcState.iCache.Invalidate(i->first);
      i = s_hooked_addresses.erase(i);
    }
    else
    {
      ++i;
    }
  }

  for (u32 i = 1; i < os_patches.size(); ++i)
  {
    // Fixed hooks don't map to symbols
    if (os_patches[i].flags == HookFlag::Fixed)
      continue;

    for (const auto& symbol : g_symbolDB.GetSymbolsFromName(os_patches[i].name))
    {
      for (u32 addr = symbol->address; addr < symbol->address + symbol->size; addr += 4)
      {
        s_hooked_addresses[addr] = i;
        PowerPC::ppcState.iCache.Invalidate(addr);
      }
      INFO_LOG_FMT(OSHLE, "Patching {} {:08x}", os_patches[i].name, symbol->address);
    }
  }
}

void Clear()
{
  s_hooked_addresses.clear();
}

void Reload()
{
  Clear();
  PatchFixedFunctions();
  PatchFunctions();
}

void Execute(u32 current_pc, u32 hook_index)
{
  hook_index &= 0xFFFFF;
  if (hook_index > 0 && hook_index < os_patches.size())
  {
    os_patches[hook_index].function();
  }
  else
  {
    PanicAlertFmt("HLE system tried to call an undefined HLE function {}.", hook_index);
  }
}

u32 GetHookByAddress(u32 address)
{
  auto iter = s_hooked_addresses.find(address);
  return (iter != s_hooked_addresses.end()) ? iter->second : 0;
}

u32 GetHookByFunctionAddress(u32 address)
{
  const u32 index = GetHookByAddress(address);
  // Fixed hooks use a fixed address and don't patch the whole function
  if (index == 0 || os_patches[index].flags == HookFlag::Fixed)
    return index;

  const auto symbol = g_symbolDB.GetSymbolFromAddr(address);
  return (symbol && symbol->address == address) ? index : 0;
}

HookType GetHookTypeByIndex(u32 index)
{
  return os_patches[index].type;
}

HookFlag GetHookFlagsByIndex(u32 index)
{
  return os_patches[index].flags;
}

bool IsEnabled(HookFlag flag)
{
  return flag != HLE::HookFlag::Debug || Config::Get(Config::MAIN_ENABLE_DEBUGGING) ||
         PowerPC::GetMode() == PowerPC::CoreMode::Interpreter;
}

u32 UnPatch(std::string_view patch_name)
{
  const auto patch = std::find_if(std::begin(os_patches), std::end(os_patches),
                                  [&](const Hook& p) { return patch_name == p.name; });
  if (patch == std::end(os_patches))
    return 0;

  if (patch->flags == HookFlag::Fixed)
  {
    const u32 patch_idx = static_cast<u32>(std::distance(os_patches.begin(), patch));
    u32 addr = 0;
    // Reverse search by OSPatch key instead of address
    for (auto i = s_hooked_addresses.begin(); i != s_hooked_addresses.end();)
    {
      if (i->second == patch_idx)
      {
        addr = i->first;
        PowerPC::ppcState.iCache.Invalidate(i->first);
        i = s_hooked_addresses.erase(i);
      }
      else
      {
        ++i;
      }
    }
    return addr;
  }

  const auto& symbols = g_symbolDB.GetSymbolsFromName(patch_name);
  if (!symbols.empty())
  {
    const auto& symbol = symbols[0];
    for (u32 addr = symbol->address; addr < symbol->address + symbol->size; addr += 4)
    {
      s_hooked_addresses.erase(addr);
      PowerPC::ppcState.iCache.Invalidate(addr);
    }
    return symbol->address;
  }

  return 0;
}

u32 UnpatchRange(u32 start_addr, u32 end_addr)
{
  u32 count = 0;

  auto i = s_hooked_addresses.lower_bound(start_addr);
  while (i != s_hooked_addresses.end() && i->first < end_addr)
  {
    INFO_LOG_FMT(OSHLE, "Unpatch HLE hooks [{:08x};{:08x}): {} at {:08x}", start_addr, end_addr,
                 os_patches[i->second].name, i->first);
    PowerPC::ppcState.iCache.Invalidate(i->first);
    i = s_hooked_addresses.erase(i);
    count += 1;
  }

  return count;
}
}  // namespace HLE
