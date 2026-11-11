// Checks every definition in devices/ without any hardware. Built and run by build.sh (native, not for Windows):
//
//   devtest devices/*.ini
//
// It includes the same devices.c that blackout.exe uses and gives it a simulated Super I/O chip and a fake HID
// transport in place of real ports. For each file it checks that:
//   - the file is plain ASCII, parses, and claims no chip or USB ID that another file claims;
//   - the chip is found behind config port 2E and behind 4E, with the right key sequence and nothing else written;
//   - reading fans and temperatures writes no register;
//   - taking a fan, changing its duty and handing it back leaves every register exactly as it was, also when the
//     fan was found in manual mode, and when an instance that was killed had left it there;
//   - USB definitions build their commands and decode their reports as written.
// It also replays the fan control Blackout shipped before definitions existed and requires the same port traffic.
#include <stdio.h>
#include <stdlib.h>
#include "../src/devices.c"

#define MAX_DEFS 256

// ---- Simulated chip ----

typedef struct {
    int present, family, id, port, base, ldn; // base as the chip reports it; its ports are at base with the low 3 bits cleared
    int config, keyPos, index, sel, bars, entered, exited, bank, hidx, writes;
    unsigned char lock, hwm[0x1000];
} Sim;
static Sim sim;
static int simSlot;
static const char *simWhy;
typedef struct { unsigned char in; unsigned short port; unsigned char v; } Io;
static Io trace[8192];
static int traceN;

static void simFail(const char *why) {
    if (!simWhy) simWhy = why;
}

static void record(int in, int port, int v) {
    if (traceN < 8192) trace[traceN].in = (unsigned char)in, trace[traceN].port = (unsigned short)port, trace[traceN++].v = (unsigned char)v;
}

static int simReg(void) { return sim.family == FAM_NUVOTON || sim.family == FAM_NCT6683 ? (sim.bank << 8 | sim.hidx) & 0xFFF : sim.hidx; }

// 0 = not a config port, 1 = handled
static int simConfig(int port, int write, int *v) {
    if ((port & ~1) != 0x2E && (port & ~1) != 0x4E) return 0;
    if ((port & ~1) != (simSlot ? 0x4E : 0x2E)) return simFail("config port of the slot that is not selected"), 1;
    if (!sim.present || (port & ~1) != sim.port) {
        if (!write) *v = 0xFF;
        return 1;
    }
    if (!write) {
        *v = 0xFF;
        if (!(port & 1) || !sim.config) return 1;
        switch (sim.index) {
        case 0x20: *v = sim.id >> 8; break;
        case 0x21: *v = sim.id & 0xFF; break;
        case 0x23: *v = sim.family == FAM_FINTEK ? 0x19 : 0; break;
        case 0x24: *v = sim.family == FAM_FINTEK ? 0x34 : 0; break;
        case 0x07: *v = sim.sel; break;
        case 0x28: *v = sim.lock; break;
        case 0x60: *v = sim.sel == sim.ldn ? sim.base >> 8 : 0; break;
        case 0x61: *v = sim.sel == sim.ldn ? sim.base & 0xFF : 0; break;
        default: *v = 0;
        }
        return 1;
    }
    if (!(port & 1)) {
        if (sim.config) {
            if (sim.family != FAM_ITE && *v == 0xAA) sim.config = 0, sim.exited++;
            else sim.index = *v;
            return 1;
        }
        int ite = sim.family == FAM_ITE, len = ite ? 4 : 2;
        int want = !ite ? 0x87 : sim.keyPos == 0 ? 0x87 : sim.keyPos == 1 ? 0x01 : sim.keyPos == 2 ? 0x55 : sim.port == 0x4E ? 0xAA : 0x55;
        if (*v != want) sim.keyPos = *v == 0x87;
        else if (++sim.keyPos == len) sim.config = 1, sim.entered++, sim.keyPos = 0;
        return 1;
    }
    if (!sim.config) return 1;
    if (sim.index == 0x07) sim.sel = *v;
    else if (sim.index == 0x28) sim.lock = (unsigned char)*v;
    else if (sim.index == 0x02 && sim.family == FAM_ITE && (*v & 2)) sim.config = 0, sim.exited++;
    else simFail("write to a config register other than 07, 28 or the ITE exit");
    return 1;
}

