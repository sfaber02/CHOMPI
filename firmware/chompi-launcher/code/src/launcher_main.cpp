/** @file launcher_main.cpp
 *  @brief CHOMPI firmware launcher.
 *
 *  Power on, and the launcher lights one key for every firmware it finds on the
 *  SD card. Press a key and that firmware starts. Power cycle to come back here.
 *
 *  Installing it is an ordinary firmware update: the launcher's own CHOMPI.bin
 *  goes in the root of the card, and the firmwares it offers go in /FIRMWARE.
 *  The stock bootloader skips directories when it hunts for a .bin to install,
 *  so it only ever sees the launcher, and QSPI keeps holding the launcher no
 *  matter which firmware was last run.
 */
#include "daisy_seed.h"
#include "hardware.h"
#include "chainload.h"
#include "fatfs.h"
#include <cstring>
#include <cstdio>
#include <strings.h>
#include <cstdarg>

using namespace daisy;
using namespace chompi;

/** Where the firmwares live. Invisible to the bootloader because it is a
 *  directory, which is exactly why this works. */
static const char *kFirmwareDir = "FIRMWARE";

/** One per white key, which is as many as the picker can show. */
static constexpr int kMaxSlots = 15;

/** White keys, left to right. These are the slot keys. */
static const Hardware::SwId kSlotKey[kMaxSlots] = {
    Hardware::SwId::KEY_1,  Hardware::SwId::KEY_2,  Hardware::SwId::KEY_3,
    Hardware::SwId::KEY_4,  Hardware::SwId::KEY_5,  Hardware::SwId::KEY_6,
    Hardware::SwId::KEY_7,  Hardware::SwId::KEY_8,  Hardware::SwId::KEY_9,
    Hardware::SwId::KEY_10, Hardware::SwId::KEY_11, Hardware::SwId::KEY_12,
    Hardware::SwId::KEY_13, Hardware::SwId::KEY_14, Hardware::SwId::KEY_15,
};

/** The SMT LED sitting under each of those keys (from TAPE/WAVE's led_map). */
static const int kSlotLed[kMaxSlots] = {
    24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10,
};

Hardware      hw;
SdmmcHandler  sdmmc;
FatFSInterface fsi;

/** Staging buffer for the chosen image. SDRAM is 64MB and an app is ~250KB, so
 *  there is no reason to be clever: read the whole file in, check it, then jump. */
static uint8_t DSY_SDRAM_BSS fw_image[0x80000];

struct Slot
{
    char name[64];
    uint32_t size;
};

static Slot slots[kMaxSlots];
static int  slot_count = 0;

/* ------------------------------------------------------------------------
 *  Logging
 *
 *  There is no debugger on this unit, so the card is the only way to see what
 *  happened. The log is built in RAM and written out in one shot.
 *
 *  The first attempt at this appended a line at a time and produced a 0-byte
 *  file -- the per-line reopen was failing silently and taking the evidence
 *  with it. Buffering means exactly one f_open/f_write/f_close per flush, and
 *  the log cannot be lost to the same failure it is supposed to be reporting. */

/* Every FIL below is static, deliberately.
 *
 *  A FIL carries its own 512-byte sector buffer, and FatFS hands that buffer
 *  straight to the SD driver's DMA. The stack lives in DTCMRAM (MSP starts at
 *  0x20020000), and DTCM is not reachable by DMA on the STM32H7 -- so a FIL
 *  declared as a local silently breaks every read and write through it, while
 *  directory operations keep working because those use the FATFS object's own
 *  window buffer, which is a global.
 *
 *  This is why every FIL in the stock firmware is a class member rather than a
 *  local (OptionsManager, FileCopier, SampleReader, ui.h -- all of them).
 *  Static puts ours in .bss alongside them.
 *
 *  Not reentrant, which is fine here: one thing happens at a time. */

static const char *kLogFile = "FIRMWARE/launcher_log.txt";
static char        log_buf[8192];
static size_t      log_len = 0;

