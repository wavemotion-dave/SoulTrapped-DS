// =====================================================================================
// Copyright (c) 2026 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave and
// Marat Fayzullin (fMSX core) are thanked profusely.
//
// The SoulTrappedDS emulator is offered as-is, without any warranty. Please see readme.md
//
// This file is our bridge between the Z80 CPU core and the rest of the system.
// SoulTrappedDS currently supports the CZ80 CPU core.
// =====================================================================================
#include <nds.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Z80_interface.h"
#include "../../SoulTrappedDS.h"
#include "../../MSX_generic.h"
#include "../../printf.h"

u8 sram_write_enabled_a = 0;        // Is SRAM writable in segment A (many SRAM games allow writes in 2 segments)
u8 sram_write_enabled_b = 0;        // Is SRAM writable in segment B (many SRAM games allow writes in 2 segments)
u8 FMPAC_SRAM_in_view   = 0;        // Is the FMPAC SRAM in view?
u8 FMPAC_Key1           = 0;        // FM-PAC SRAM requires a special key write sequence to open up SRAM access
u16 NEO_block           = 0x0000;   // NEO8/16 block number
u16 NEO16_bank[3] = {0};

// -----------------------------------------------------------------------------------------------
// All memory fetches run through this except OP codes which are read directly from memory
// for maximum speed (an Op Code fetch should never result in accessing special memory registers).
// -----------------------------------------------------------------------------------------------
ITCM_CODE u8 cpu_readmem16(u16 address)
{
    if ((special_memory_access & SPEC_MEM_SUBSLOT_ACTIVE) && (address == 0xFFFF)) // Subslot check... only when page 3 is mapped to an expanded slot
    {
        return (u8)~msx_subslot; // Compliment is returned. MSX1 map will never set SPEC_MEM_SUBSLOT_ACTIVE.
    }

    // Otherwise normal read - just index into the 8K memory block and fetch the byte...
    return (MemoryMap[address>>13][address]);
}


// -----------------------------------------------------------------------------------
// NEO16 is an 16K mapper but has a 16-bit block size. We can only support 8MB max...
// -----------------------------------------------------------------------------------
void __attribute__((noinline)) HandleNeo16(u8 media_id, u16 address, u8 value)
{
    // Register select = bits 13..11 (BBB): 010 -> bank 0, 100 -> bank 1, 110 -> bank 2
    u8 bbb = (address >> 11) & 0x07;
    if (bbb < 2 || (bbb & 1))
        return;
    u8 bank = (bbb - 2) >> 1;                  // 0..2

    if (address & 1)   // odd = high byte (only 6 bits used as upper bits reserved)
        NEO16_bank[bank] = (NEO16_bank[bank] & 0x00FF) | ((u16)(value & 0x3F) << 8);
    else               // even = low byte
        NEO16_bank[bank] = (NEO16_bank[bank] & 0xFF00) | value;

    // Segments are 16K here, regardless of what blockSize holds for this media
    u32 seg     = NEO16_bank[bank] & MyMedia[media_id].mapperMask;
    u8 *src     = (u8*)ROM_Memory + (seg << 14);

    // Keep the per-8K cart pointers in the same layout HandleNeo8 uses
    MSXCartPtr[media_id][bank * 2]     = src;
    MSXCartPtr[media_id][bank * 2 + 1] = src + 0x2000;

    if (bCartInPage[bank] == (media_id + 1))   // 16K bank n == MSX page n
    {
        u8 *base = src - (bank * 0x4000);      // same bias works for both 8K halves
        MemoryMap[bank * 2]     = base;
        MemoryMap[bank * 2 + 1] = base;
    }
}