static int hostIn(void *h, int port) {
    int v = 0xFF;
    (void)h;
    if (!simConfig(port, 0, &v)) {
        if (!sim.bars || (port & ~7) != (sim.base & ~7)) simFail("read from a port outside the chip's range, or before find_bars");
        else if ((port & 7) != 6) simFail("read from a port that is not the data port");
        else v = sim.family == FAM_NUVOTON && sim.hidx == 0x4E ? sim.bank : sim.hwm[simReg()];
    }
    record(1, port, v);
    return v;
}

static void hostOut(void *h, int port, int v) {
    (void)h;
    record(0, port, v);
    if (simConfig(port, 1, &v)) return;
    if (!sim.bars || (port & ~7) != (sim.base & ~7)) {
        simFail("write to a port outside the chip's range, or before find_bars");
    } else if ((port & 7) == 5) {
        sim.hidx = v;
    } else if ((port & 7) == 4 && sim.family == FAM_NCT6683) {
        if (v != 0xFF) sim.bank = v;
    } else if ((port & 7) == 6) {
        if (sim.family == FAM_NUVOTON && sim.hidx == 0x4E) sim.bank = v;
        else sim.hwm[simReg()] = (unsigned char)v, sim.writes++;
    } else {
        simFail("write to a port that is neither index nor data");
    }
}

static void hostSlot(void *h, int slot) {
    (void)h;
    simSlot = slot, sim.bars = 0;
}

static void hostBars(void *h) {
    (void)h;
    if (!sim.present || !sim.config) simFail("find_bars outside config mode");
    else sim.bars = 1;
}

// ---- Fake HID transport ----

static unsigned char hidOut[DEV_REPORT], hidReply[DEV_REPORT];
static int hidOutLen, hidInLen, hidCalls, hidWantedReply;

static int hostHid(void *h, const unsigned char *out, int outLen, unsigned char *in, int inLen) {
    (void)h;
    hidCalls++, hidOutLen = outLen, hidInLen = inLen, hidWantedReply = in != NULL;
    memcpy(hidOut, out, outLen);
    if (in) memcpy(in, hidReply, inLen);
    return 1;
}

// ---- Definitions under test ----

static Dev defs[MAX_DEFS];
static char *texts[MAX_DEFS], names[MAX_DEFS][64];
static int count, failures;
static const char *current = "";

static void failed(const char *what) {
    printf("FAIL %s: %s\n", current, what);
    failures++;
}

#define CHECK(cond, what) \
    do { \
        if (!(cond)) failed(what); \
    } while (0)

// Parses everything afresh, so no test sees state left by another. 0 if any file is rejected.
static int loadAll(int report) {
    int ok = 1;
    for (int i = 0; i < count; i++) {
        char *copy = malloc(strlen(texts[i]) + 1);
        strcpy(copy, texts[i]);
        if (!devParse(&defs[i], copy, names[i])) {
            ok = 0;
            if (report) current = names[i], failed(devErr);
        }
        free(copy);
    }
    return ok;
}

static void simReset(const Dev *d, int id, int slot, int inConfig) {
    memset(&sim, 0, sizeof(sim));
    sim.present = 1, sim.family = d->family, sim.id = id, sim.port = slot ? 0x4E : 0x2E, sim.ldn = d->ldn, sim.lock = 0x10;
    sim.base = d->family == FAM_FINTEK ? 0x295 : d->family == FAM_ITE ? 0xA40 : 0x290; // Fintek: the +5 quirk
    sim.config = inConfig;
    for (int r = 0; r < 0x1000; r++) sim.hwm[r] = (unsigned char)(r * 37 + (r >> 8) * 11 + 5);
    simWhy = NULL, traceN = 0;
}