static void Log(const char *fmt, ...)
{
    if (log_len + 256 >= sizeof(log_buf))
        return; /* full; keep the earliest lines, they are the interesting ones */

    int n = snprintf(log_buf + log_len, sizeof(log_buf) - log_len, "[%7lu] ",
                     (unsigned long)System::GetNow());
    if (n < 0)
        return;
    log_len += n;

    va_list args;
    va_start(args, fmt);
    n = vsnprintf(log_buf + log_len, sizeof(log_buf) - log_len, fmt, args);
    va_end(args);
    if (n < 0)
        return;
    log_len += n;

    log_len += snprintf(log_buf + log_len, sizeof(log_buf) - log_len, "\r\n");
}

/** Write the whole log out.
 *
 *  Deliberately NOT FA_CREATE_ALWAYS. That truncates on open, so a flush whose
 *  write then fails leaves an empty file -- which is exactly how the first
 *  version of this destroyed the evidence it was collecting. Open without
 *  truncating, overwrite from the start, and only cut the file back once the
 *  write has actually succeeded. A failed flush then leaves the previous
 *  contents intact. */
static void LogFlush()
{
    static FIL f;
    if (f_open(&f, kLogFile, FA_OPEN_ALWAYS | FA_WRITE) != FR_OK)
        return;
    if (f_lseek(&f, 0) != FR_OK)
    {
        f_close(&f);
        return;
    }

    UINT put = 0;
    if (f_write(&f, log_buf, log_len, &put) == FR_OK && put == log_len)
        f_truncate(&f); /* drop any tail left from a longer previous flush */

    f_close(&f); /* closing is what actually commits it */
}

/** Case-insensitive test for a trailing ".bin". */
static bool HasBinExtension(const char *name)
{
    const size_t len = strlen(name);
    if (len < 5) /* at minimum "x.bin" */
        return false;
    const char *ext = name + len - 4;
    return (ext[0] == '.') && (ext[1] == 'b' || ext[1] == 'B') &&
           (ext[2] == 'i' || ext[2] == 'I') && (ext[3] == 'n' || ext[3] == 'N');
}

/** Collect up to kMaxSlots .bin files from /FIRMWARE, sorted by name so the key
 *  order stays put as firmwares are added and removed. Prefix them 01_, 02_ and
 *  so on to pin them to particular keys. */
static void ScanFirmwares()
{
    DIR     dir;
    FILINFO info;

    slot_count = 0;

    FRESULT res = f_opendir(&dir, kFirmwareDir);
    Log("f_opendir(\"%s\") -> %d", kFirmwareDir, (int)res);
    if (res != FR_OK)
        return;

    int seen = 0;
    while (slot_count < kMaxSlots)
    {
        res = f_readdir(&dir, &info);
        if (res != FR_OK)
        {
            Log("f_readdir -> %d (stopping)", (int)res);
            break;
        }
        if (info.fname[0] == 0)
            break; /* end of directory */

        seen++;
        Log("  entry: \"%s\" attrib=0x%02X size=%lu", info.fname,
            (unsigned)info.fattrib, (unsigned long)info.fsize);

        if (info.fattrib & (AM_HID | AM_DIR))
        {
            Log("    skip: hidden or directory");
            continue;
        }
        if (!HasBinExtension(info.fname))
        {
            Log("    skip: not .bin");
            continue;
        }
        /* Anything that cannot be a valid image is not worth a key. */
        if (info.fsize < 8 || info.fsize > sizeof(fw_image))
        {
            Log("    skip: size out of range (max %lu)",
                (unsigned long)sizeof(fw_image));
            continue;
        }

        strncpy(slots[slot_count].name, info.fname, sizeof(slots[0].name) - 1);
        slots[slot_count].name[sizeof(slots[0].name) - 1] = '\0';
        slots[slot_count].size = (uint32_t)info.fsize;
        slot_count++;
        Log("    ACCEPTED as slot %d", slot_count);
    }
    f_closedir(&dir);
    Log("directory walk saw %d entries, accepted %d", seen, slot_count);

    /* Insertion sort: tiny list, and it keeps the ordering obvious. */
    for (int i = 1; i < slot_count; i++)
    {
        Slot key = slots[i];
        int  j   = i - 1;
        while (j >= 0 && strcasecmp(slots[j].name, key.name) > 0)
        {
            slots[j + 1] = slots[j];
            j--;
        }
        slots[j + 1] = key;
    }
}