// -------------------------------------------------------------------
// The FM-PAC presents 8K of SRAM for a game to use to save progress.
// This handler is called when any byte in the 0x4000-0x5FFF range is
// written and will determine if/when the SRAM is available.
// -------------------------------------------------------------------
#define FMPAC_KEY1 0x4D
#define FMPAC_KEY2 0x69
void __attribute__((noinline)) HandleFMPAC(u8 media_id, u16 address, u8 value)
{
    if (address == 0x5FFE)
    {
        // Key register
        SRAM_Memory[0x1FFE] = value;

        if ((SRAM_Memory[0x1FFF] == 0x69) && (value == 0x4D))
        {
            FMPAC_SRAM_in_view = 1;
            MSXCartPtr[MEDIA_CART2][2] = (u8*)SRAM_Memory;
            MemoryMap[2] = MSXCartPtr[MEDIA_CART2][2] - 0x4000;
        }
        else
        {
            FMPAC_SRAM_in_view = 0;
            MSXCartPtr[MEDIA_CART2][2] = (u8*)MSXBios_FMPAC;
            MemoryMap[2] = MSXCartPtr[MEDIA_CART2][2] - 0x4000;
        }
        return;
    }
    else if (address == 0x5FFF)
    {
        // Key register
        SRAM_Memory[0x1FFF] = value;

        if ((SRAM_Memory[0x1FFE] == 0x4D) && (value == 0x69))
        {
            FMPAC_SRAM_in_view = 1;
            MSXCartPtr[MEDIA_CART2][2] = (u8*)SRAM_Memory;
            MemoryMap[2] = MSXCartPtr[MEDIA_CART2][2] - 0x4000;
        }
        else
        {
            FMPAC_SRAM_in_view = 0;
            MSXCartPtr[MEDIA_CART2][2] = (u8*)MSXBios_FMPAC;
            MemoryMap[2] = MSXCartPtr[MEDIA_CART2][2] - 0x4000;
        }
        return;
    }

    // Otherwise see if we are able to write into the SRAM
    if (FMPAC_SRAM_in_view && (address >= 0x4000) && (address < 0x5FFE))
    {
        SRAM_Memory[address & 0x1FFF] = value;
        sram_show_status = 4;
    }
}

typedef void (*MapperWriteHandler)(u8 media_id, u16 address, u8 value);

static MapperWriteHandler const mapperWriteHandlers[END_MAPPERS] __attribute__((section(".dtcm"))) =
{
    [NEO16]             = HandleNeo16,
    [FMPAC_SRAM]        = HandleFMPAC,
};

// ---------------------------------------------------------------------
// Write memory handles both normal writes and bankswitched since
// write is much less common than reads... All of the MSX cart mappers
// are vectored here so we can handle all the bank switching, etc.
// ---------------------------------------------------------------------
ITCM_CODE void cpu_writemem16(u16 address, u8 value)
{
    if ((special_memory_access & SPEC_MEM_SUBSLOT_ACTIVE) && (address == 0xFFFF)) // Subslot check... only when page 3 is mapped to an expanded slot
    {
        msx_subslot = value;
        cpu_writeport_msx(0xA8, Port_PPI_A); // Enable the new map...
    }
    else if (bRAMInPage[address >> 14]) // RAM Exists in this slot... write it.
    {
        MemoryMap[address>>13][address]=value;
    }
    else if (bCartInPage[address>>14]) // Is there a Cartridge mapped into the write address area?
    {
        u8 media_id = bCartInPage[address>>14] - 1;     // Is this MEDIA_CART1 or MEDIA_CART2?
        u8 mapper_type = MyMedia[media_id].mapperType;  // And what type of CART is it?

        // -----------------------------------------------------------------------
        // Each cartridge mapper is different - call into the appropriate handler.
        // A table-lookup here keeps this execution path relatively deterministic
        // no matter how many mappers we have.
        // -----------------------------------------------------------------------
        if (mapperWriteHandlers[mapper_type] != NULL)
        {
            mapperWriteHandlers[mapper_type](media_id, address, value);
        }
    }
}

// -----------------------------------------------------------------
// Reset a few key variables needed for proper Z80 interface use...
// -----------------------------------------------------------------
void Z80_Interface_Reset(void)
{
    CPU.CycleDeficit      = 0;
    sram_write_enabled_a  = 0;
    sram_write_enabled_b  = 0;
    FMPAC_SRAM_in_view    = 0;
    FMPAC_Key1            = 0;
    NEO_block             = 0;
    memset(NEO16_bank, 0x00, sizeof(NEO16_bank));
}

// -----------------------------------------------------------------
// Trap and report illegal opcodes to the SoulTrappedDS debugger...
// -----------------------------------------------------------------
void Trap_Bad_Ops(char *prefix, byte I, word W)
{
    if (myGlobalConfig.debugger)
    {
        char tmp[32];
        sprintf(tmp, "ILLOP: %s %02X %04X", prefix, I, W);
        DSPrint(0,0,6, tmp);
    }
}

// End of file