static int expectRpm(const Dev *d, const DevFan *f) {
    int n;
    if (!f->hasRpm) return 0;
    if (d->family == FAM_ITE) {
        n = sim.hwm[f->rpm] | sim.hwm[f->rpmHi] << 8;
        return n > 0x3F && n < 0xFFFF ? 1350000 / (n * 2) : 0;
    }
    n = sim.hwm[f->rpm] << 8 | sim.hwm[f->rpm + 1];
    return d->family == FAM_FINTEK ? (n > 0 && n < 0xFFF ? 1500000 / n : 0) : n;
}

// Put the guard registers in the state the definition asks for (or its opposite).
static void setGuards(const Dev *d, int satisfied) {
    for (int g = 0; g < d->guards; g++) {
        unsigned char *r = &sim.hwm[d->guard[g].reg], mask = d->guard[g].mask, value = d->guard[g].value;
        *r = (unsigned char)((*r & ~mask) | ((satisfied != 0) != (d->guard[g].differ != 0) ? value : (value ^ mask) & mask));
    }
}

static void testSio(int m, int id, int slot, int inConfig) {
    static unsigned char before[0x1000];
    Dev *d;
    int seen = -1, found, bank0;
    double c;
    if (!loadAll(0)) return;
    d = &defs[m];
    simReset(d, id, slot, inConfig);

    // Not behind the other port.
    found = sioProbe(NULL, !slot, defs, count, &seen);
    CHECK(found < 0 && !seen, "found a chip behind the port that has none");
    CHECK(!simWhy, simWhy);
    CHECK(sim.config == inConfig && !sim.entered && !sim.exited, "probing the empty port touched the chip");

    found = sioProbe(NULL, slot, defs, count, &seen);
    CHECK(found == m, "the chip was not matched to this file");
    CHECK(seen == id, "wrong chip ID reported");
    CHECK(!simWhy, simWhy);
    if (found != m) return;
    CHECK(d->base == (sim.base & ~7) && d->slot == slot, "wrong base address or slot");
    if (inConfig) CHECK(sim.config && !sim.entered && !sim.exited, "a chip found in config mode must be left in it, with no key sent");
    else if (d->family == FAM_ITE && slot) CHECK(sim.entered == 1 && !sim.exited, "ITE at 4E: enter once, never exit");
    else CHECK(!sim.config && sim.entered == 1 && sim.exited == 1, "the chip must be entered once and left again");
    CHECK(sim.lock == (d->iolock ? 0x00 : 0x10), "iolock: bit 4 of register 28 handled wrongly");

    // Reading never writes.
    setGuards(d, 1);
    memcpy(before, sim.hwm, sizeof(before));
    for (int i = 0; i < d->fanCount; i++) CHECK(devFanRpm(d, i) == expectRpm(d, &d->fans[i]), "RPM decoded wrongly");
    for (int i = 0; i < d->tempCount; i++) {
        const DevTemp *t = &d->temps[i];
        double want = (signed char)before[t->reg] + (t->hasHalf && (before[t->half] >> t->halfBit & 1) ? 0.5 : 0);
        CHECK(devTempRead(d, i, &c) && c == want, "temperature decoded wrongly");
    }
    CHECK(devSafe(d), "guard not satisfied when it should be");
    devEnd(d);
    CHECK(!sim.writes && !memcmp(before, sim.hwm, sizeof(before)), "reading wrote a register");
    bank0 = d->family != FAM_NUVOTON || sim.bank == 0;
    CHECK(bank0, "bank not returned to 0");
    if (d->guards) {
        setGuards(d, 0);
        CHECK(!devSafe(d), "guard satisfied when it should not be");
        setGuards(d, 1);
    }

    // Take, change, hand back: one fan at a time, from the BIOS's automatic mode.
    for (int i = 0; i < d->fanCount; i++) {
        DevFan *f = &d->fans[i];
        if (!f->control) {
            CHECK(!devFanSet(d, i, 50), "a read-only fan accepted a duty");
            continue;
        }
        sim.hwm[f->mode] = (unsigned char)((sim.hwm[f->mode] & ~f->mask) | (~f->value & f->mask));
        if (f->hasEnable) sim.hwm[f->enable] &= (unsigned char)~f->enableBit;
        memcpy(before, sim.hwm, sizeof(before));
        CHECK(devFanSet(d, i, 40), "set failed");
        CHECK(sim.hwm[f->mode] == ((before[f->mode] & ~f->mask) | f->value), "mode register not put in manual");
        CHECK(sim.hwm[f->duty] == 102, "duty register is not 40%");
        if (f->hasEnable) CHECK(sim.hwm[f->enable] == (before[f->enable] | f->enableBit), "enable bit not set");
        for (int r = 0; r < 0x1000; r++)
            if (r != f->mode && r != f->duty && !(f->hasEnable && r == f->enable) && sim.hwm[r] != before[r]) {
                failed("taking a fan changed a register it does not own");
                break;
            }
        CHECK(devFanSet(d, i, 100) && sim.hwm[f->duty] == 255, "duty register is not 100%");
        devFanRelease(d, i);
        devEnd(d);
        CHECK(!memcmp(before, sim.hwm, sizeof(before)), "handing a fan back did not restore every register");

        // Found in manual mode with nothing noted: that is the BIOS's own setting, and it is handed back as found.
        sim.hwm[f->mode] = (unsigned char)((before[f->mode] & ~f->mask) | f->value);
        sim.hwm[f->duty] = 0x33;
        if (f->hasEnable) sim.hwm[f->enable] |= f->enableBit;
        devFanSet(d, i, 60);
        devFanRelease(d, i);
        CHECK(sim.hwm[f->mode] == ((before[f->mode] & ~f->mask) | f->value) && sim.hwm[f->duty] == 0x33, "a fan the BIOS runs in manual mode was not handed back as found");
        // Same, but an instance that was killed had noted what the BIOS had: that is what goes back.
        {
            unsigned char died[3] = {before[f->mode], before[f->duty], 0};
            devFanSave(d, i, died);
            devFanSet(d, i, 60);
            devFanRelease(d, i);
            devEnd(d);
            CHECK(!memcmp(before, sim.hwm, sizeof(before)), "a fan left behind by a killed instance was not handed back as the BIOS had it");
        }
        // A note is not believed once the fan is out of manual mode again (the BIOS reset the chip).
        {
            unsigned char died[3] = {0x5A, 0xA5, 1};
            devFanSave(d, i, died);
            devFanSet(d, i, 60);
            devFanRelease(d, i);
            devEnd(d);
            CHECK(!memcmp(before, sim.hwm, sizeof(before)), "a stale note was believed although the fan was not in manual mode");
        }
    }

    // All fans together, handed back in the opposite order (they can share an enable register).
    for (int i = 0; i < d->fanCount; i++)
        if (d->fans[i].control) sim.hwm[d->fans[i].mode] = (unsigned char)((sim.hwm[d->fans[i].mode] & ~d->fans[i].mask) | (~d->fans[i].value & d->fans[i].mask));
    memcpy(before, sim.hwm, sizeof(before));
    for (int i = 0; i < d->fanCount; i++)
        if (d->fans[i].control) devFanSet(d, i, 55);
    for (int i = d->fanCount - 1; i >= 0; i--) devFanRelease(d, i);
    devEnd(d);
    CHECK(!memcmp(before, sim.hwm, sizeof(before)), "taking and returning all fans did not restore every register");
    CHECK(!simWhy, simWhy);
}