/* ------------------------------------------------------------------------
 *  Per-firmware settings
 *
 *  All three stock firmwares keep options.json and presets.json in the card
 *  root, and all three use a different format for them. Left alone they would
 *  overwrite each other every time you switched.
 *
 *  So the launcher owns them: each firmware's pair is parked in /FIRMWARE
 *  under its own name, swapped into the root just before that firmware starts,
 *  and harvested back on the next boot. last.txt remembers whose settings are
 *  currently sitting in the root.
 * ---------------------------------------------------------------------- */

static const char *kLastFile  = "FIRMWARE/last.txt";
static const char *kSettings[] = {"options.json", "presets.json"};
static constexpr int kNumSettings = 2;

/** Shared scratch for file copies. 4K keeps the settings copy quick without
 *  eating into anything; these files are only a few KB each.
 *
 *  Aligned because it is a DMA target: SDMMC transfers whole words. */
static uint8_t __attribute__((aligned(32))) copy_buf[4096];

/** Strip the ".bin" to get the stem a firmware's settings are filed under. */
static void StemOf(const char *filename, char *out, size_t out_len)
{
    strncpy(out, filename, out_len - 1);
    out[out_len - 1] = '\0';
    const size_t len = strlen(out);
    if (len > 4)
        out[len - 4] = '\0'; /* HasBinExtension() already vouched for ".bin" */
}

/** Copy one file, truncating the destination. Returns false if the source is
 *  missing, which is the normal case the first time a firmware is launched. */
