// [wcw] SI / PIF (joybus) emulation for games that use the RAW libultra SI primitives
// (__osSiRawStartDma / __osSiDeviceBusy) instead of the high-level osCont* API.
//
// recomp.h's MEM_* macros do not trap MMIO, so the recompiled raw-SI code crashes writing
// SI_PIF_ADDR_* / reading SI_STATUS (0xA48000xx -> out of RDRAM). We instead reimplement the
// two SI primitives in C: name func_80023970 -> __osSiRawStartDma and func_800251E0 ->
// __osSiDeviceBusy in tools/gen_symbols.py, and provide the *_recomp shims here.
//
// The game builds a 64-byte PIF RAM joybus command block in RDRAM, does
// __osSiRawStartDma(OS_WRITE) (DRAM->PIF), waits for the SI message, then
// __osSiRawStartDma(OS_READ) (PIF->DRAM) to read the result. We keep a 64-byte PIF RAM,
// run the joybus on the WRITE (filling results from host input via osContGetReadData), and
// copy it back on the READ. Both directions raise the SI completion message.
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/ultra64.h"
#include "ultramodern/input.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "helpers.hpp"

#define MAXCONTROLLERS 4

static uint8_t pif_ram[64];

// [wcw] Controller Pak emulation. WCW saves to the Controller Pak in port 1 (no cart
// EEPROM/SRAM), and its homegrown raw-SI driver accesses it with joybus commands 0x02
// (read 32-byte block) / 0x03 (write 32-byte block) through the same PIF path as the
// controller reads. We back the pak's 32 KB with librecomp's save subsystem: the game's
// SaveType is Sram (also exactly 0x8000 bytes), so the pak contents live in the standard
// save buffer / save file (saves/<game id>.bin) with async persistence + backup for free.
void save_write_ptr(const void* in, uint32_t offset, uint32_t count);
void save_read_ptr(void* out, uint32_t offset, uint32_t count);

// [wcw] Hybrid pak identity for rumble support. A real N64 has ONE pak slot per controller:
// WCW saves to the Controller Pak but also supports the Rumble Pak ("Insert a Rumble Pak
// now" prompt at the title), and on hardware the player physically swaps them. Our virtual
// pak can be both: it answers as a Controller Pak by default (save filesystem in the
// <0x8000 data region), but when the game runs a Rumble Pak probe — writing a uniform
// 0x80 block to the bank/ID region, libultra osMotorInit-style — it switches identity and
// answers bank-region reads with 0x80s (Rumble Pak signature) until any other bank value
// is written (0xFE = the mempak-probe reset in osMotorInit; 0x00 = mempak bank select).
// Motor commands are pak writes to the 0xC000 region: data 0x01 = on, 0x00 = off,
// forwarded to the host controller via ultramodern::input::set_rumble. Save-region reads
// and writes are honored regardless of identity, so saves can never be lost to rumble.
static bool wcw_pak_is_rumble = false;

// Bounded diagnostic log of bank/ID-region pak traffic (dedup'd), to observe the game's
// accessory-detect and rumble-probe sequences.
static void wcw_log_pak_bank_op(char op, uint16_t addr, uint8_t value) {
    static int lines = 0;
    static uint32_t last = 0xFFFFFFFF;
    uint32_t key = ((uint32_t)op << 24) | ((uint32_t)addr << 8) | value;
    if (key == last || lines >= 64) return;
    last = key;
    lines++;
    fprintf(stderr, "[wcw][pak] %s addr=0x%04X data=0x%02X identity=%s\n",
            op == 'W' ? "write" : "read ", addr, value, wcw_pak_is_rumble ? "rumble" : "mempak");
}