static void put16(unsigned char *b, const DevField *f, unsigned v) {
    if (f->type == FT_U8) b[f->at] = (unsigned char)v;
    else if (f->type == FT_U16BE || f->type == FT_S16BE) b[f->at] = (unsigned char)(v >> 8), b[f->at + 1] = (unsigned char)v;
    else b[f->at] = (unsigned char)v, b[f->at + 1] = (unsigned char)(v >> 8);
}

static void testHid(int m) {
    static unsigned char report[DEV_REPORT], owner[DEV_REPORT];
    Dev *d;
    double c;
    if (!loadAll(0)) return;
    d = &defs[m];
    CHECK(d->fanCount || d->tempCount, "nothing to read");

    // Stream: every field gets its own value, in bytes no other field uses.
    memset(report, 0, sizeof(report)), memset(owner, 0, sizeof(owner));
    for (int pass = 0; pass < 2; pass++) {
        const DevStream *s = pass ? &d->tempIn : &d->fanIn;
        if (!s->listen) continue;
        for (int i = 0; i < (pass ? d->tempCount : d->fanCount); i++) {
            const DevField *f = pass ? &d->temps[i].field : &d->fans[i].field;
            for (int b = f->at; b < dvEnd(f); b++) {
                CHECK(!owner[b] && b >= s->matchLen, "two fields (or a field and match=) share a byte");
                owner[b] = 1;
            }
            put16(report, f, f->type == FT_U8 ? 20u + i : 1000u + 100u * i + pass);
        }
        memcpy(report, s->match, s->matchLen);
    }
    devHidReport(d, report, d->inLen + 1);
    if (d->fanIn.listen)
        for (int i = 0; i < d->fanCount; i++) {
            int raw = d->fans[i].field.type == FT_U8 ? 20 + i : 1000 + 100 * i;
            CHECK(devFanRpm(d, i) == raw / d->fans[i].field.div, "stream RPM decoded wrongly");
        }
    if (d->tempIn.listen)
        for (int i = 0; i < d->tempCount; i++) {
            int raw = d->temps[i].field.type == FT_U8 ? 20 + i : 1001 + 100 * i;
            CHECK(devTempRead(d, i, &c) && (int)(c * 100 + 0.5) == raw * 100 / d->temps[i].field.div, "stream temperature decoded wrongly");
        }
    if (d->tempIn.listen && d->tempCount) {
        CHECK(devTempRead(d, 0, &c), "a stream value should last two reads");
        CHECK(!devTempRead(d, 0, &c), "a stream value must go stale when no new report arrives");
        if (d->tempIn.matchLen) {
            report[0] ^= 0xFF;
            devHidReport(d, report, d->inLen + 1);
            CHECK(!devTempRead(d, 0, &c), "a report that does not match was taken");
            report[0] ^= 0xFF;
        }
        if (d->tempIn.hasInvalid && d->temps[0].field.type != FT_U8) {
            put16(report, &d->temps[0].field, d->tempIn.invalid);
            devHidReport(d, report, d->inLen + 1);
            CHECK(!devTempRead(d, 0, &c), "the invalid= value was taken as a reading");
        }
        devHidReport(d, report, 1);
        CHECK(!devTempRead(d, 0, &c), "a short report was read past its end");
    }

    // Commands.
    if (d->set.len) {
        int listening = d->fanIn.listen || d->tempIn.listen;
        hidCalls = 0;
        CHECK(devFanSet(d, d->fanCount - 1, 40) && hidCalls == 1, "set sent nothing");
        CHECK(hidOutLen == d->outLen + 1 && hidOut[0] == d->reportId, "set: wrong report size or ID");
        CHECK(hidWantedReply == !listening, "set: the answer must be read unless a stream is running");
        for (int k = d->set.len + 1; k < hidOutLen; k++) CHECK(!hidOut[k], "set: bytes after the command are not zero");
        devFanRelease(d, 0);
        CHECK(hidCalls == 2, "release sent nothing");
    } else if (d->fanCount) {
        hidCalls = 0;
        CHECK(!devFanSet(d, 0, 40) && !hidCalls, "a controller without set= accepted a duty");
        devFanRelease(d, 0);
        CHECK(!hidCalls, "release wrote to a controller without set=");
    }
    if (d->rpmCmd.len) {
        memset(hidReply, 0, sizeof(hidReply));
        put16(hidReply, &d->rpmField, 1200);
        CHECK(devFanRpm(d, 0) == 1200 / d->rpmField.div && hidInLen == d->inLen + 1, "rpm command decoded wrongly");
    }
    if (d->tempCmd.len) {
        memset(hidReply, 0, sizeof(hidReply));
        put16(hidReply, &d->tempField, 3150);
        CHECK(devTempRead(d, 0, &c) && (int)(c * 100 + 0.5) == 315000 / d->tempField.div, "temperature command decoded wrongly");
    }
}