static bool CopyFile(const char *src, const char *dst)
{
    static FIL fsrc, fdst;

    if (f_open(&fsrc, src, FA_OPEN_EXISTING | FA_READ) != FR_OK)
        return false;

    if (f_open(&fdst, dst, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
    {
        f_close(&fsrc);
        return false;
    }

    bool ok = true;
    for (;;)
    {
        UINT got = 0, put = 0;
        if (f_read(&fsrc, copy_buf, sizeof(copy_buf), &got) != FR_OK)
        {
            ok = false;
            break;
        }
        if (got == 0)
            break; /* end of file */
        if (f_write(&fdst, copy_buf, got, &put) != FR_OK || put != got)
        {
            ok = false;
            break;
        }
    }

    f_close(&fsrc);
    f_close(&fdst);
    return ok;
}

/** "FIRMWARE/<stem>.<settings-file>" */
static void SettingsPath(const char *stem, const char *which, char *out, size_t out_len)
{
    snprintf(out, out_len, "FIRMWARE/%s.%s", stem, which);
}

/** Save whatever is in the root back to the firmware that last ran. */
static void HarvestPreviousSettings()
{
    static FIL f;
    char       stem[64] = {0};
    UINT got      = 0;

    if (f_open(&f, kLastFile, FA_OPEN_EXISTING | FA_READ) != FR_OK)
        return; /* nothing has run yet */

    if (f_read(&f, stem, sizeof(stem) - 1, &got) == FR_OK && got > 0)
    {
        stem[got] = '\0';
        /* Trim any trailing newline a text editor may have added. */
        for (UINT i = 0; i < got; i++)
            if (stem[i] == '\r' || stem[i] == '\n')
            {
                stem[i] = '\0';
                break;
            }
    }
    f_close(&f);

    if (stem[0] == '\0')
        return;

    for (int i = 0; i < kNumSettings; i++)
    {
        char dst[96];
        SettingsPath(stem, kSettings[i], dst, sizeof(dst));
        CopyFile(kSettings[i], dst);
    }
}

/** Put a firmware's own settings in the root, and record that they are there. */
static void InstallSettingsFor(const char *stem)
{
    for (int i = 0; i < kNumSettings; i++)
    {
        char src[96];
        SettingsPath(stem, kSettings[i], src, sizeof(src));
        /* A miss is fine and expected on a firmware's first run: whatever is
           already in the root stays, and the firmware regenerates it. */
        CopyFile(src, kSettings[i]);
    }

    static FIL f;
    if (f_open(&f, kLastFile, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK)
    {
        UINT put = 0;
        f_write(&f, stem, strlen(stem), &put);
        f_close(&f);
    }
}

/** Bounce buffer for the firmware read.
 *
 *  Two separate reasons this cannot read straight into fw_image:
 *
 *  1. SDMMC's DMA will not land data in SDRAM at all. The stock firmware
 *     bounces through internal RAM for the same reason -- see
 *     FileStreamingManager's workspace_buffer and the resigned comment above
 *     it ("the copy-to/from-the-workspace is a disappointing extra step").
 *
 *  2. When a read is a whole number of sectors, FatFS hands the user buffer
 *     straight to disk_read as a multi-sector burst (ff.c: `disk_read(fs->drv,
 *     rbuff, sect, cc)`). A 32-sector burst never completes here and eats the
 *     full 30-second SD_TIMEOUT. One sector at a time is the same shape of
 *     transfer the directory reads use, and those work.
 *
 *  Aligned because it is a DMA target, and because sd_diskio.c does its cache
 *  maintenance on 32-byte boundaries around whatever address it is given.
 *
 *  4K rather than one sector: every transfer carries a fixed overhead (two
 *  cache maintenance calls and a card-state poll), and at 512 bytes a 240K
 *  image needs 470 of them. Safe now that the FIL buffers are out of DTCM. */
static constexpr uint32_t kChunkSize = 4096;
static uint8_t __attribute__((aligned(32))) bounce_buf[kChunkSize];

/** Read a firmware into the staging buffer in SDRAM, a sector at a time. */
static bool LoadImage(const Slot &slot, uint32_t *out_len)
{
    char path[80];
    snprintf(path, sizeof(path), "%s/%s", kFirmwareDir, slot.name);

    static FIL file;
    FRESULT    res = f_open(&file, path, FA_OPEN_EXISTING | FA_READ);
    Log("f_open(\"%s\") -> %d", path, (int)res);
    if (res != FR_OK)
        return false;

    /* Commit before the read starts. If the read hangs rather than failing,
       this is the last thing that will ever reach the card, and knowing the
       open succeeded narrows it to the transfer itself. */
    Log("starting read of %lu bytes in %lu-byte chunks",
        (unsigned long)slot.size, (unsigned long)kChunkSize);
    LogFlush();

    const uint32_t started = System::GetNow();
    uint32_t offset = 0;
    uint32_t chunk  = 0;
    bool     ok     = true;

    while (offset < slot.size)
    {
        uint32_t want = slot.size - offset;
        if (want > sizeof(bounce_buf))
            want = sizeof(bounce_buf);

        UINT got = 0;
        res = f_read(&file, bounce_buf, want, &got);

        /* Noisy for the first few so a working read is visible in the log too,
           then only on trouble -- a 470-chunk read would bury everything. */
        if (chunk < 3)
            Log("  chunk %lu: f_read(%lu) -> %d, got %lu",
                (unsigned long)chunk, (unsigned long)want, (int)res,
                (unsigned long)got);

        if (res != FR_OK || got != want)
        {
            Log("  FAIL at chunk %lu offset %lu: res=%d want=%lu got=%lu",
                (unsigned long)chunk, (unsigned long)offset, (int)res,
                (unsigned long)want, (unsigned long)got);
            ok = false;
            break;
        }

        memcpy(fw_image + offset, bounce_buf, got);
        offset += got;
        chunk++;

        /* Fill the keybed left to right as the image comes in. Without this a
           slow read and a dead unit look exactly the same from the outside. */
        const int lit = (int)((offset * kMaxSlots) / slot.size);
        for (int k = 0; k < kMaxSlots; k++)
            SetSmtLedFloat(kSlotLed[k], k < lit ? .9f : 0.f,
                           k < lit ? .9f : 0.f, k < lit ? .9f : 0.f);
        fill_led_data();
    }

    f_close(&file);
    Log("read loop: %lu/%lu bytes in %lu chunks, %lu ms",
        (unsigned long)offset, (unsigned long)slot.size, (unsigned long)chunk,
        (unsigned long)(System::GetNow() - started));

    if (!ok || offset != slot.size)
        return false;

    *out_len = offset;
    return true;
}

/** Breathing white on every populated key. Unpopulated keys stay dark, so the
 *  number of lit keys is the number of firmwares you have. */
static void DrawPicker(uint32_t now)
{
    /* ~2 s cycle, never fully dark so a slot is always readable. */
    const float phase = (float)(now % 2000) / 2000.f;
    const float tri   = phase < .5f ? phase * 2.f : (1.f - phase) * 2.f;
    const float level = .15f + tri * .65f;

    for (int i = 0; i < kNumSmtLeds; i++)
        SetSmtLed(i, 0, 0, 0);
    for (int i = 0; i < kNumPthLeds; i++)
        SetPthLed(i, 0, 0, 0);

    for (int i = 0; i < slot_count; i++)
        SetSmtLedFloat(kSlotLed[i], level, level, level);

    fill_led_data();
}

/** Confirmation flash on the chosen key, so a press always feels acknowledged
 *  even though loading is quick. */
static void FlashChoice(int index)
{
    for (int pulse = 0; pulse < 3; pulse++)
    {
        for (int i = 0; i < kNumSmtLeds; i++)
            SetSmtLed(i, 0, 0, 0);
        SetSmtLedFloat(kSlotLed[index], 1.f, 1.f, 1.f);
        fill_led_data();
        System::Delay(60);

        SetSmtLed(kSlotLed[index], 0, 0, 0);
        fill_led_data();
        System::Delay(60);
    }

    /* Leave it lit through the load. Dark would be indistinguishable from a
       dead unit, and a slow read is exactly when you most want to know the
       difference. */
    SetSmtLedFloat(kSlotLed[index], 1.f, 1.f, 1.f);
    fill_led_data();
}

/** Why the launcher gave up. Each gets its own colour so the fault is readable
 *  off the panel without pulling the card. */
enum class Fault
{
    NoCard,       /**< red     -- no card, or it would not mount */
    NoFirmwares,  /**< amber   -- card mounted, /FIRMWARE is empty */
    ReadFailed,   /**< magenta -- the image would not read off the card */
    BadImage,     /**< blue    -- image read, but its vector table is wrong */
    Internal,     /**< white   -- the trampoline does not fit; cannot happen */
};

/** Slow pulse, and the unit stays here rather than jumping into nothing.
 *  Commits the log on the way in -- this is where the evidence matters most. */
[[noreturn]] static void ErrorLoop(Fault fault)
{
    Log("STOP: fault %d", (int)fault);
    LogFlush();

    float r = .8f, g = 0.f, b = 0.f;
    switch (fault)
    {
        case Fault::NoCard:      r = .8f; g = 0.f;  b = 0.f;  break;
        case Fault::NoFirmwares: r = .8f; g = .35f; b = 0.f;  break;
        case Fault::ReadFailed:  r = .8f; g = 0.f;  b = .8f;  break;
        case Fault::BadImage:    r = 0.f; g = .2f;  b = .9f;  break;
        case Fault::Internal:    r = .8f; g = .8f;  b = .8f;  break;
    }

    while (true)
    {
        const uint32_t now = System::GetNow();
        const float phase  = (float)(now % 1200) / 1200.f;
        const float tri    = phase < .5f ? phase * 2.f : (1.f - phase) * 2.f;

        for (int i = 0; i < kNumSmtLeds; i++)
            SetSmtLed(i, 0, 0, 0);
        for (int i = 0; i < kNumPthLeds; i++)
            SetPthLedFloat(i, tri * r, tri * g, tri * b);

        fill_led_data();
        System::Delay(16);
        hw.LowBatteryLockoutCheck();
    }
}

/** Stop everything that writes to memory on its own before the handover. The
 *  LED driver runs off a DMA chain that would otherwise still be firing while
 *  the trampoline overwrites SRAM. */
static void ShutdownPeripherals()
{
    hw.LedsOff();
    StopLeds();
    System::Delay(20); /* let the last DMA transfer retire */

    /* libDaisy's SDMMC driver has no teardown call, but unmounting ends the
       last transfer, and the peripheral does nothing further unless asked. */
    f_mount(nullptr, "", 0);
    System::Delay(5);

    /* The same teardown the stock bootloader performs before it starts an
       application (ChompiDeInitCallback in the v6.4 bootloader's main.cpp).
       Without it the incoming firmware inherits live peripherals and DMA
       streams underneath its own initialisation.
     *
     *  seed.DeInit() runs dsy_dma_deinit(), disables both caches and deinits
     *  the timer behind System::GetNow() -- so nothing below may call
     *  System::Delay(), and this has to be the last thing we do. It leaves
     *  SDRAM alone (that teardown is commented out in libDaisy), so the
     *  staged image survives for the trampoline to copy. */
    hw.seed.StopAudio();
    hw.seed.DeInit();
}

int main(void)
{
    hw.Init();
    LedSetup();

    SdmmcHandler::Config sd_cfg;
    sd_cfg.speed = SdmmcHandler::Speed::FAST;
    sd_cfg.width = SdmmcHandler::BusWidth::BITS_4;
    sdmmc.Init(sd_cfg);

    fsi.Init(FatFSInterface::Config::MEDIA_SD);

    const char   *sd_path = fsi.GetSDPath();
    const FRESULT mres    = f_mount(&fsi.GetSDFileSystem(), sd_path, 1);

    Log("CHOMPI launcher: boot");
    Log("f_mount(\"%s\") -> %d", sd_path ? sd_path : "(null)", (int)mres);

    if (mres != FR_OK)
        ErrorLoop(Fault::NoCard); /* no card, or an unreadable one */

    /* A trampoline too large for its landing site would be truncated, and the
       failure would only show at the handover. Refuse to offer anything. */
    if (!CheckTrampolineFits())
    {
        Log("FATAL: trampoline is %lu bytes, max %lu",
            (unsigned long)TrampolineSize(), (unsigned long)kTrampolineMax);
        ErrorLoop(Fault::Internal);
    }
    Log("trampoline %lu bytes, fits", (unsigned long)TrampolineSize());

    ScanFirmwares();
    Log("scan: %d firmware(s) in /%s", slot_count, kFirmwareDir);
    for (int i = 0; i < slot_count; i++)
        Log("  key %d -> %s (%lu bytes)", i + 1, slots[i].name,
            (unsigned long)slots[i].size);

    if (slot_count == 0)
    {
        Log("nothing to launch");
        ErrorLoop(Fault::NoFirmwares);
    }

    /* Whatever ran last left its settings in the root. File them away before
       anything else can overwrite them. */
    HarvestPreviousSettings();
    Log("ready -- picker up with %d slot(s)", slot_count);
    LogFlush();

    /* Wait for every key to be released before arming, so that a key still held
       down from power-on does not immediately select a slot. */
    bool armed = false;

    while (true)
    {
        hw.ProcessAllControls();
        const uint32_t now = System::GetNow();

        bool any_down = false;
        for (int i = 0; i < slot_count; i++)
        {
            if (hw.button_sr.State(static_cast<int>(kSlotKey[i])))
            {
                any_down = true;
                if (armed)
                {
                    uint32_t length = 0;

                    Log("key %d pressed -> %s", i + 1, slots[i].name);
                    FlashChoice(i);

                    if (!LoadImage(slots[i], &length))
                    {
                        Log("FAIL: could not read %s off the card", slots[i].name);
                        ErrorLoop(Fault::ReadFailed);
                    }
                    Log("read %lu bytes into SDRAM", (unsigned long)length);

                    const uint32_t msp   = *(const uint32_t *)(fw_image + 0);
                    const uint32_t entry = *(const uint32_t *)(fw_image + 4);
                    Log("vector table: MSP=0x%08lX entry=0x%08lX",
                        (unsigned long)msp, (unsigned long)entry);

                    if (!ImageLooksValid(fw_image, length))
                    {
                        Log("FAIL: vector table rejected");
                        ErrorLoop(Fault::BadImage);
                    }

                    /* Only once the image is known good, so a bad .bin cannot
                       disturb the settings currently in the root. */
                    char stem[64];
                    StemOf(slots[i].name, stem, sizeof(stem));
                    InstallSettingsFor(stem);
                    Log("settings installed for %s", stem);

                    /* Last thing written: if the log ends here, the handover
                       itself is where it went wrong. */
                    Log("handing over to 0x%08lX -- goodbye", (unsigned long)entry);
                    LogFlush(); /* last chance; the next instruction is the jump */

                    ShutdownPeripherals();
                    ChainLoad(fw_image, length); /* does not return */
                }
            }
        }

        if (!any_down)
            armed = true;

        DrawPicker(now);
        hw.LowBatteryLockoutCheck();
        System::Delay(8);
    }
}