// [wcw] Fresh-save Controller Pak formatting. librecomp's save buffer starts all-zero
// when no save file exists, and libultra's pak filesystem code reads a zeroed page-
// allocation table as "formatted, 0 pages free, no notes" (free pages are marked
// 0x0003) — so on a fresh install the game could never create its save note and
// showed the "Select note to be erased / 0 Pages free" screen at boot (beta QA,
// 2026-07-07). A real pak is always formatted (checksummed ID block + free-page
// table), so: on the first data-region access, if the entire 32 KB image has never
// been written, lay down an empty formatted filesystem. Byte-for-byte per
// mupen64plus's format_mempak (mempak.c) with its default device id/banks/version.
static void wcw_pak_ensure_formatted() {
    static bool checked = false;
    if (checked) { return; }
    checked = true;

    uint8_t page[256];
    for (uint32_t off = 0; off < 0x8000; off += (uint32_t)sizeof(page)) {
        save_read_ptr(page, off, (uint32_t)sizeof(page));
        for (size_t b = 0; b < sizeof(page); b++) {
            if (page[b] != 0) { return; }        // save has data — leave it alone
        }
    }

    uint8_t pages[3 * 256] = {0};

    // Page 0: ID block at +0x20 (serial, device id 0x0001, 1 bank, version 0,
    // checksum = sum of the 14 leading BE words, inverted checksum = 0xFFF2 - sum),
    // with backup copies at +0x60, +0x80, +0xC0.
    uint8_t* id = &pages[0x20];
    static const uint8_t serial[24] = {          // arbitrary fixed serial (6 BE words)
        'W', 'C', 'W', 'R', 'E', 'C', 'O', 'M',
        'P', 'I', 'L', 'E', 'D', 'P', 'A', 'K',
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    };
    memcpy(id, serial, sizeof(serial));
    id[24] = 0x00; id[25] = 0x01;                // device id
    id[26] = 0x01;                               // banks
    id[27] = 0x00;                               // version
    uint16_t sum = 0;
    for (int b = 0; b < 28; b += 2) { sum = (uint16_t)(sum + ((id[b] << 8) | id[b + 1])); }
    uint16_t isum = (uint16_t)(0xFFF2 - sum);
    id[28] = (uint8_t)(sum >> 8);  id[29] = (uint8_t)sum;
    id[30] = (uint8_t)(isum >> 8); id[31] = (uint8_t)isum;
    memcpy(&pages[0x60], id, 32);
    memcpy(&pages[0x80], id, 32);
    memcpy(&pages[0xC0], id, 32);

    // Page 1: page-allocation table — pages 5..127 marked free (0x0003), byte 1 =
    // checksum (byte sum of the 123 entries). Page 2 is its backup. Pages 3-4 (note
    // table) and the data pages stay zero.
    uint8_t* inode = &pages[0x100];
    for (int p = 5; p < 128; p++) { inode[2 * p] = 0x00; inode[2 * p + 1] = 0x03; }
    uint8_t csum = 0;
    for (int b = 2 * 5; b < 256; b++) { csum = (uint8_t)(csum + inode[b]); }
    inode[1] = csum;
    memcpy(&pages[0x200], inode, 256);

    save_write_ptr(pages, 0, (uint32_t)sizeof(pages));
    fprintf(stderr, "[wcw][pak] fresh save image - formatted empty Controller Pak filesystem\n");
}

// Standard joybus mempak data CRC (poly 0x85 over the 32 data bytes + one zero byte).
static uint8_t wcw_pak_data_crc(const uint8_t* data) {
    uint8_t crc = 0;
    for (int i = 0; i <= 32; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            uint8_t xor_tap = (crc & 0x80) ? 0x85 : 0;
            crc <<= 1;
            if (i < 32 && (data[i] & (1 << bit))) crc |= 1;
            crc ^= xor_tap;
        }
    }
    return crc;
}

extern "C" void __osSiDeviceBusy_recomp(uint8_t* rdram, recomp_context* ctx) {
    // The host SI is never busy.
    _return<s32>(ctx, 0);
}