// The Commander Pro's protocol is known byte for byte (liquidctl, Linux corsair-cpro): check against that, not
// against what the parser made of the file.
static void testCommanderPro(void) {
    double c;
    int m = -1;
    if (!loadAll(0)) return;
    for (int i = 0; i < count; i++)
        if (!strcmp(names[i], "corsair-commander-pro")) m = i;
    if (m < 0) return;
    current = "corsair-commander-pro (known bytes)";
    devFanSet(&defs[m], 2, 40);
    CHECK(hidOutLen == 65 && hidOut[0] == 0 && hidOut[1] == 0x23 && hidOut[2] == 2 && hidOut[3] == 40 && !hidOut[4], "set fan 3 to 40% is not 00 23 02 28");
    memset(hidReply, 0, sizeof(hidReply));
    hidReply[2] = 0x04, hidReply[3] = 0xB0;
    CHECK(devFanRpm(&defs[m], 5) == 1200 && hidOut[1] == 0x21 && hidOut[2] == 5, "rpm of fan 6 is not 00 21 05, answer at bytes 2 and 3");
    hidReply[2] = 0x0C, hidReply[3] = 0x4E;
    CHECK(devTempRead(&defs[m], 1, &c) && c == 31.5 && hidOut[1] == 0x11 && hidOut[2] == 1, "temperature 2 is not 00 11 01, 1/100 C at bytes 2 and 3");
}

// ---- The fan control Blackout shipped before definitions (commit a3fb290), kept here as the reference ----

static const int oldPage[7] = {0x100, 0x200, 0x300, 0x800, 0x900, 0xA00, 0xB00};
static unsigned char oldMode[7], oldDuty[7];
static int oldBase, oldManual;

static void oldSelect(int reg) {
    hostOut(NULL, oldBase + 5, 0x4E);
    hostOut(NULL, oldBase + 6, reg >> 8);
    hostOut(NULL, oldBase + 5, reg & 0xFF);
}
static int oldRead(int reg) {
    oldSelect(reg);
    return hostIn(NULL, oldBase + 6);
}
static void oldWrite(int reg, int v) {
    oldSelect(reg);
    hostOut(NULL, oldBase + 6, v);
}
static int oldCfgIn(int port, int reg) {
    hostOut(NULL, port, reg);
    return hostIn(NULL, port + 1);
}
static void oldInit(void) {
    int port = 0x2E, id, lock, hi;
    hostSlot(NULL, 0);
    hostOut(NULL, port, 0x87);
    hostOut(NULL, port, 0x87);
    id = oldCfgIn(port, 0x20);
    if (id == 0xD4 || id == 0xD8) {
        hostOut(NULL, port, 0x07), hostOut(NULL, port + 1, 0x0B);
        hi = oldCfgIn(port, 0x60);
        oldBase = hi << 8 | oldCfgIn(port, 0x61);
        lock = oldCfgIn(port, 0x28);
        if (lock & 0x10) hostOut(NULL, port, 0x28), hostOut(NULL, port + 1, lock & ~0x10);
        hostBars(NULL);
    }
    hostOut(NULL, port, 0xAA);
}
static void oldSet(const int *ch, int n, int duty) {
    for (int i = 0; i < n; i++) {
        int page = oldPage[ch[i]];
        if (!oldManual) {
            oldMode[i] = (unsigned char)oldRead(page + 0x02);
            oldDuty[i] = (unsigned char)oldRead(page + 0x09);
            if (!(oldMode[i] & 0xF0)) oldMode[i] |= 0x40, oldDuty[i] = 0xFF;
        }
        oldWrite(page + 0x02, oldMode[i] & 0x0F);
        oldWrite(page + 0x09, duty * 255 / 100);
    }
    oldSelect(0);
    oldManual = 1;
}
static void oldRelease(const int *ch, int n) {
    for (int i = 0; i < n; i++) {
        oldWrite(oldPage[ch[i]] + 0x09, oldDuty[i]);
        oldWrite(oldPage[ch[i]] + 0x02, oldMode[i]);
    }
    oldSelect(0);
    oldManual = 0;
}
static void oldRpm(const int *ch, int n) {
    for (int i = 0; i < n; i++) {
        int reg = ch[i] == 6 ? 0x4CE : 0x4C0 + ch[i] * 2;
        oldRead(reg), oldRead(reg + 1);
    }
    oldSelect(0);
}