// Execute the joybus command block currently in pif_ram, filling in result bytes.
static void wcw_process_pif() {
    OSContPad pads[MAXCONTROLLERS];
    for (int c = 0; c < MAXCONTROLLERS; c++) {
        pads[c].button = 0; pads[c].stick_x = 0; pads[c].stick_y = 0; pads[c].err_no = 0;
    }
    // [wcw fix] Latch host input NOW. osContGetReadData only reads state previously latched
    // by the poll_input callback (recompinput::poll_inputs — it caches SDL_GetKeyboardState
    // and the controller list). The normal poller is osContStartReadData, which this game
    // never calls (raw-SI path) — without this poll the cache stays empty forever and all
    // input reads as neutral.
    ultramodern::input::poll_input();
    // osContGetReadData reads current host input (already converted to N64 stick range) and
    // sets err_no != 0 for absent controllers.
    osContGetReadData(pads);

    int channel = 0;
    int i = 0;
    while (i < 63 && channel < MAXCONTROLLERS) {
        uint8_t t = pif_ram[i];
        if (t == 0xFE) { break; }                       // end of commands
        if (t == 0x00) { channel++; i++; continue; }    // skip this channel
        if (t == 0xFF) { i++; continue; }               // padding byte (channel unchanged)
        if (t & 0xC0)  { i++; continue; }               // not a valid tx count
        uint8_t rx = pif_ram[i + 1] & 0x3F;
        if (i + 2 + t + rx > 64) { break; }             // malformed / overflow guard
        uint8_t cmd = pif_ram[i + 2];
        uint8_t* res = &pif_ram[i + 2 + t];
        // [wcw] Port 1 (channel 0) is ALWAYS reported connected — a virtual controller is always
        // present (neutral input when the host pad/key is idle). osContGetReadData sets err_no != 0
        // whenever the input callback returns no response that frame, which would otherwise make
        // the game see "no controller in port 1" and refuse to advance past the title.
        bool connected = (channel == 0) || (pads[channel].err_no == 0);

        if (!connected) {
            pif_ram[i + 1] |= 0x80;                      // CHNL_ERR_NORESP: no device
        }
        else if (cmd == 0x00 || cmd == 0xFF) {           // request status
            // status byte 0x01 = Controller Pak present (port 1 only; see pak emulation above).
            if (rx >= 3) { res[0] = 0x05; res[1] = 0x00; res[2] = (channel == 0) ? 0x01 : 0x00; }
        }
        else if (cmd == 0x01) {                          // read button
            if (rx >= 4) {
                uint16_t b = pads[channel].button;
                res[0] = (uint8_t)(b >> 8);
                res[1] = (uint8_t)(b & 0xFF);
                res[2] = (uint8_t)pads[channel].stick_x;
                res[3] = (uint8_t)pads[channel].stick_y;
                // [wcw] WCW_INPUT_LOG=1: throttled per-channel activity log (multiplayer
                // verification — proves which N64 port each host device's input reaches).
                static int log_enabled = -1;
                if (log_enabled < 0) { const char* e = getenv("WCW_INPUT_LOG"); log_enabled = (e && e[0] == '1') ? 1 : 0; }
                if (log_enabled && (b != 0 || pads[channel].stick_x != 0 || pads[channel].stick_y != 0)) {
                    static uint64_t last_ms[MAXCONTROLLERS];
                    uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (now - last_ms[channel] >= 500) {
                        last_ms[channel] = now;
                        fprintf(stderr, "[wcw][input] ch%d buttons=%04X stick=(%d,%d)\n",
                                channel, b, pads[channel].stick_x, pads[channel].stick_y);
                    }
                }
            }
        }
        else if (cmd == 0x02 && channel == 0 && t >= 3 && rx >= 33) {   // pak read block
            // tx = cmd, addr_hi, addr_lo; low 5 address bits are an address CRC (ignored).
            uint16_t addr = (uint16_t)((pif_ram[i + 3] << 8) | pif_ram[i + 4]);
            uint32_t offset = addr & 0xFFE0u;
            if (offset < 0x8000) {
                wcw_pak_ensure_formatted();
                save_read_ptr(res, offset, 32);          // pak RAM <- save buffer
            }
            else {
                // Bank/ID region: answer with the current pak identity — 0x80s is the Rumble
                // Pak signature (only in its 0x8000..0x8FFF window, like real hardware),
                // zeros is the Controller Pak one.
                uint8_t fill = (wcw_pak_is_rumble && offset < 0x9000) ? 0x80 : 0x00;
                memset(res, fill, 32);
                wcw_log_pak_bank_op('R', addr, fill);
            }
            res[32] = wcw_pak_data_crc(res);
            static bool logged = false;
            if (!logged) { logged = true; fprintf(stderr, "[wcw] first Controller Pak read (addr=0x%04X)\n", addr); }
        }
        else if (cmd == 0x03 && channel == 0 && t >= 35 && rx >= 1) {   // pak write block
            uint16_t addr = (uint16_t)((pif_ram[i + 3] << 8) | pif_ram[i + 4]);
            uint32_t offset = addr & 0xFFE0u;
            const uint8_t* data = &pif_ram[i + 5];
            if (offset < 0x8000) {
                wcw_pak_ensure_formatted();              // format first so the game's write wins
                save_write_ptr(data, offset, 32);        // persists asynchronously (librecomp)
            }
            else if (offset >= 0xC000) {
                // Rumble Pak motor command: a block of 0x01s starts the motor, 0x00s stops it.
                bool on = (data[0] & 1) != 0;
                ultramodern::input::set_rumble(0, on);
                static bool logged_on = false, logged_off = false;
                bool& logged = on ? logged_on : logged_off;
                if (!logged) { logged = true; fprintf(stderr, "[wcw][pak] first motor %s (addr=0x%04X data=0x%02X)\n", on ? "ON" : "OFF", addr, data[0]); }
            }
            else {
                // Bank/ID-region write: not stored, but a uniform 0x80 block is the Rumble Pak
                // probe/init — switch the pak's identity so the probe's readback sees 0x80s.
                // Any other value (0xFE mempak-probe reset, 0x00 bank select) reverts to mempak.
                bool uniform = true;
                for (int b = 1; b < 32; b++) { if (data[b] != data[0]) { uniform = false; break; } }
                bool want_rumble = uniform && data[0] == 0x80;
                if (want_rumble != wcw_pak_is_rumble) {
                    wcw_pak_is_rumble = want_rumble;
                    if (!want_rumble) ultramodern::input::set_rumble(0, false);
                    fprintf(stderr, "[wcw][pak] identity -> %s (bank write addr=0x%04X data=0x%02X)\n",
                            want_rumble ? "RUMBLE PAK" : "CONTROLLER PAK", addr, data[0]);
                }
                wcw_log_pak_bank_op('W', addr, data[0]);
            }
            res[0] = wcw_pak_data_crc(data);
            static bool logged = false;
            if (!logged) { logged = true; fprintf(stderr, "[wcw] first Controller Pak write (addr=0x%04X)\n", addr); }
        }
        else {
            // Unsupported joybus command: report no device so the game falls back gracefully.
            pif_ram[i + 1] |= 0x80;
        }

        i += 2 + t + rx;
        channel++;
    }
}

extern "C" void __osSiRawStartDma_recomp(uint8_t* rdram, recomp_context* ctx) {
    s32 dir = (s32)ctx->r4;          // a0: OS_READ(0) = PIF->DRAM, OS_WRITE(1) = DRAM->PIF
    gpr dram_addr = ctx->r5;          // a1: RDRAM address of the 64-byte PIF block

    if (dir == 1) {
        // DRAM -> PIF: latch the command block, then run joybus so results are ready.
        for (int i = 0; i < 64; i++) {
            pif_ram[i] = (uint8_t)MEM_B(i, dram_addr);
        }
        wcw_process_pif();
    }
    else {
        // PIF -> DRAM: hand back the (already-processed) result block.
        for (int i = 0; i < 64; i++) {
            MEM_B(i, dram_addr) = (int8_t)pif_ram[i];
        }
    }

    // Each SI DMA completion raises the SI interrupt; the game blocks on osRecvMesg for it.
    ultramodern::send_si_message();

    _return<s32>(ctx, 0);
}