// Same chip, same starting registers, same calls: the old code and a definition must produce the same port traffic.
// The one allowed difference: chip detection now also reads ID register 21. (The old code also guessed that a fan
// found in manual mode was left by a crash and switched it to automatic; that guess is gone, so it is not replayed.)
static void testLegacy(const char *file) {
    static const int ch[3] = {1, 3, 4}; // channels=1,3,4 is outputs=sio/2,sio/4,sio/5
    static Io oldTrace[8192];
    static unsigned char image[0x1000];
    int oldN, m, seen, k = 0, same;
    Dev probe;
    current = file;
    if (!loadAll(0)) return;
    probe = defs[0];
    probe.family = FAM_NUVOTON, probe.ldn = 0x0B;
    simReset(&probe, 0xD42A, 0, 0);
    sim.base = 0x2A0;
    for (int i = 0; i < 3; i++) sim.hwm[oldPage[ch[i]] + 2] = 0x44;
    memcpy(image, sim.hwm, sizeof(image));
    oldManual = 0;
    oldInit();
    oldSet(ch, 3, 40), oldSet(ch, 3, 55), oldRpm(ch, 3), oldRelease(ch, 3), oldSet(ch, 3, 80), oldRelease(ch, 3);
    oldN = traceN;
    memcpy(oldTrace, trace, sizeof(trace));

    simReset(&probe, 0xD42A, 0, 0);
    sim.base = 0x2A0;
    memcpy(sim.hwm, image, sizeof(image));
    m = sioProbe(NULL, 0, defs, count, &seen);
    CHECK(m >= 0 && !strcmp(names[m], file), "D42A is not matched to this definition");
    if (m < 0) return;
    for (int round = 0; round < 6; round++) {
        Dev *d = &defs[m];
        for (int i = 0; i < 3; i++) {
            if (round == 0) devFanSet(d, ch[i], 40);
            if (round == 1) devFanSet(d, ch[i], 55);
            if (round == 2) devFanRpm(d, ch[i]);
            if (round == 3 || round == 5) devFanRelease(d, ch[i]);
            if (round == 4) devFanSet(d, ch[i], 80);
        }
        devEnd(d);
    }
    // Drop the read of register 21 (one write of the index, one read) from the new trace, then compare.
    for (int i = 0; i < traceN; i++) {
        if (i + 1 < traceN && !trace[i].in && trace[i].port == 0x2E && trace[i].v == 0x21 && trace[i + 1].in) {
            i++;
            continue;
        }
        trace[k++] = trace[i];
    }
    same = k == oldN && !memcmp(trace, oldTrace, k * sizeof(*trace));
    CHECK(same, "port traffic differs from the old fan control");
    CHECK(!memcmp(sim.hwm, image, sizeof(image)), "registers after hand-back are not as they started");
    CHECK(!simWhy, simWhy);
}

int main(int argc, char **argv) {
    int sio = 0, hid = 0;
    for (int a = 1; a < argc && count < MAX_DEFS - 1; a++) {
        const char *path = argv[a], *base = strrchr(path, '/'), *dot;
        FILE *f = fopen(path, "rb");
        long size;
        base = base ? base + 1 : path;
        dot = strrchr(base, '.');
        current = base;
        if (!f || !dot || strcmp(dot, ".ini") || dot - base > 39) {
            failed("cannot open, not .ini, or the name is longer than 39 characters");
            continue;
        }
        fseek(f, 0, SEEK_END), size = ftell(f), fseek(f, 0, SEEK_SET);
        texts[count] = calloc(1, (size_t)size + 1);
        if (fread(texts[count], 1, (size_t)size, f) != (size_t)size) failed("read error");
        fclose(f);
        memcpy(names[count], base, (size_t)(dot - base));
        for (const char *c = names[count]; *c; c++)
            if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) {
                failed("file names use a to z, 0 to 9 and - only");
                break;
            }
        for (long i = 0; i < size; i++)
            if ((unsigned char)texts[count][i] >= 0x80 || texts[count][i] == '\t' || !texts[count][i]) {
                failed("plain ASCII only, no tabs (Blackout reads files up to 16384 bytes)");
                break;
            }
        if (size > 16384) failed("larger than 16384 bytes");
        count++;
    }
    // The built-in fallback goes last, as in blackout.exe.
    texts[count] = malloc(sizeof(devBuiltin));
    memcpy(texts[count], devBuiltin, sizeof(devBuiltin));
    strcpy(names[count++], "builtin");
    if (!loadAll(1)) return printf("%d problem%s\n", failures, failures == 1 ? "" : "s"), 1;

    // No two files may claim the same chip or the same USB device.
    for (int i = 0; i < count; i++)
        for (int j = i + 1; j < count; j++) {
            current = names[j];
            if (defs[i].type != defs[j].type) continue;
            if (defs[i].type == DEV_SIO) {
                for (int a = 0; a < defs[i].chips; a++)
                    for (int b = 0; b < defs[j].chips; b++)
                        if (defs[i].chip[a] == defs[j].chip[b] && defs[i].chipDigits[a] == defs[j].chipDigits[b] && (defs[i].family == FAM_ITE) == (defs[j].family == FAM_ITE)) {
                            printf("FAIL %s: chip %X is also claimed by %s\n", names[j], defs[i].chip[a], names[i]), failures++;
                        }
            } else if (defs[i].vid == defs[j].vid && !strcmp(defs[i].iface, defs[j].iface)) {
                for (int a = 0; a < defs[i].pids; a++)
                    for (int b = 0; b < defs[j].pids; b++)
                        if (defs[i].pid[a] == defs[j].pid[b]) printf("FAIL %s: USB %04X:%04X is also claimed by %s\n", names[j], defs[i].vid, defs[i].pid[a], names[i]), failures++;
            }
        }

    for (int i = 0; i < count; i++) {
        Dev d = defs[i]; // loadAll() in each test rewrites defs
        current = names[i];
        if (d.type == DEV_HID) {
            hid++;
            testHid(i);
            continue;
        }
        sio++;
        for (int k = 0; k < d.chips; k++) {
            int shift = 16 - 4 * d.chipDigits[k], id = d.chip[k] << shift;
            // A short prefix stands for a range: test an ID in it that no longer prefix claims.
            loadAll(0);
            while (sioMatch(defs, count, id, d.family == FAM_ITE) != i && (id + 1) >> shift == d.chip[k]) id++;
            for (int slot = 0; slot < 2; slot++) {
                testSio(i, id, slot, 0);
                if (d.family == FAM_ITE) testSio(i, id, slot, 1); // left in config mode by the BIOS
            }
        }
    }
    testCommanderPro();
    testLegacy("nuvoton-nct6796d-r");
    // Without the devices folder the built-in definition takes over and must behave the same.
    sio += hid, texts[0] = texts[count - 1], strcpy(names[0], "builtin"), count = 1;
    testLegacy("builtin");

    if (failures) return printf("%d problem%s\n", failures, failures == 1 ? "" : "s"), 1;
    printf("devices: %d definitions OK (%d fan chips, %d USB controllers), old fan control reproduced exactly\n", sio - 1, sio - hid - 1, hid);
    return 0;
}
