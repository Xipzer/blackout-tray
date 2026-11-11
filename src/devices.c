// Device definitions: small text files that describe a motherboard fan chip or a USB controller (docs/devices.md),
// and the code that reads and drives the hardware from them. Included by blackout.c and by tools/devtest.c.
//
// Nothing in this file calls Windows. The includer supplies the host functions below, so the same code runs against
// real hardware in blackout.exe and against a simulated chip in the test. Only the definitions in use stay in memory.
#include <string.h>

#define DEV_ITEMS  16   // fans or temperatures in one definition
#define DEV_NAME   24
#define DEV_CMD    16   // bytes in a command template
#define DEV_REPORT 1024 // largest HID report, report ID included

static int hostIn(void *host, int port);
static void hostOut(void *host, int port, int value);
static void hostSlot(void *host, int slot); // config port: 0 = 2E, 1 = 4E
static void hostBars(void *host);           // called in config mode, so the host can learn the chip's I/O ranges
// Sends one output report (if any) and reads one input report (if wanted). 1 = done.
static int hostHid(void *host, const unsigned char *out, int outLen, unsigned char *in, int inLen);

enum { DEV_SIO, DEV_HID };
enum { FAM_NUVOTON, FAM_ITE, FAM_FINTEK, FAM_NCT6683, FAM_COUNT };
enum { FT_U8, FT_U16BE, FT_U16LE, FT_S16BE, FT_S16LE, FT_COUNT };
enum { CK_BYTE, CK_CH, CK_CH1, CK_DUTY, CK_DUTY255 };

typedef struct { unsigned short at, div; unsigned char type, set; } DevField; // where a value sits in a HID report
typedef struct { unsigned char byte[DEV_CMD], kind[DEV_CMD], len; } DevCmd;
typedef struct { unsigned char listen, count, match[4], matchLen, hasInvalid; unsigned invalid; } DevStream;

typedef struct {
    char name[DEV_NAME];
    unsigned short duty, mode, rpm, rpmHi, enable;
    unsigned char mask, value, enableBit;
    unsigned char control, hasEnable, hasRpm;
    DevField field;
    // While Blackout holds the fan: what the registers held before.
    unsigned char taken, savedMode, savedDuty, savedEnable;
    // HID stream: newest value and how many more reads it stays valid for.
    unsigned char fresh;
    int seen;
} DevFan;

typedef struct {
    char name[DEV_NAME];
    unsigned short reg, half;
    unsigned char halfBit, hasHalf, fresh;
    DevField field;
    int seen; // 1/100 C
} DevTemp;

typedef struct {
    char name[40], file[40];
    unsigned char type, verified, family, ldn, iolock, slot, chips, guards, fanCount, tempCount, hasId, reportId, pids;
    unsigned char chipDigits[4];
    unsigned short chip[4], base, vid, pid[8], outLen, inLen, inNeed;
    struct { unsigned short reg; unsigned char mask, value, differ; } guard[2];
    char iface[16];
    DevStream fanIn, tempIn;
    DevCmd set, rpmCmd, tempCmd;
    DevField rpmField, tempField;
    DevFan fans[DEV_ITEMS];
    DevTemp temps[DEV_ITEMS];
    void *host;
} Dev;

// Used when blackout.ini still has channels= and no file in the devices folder matches the chip: the layout every
// NCT679x shares, as Blackout drove it before definitions existed.
static const char devBuiltin[] =
    "[device]\nname=Nuvoton NCT679x\ntype=sio\nstatus=verified\nfamily=nuvoton\nchip=D4,D8\nldn=0B\niolock=1\n[fans]\n"
    "Fan 1=duty:109 mode:102 manual:F0=00 rpm:4C0\nFan 2=duty:209 mode:202 manual:F0=00 rpm:4C2\n"
    "Fan 3=duty:309 mode:302 manual:F0=00 rpm:4C4\nFan 4=duty:809 mode:802 manual:F0=00 rpm:4C6\n"
    "Fan 5=duty:909 mode:902 manual:F0=00 rpm:4C8\nFan 6=duty:A09 mode:A02 manual:F0=00 rpm:4CA\n"
    "Fan 7=duty:B09 mode:B02 manual:F0=00 rpm:4CE\n";

// ---- Reading a definition ----

static char devErr[112];
static int devLine; // 0 = the problem is not on one line

static int dvFail(const char *msg) {
    int n = 0, line = devLine, digit[6], k = 0;
    if (line) {
        for (const char *p = "line "; *p; p++) devErr[n++] = *p;
        do digit[k++] = line % 10;
        while ((line /= 10) && k < 6);
        while (k) devErr[n++] = (char)('0' + digit[--k]);
        devErr[n++] = ':', devErr[n++] = ' ';
    }
    while (*msg && n < (int)sizeof(devErr) - 1) devErr[n++] = *msg++;
    devErr[n] = 0;
    return 0;
}

static char *dvTrim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    for (e = s + strlen(s); e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'); *--e = 0);
    return s;
}

// Next piece of *p up to sep, trimmed. NULL when nothing is left.
static char *dvCut(char **p, char sep) {
    char *s = *p, *e;
    if (!s) return NULL;
    e = strchr(s, sep);
    if (e) *e = 0, *p = e + 1;
    else *p = NULL;
    return dvTrim(s);
}

// Strict numbers: the whole string, nothing else. -1 if not.
static int dvHex(const char *s, int digits) {
    int v = 0, n = 0;
    for (; *s; s++, n++) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : -1;
        if (d < 0 || n >= digits) return -1;
        v = v * 16 + d;
    }
    return n ? v : -1;
}

static int dvDec(const char *s, int max) {
    int v = 0, n = 0;
    for (; *s; s++, n++) {
        if (*s < '0' || *s > '9' || n >= 6) return -1;
        v = v * 10 + *s - '0';
    }
    return n && v <= max ? v : -1;
}

static int dvSame(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static int dvText(char *dst, int cap, const char *src) {
    int n = (int)strlen(src);
    if (!n || n >= cap) return 0;
    memcpy(dst, src, n + 1);
    return 1;
}

// "u16be@2 /100"
static int dvField(char *v, DevField *f) {
    static const char *const types[FT_COUNT] = {"u8", "u16be", "u16le", "s16be", "s16le"};
    char *spec = dvCut(&v, ' '), *at = strchr(spec, '@');
    int type = 0, off, div = 1;
    if (!at) return dvFail("a field is type@offset, such as u16be@2");
    *at = 0;
    while (type < FT_COUNT && strcmp(spec, types[type])) type++;
    off = dvDec(at + 1, DEV_REPORT - 2);
    if (type == FT_COUNT || off < 0) return dvFail("a field is type@offset: u8, u16be, u16le, s16be or s16le");
    if (v && *(v = dvTrim(v)) && (*v != '/' || (div = dvDec(dvTrim(v + 1), 10000)) < 1)) return dvFail("after a field only /divisor may follow");
    f->at = (unsigned short)off, f->div = (unsigned short)div, f->type = (unsigned char)type, f->set = 1;
    return 1;
}

// "23 {ch} {duty}"
static int dvCmd(char *v, DevCmd *c) {
    static const char *const holes[4] = {"{ch}", "{ch1}", "{duty}", "{duty255}"};
    char *tok;
    c->len = 0;
    while ((tok = dvCut(&v, ' '))) {
        int k = 0, b;
        if (!*tok) continue;
        if (c->len == DEV_CMD) return dvFail("command is too long");
        while (k < 4 && strcmp(tok, holes[k])) k++;
        b = k < 4 ? 0 : dvHex(tok, 2);
        if (b < 0) return dvFail("a command is hex bytes and {ch} {ch1} {duty} {duty255}");
        c->byte[c->len] = (unsigned char)b, c->kind[c->len++] = (unsigned char)(k < 4 ? k + 1 : CK_BYTE);
    }
    return c->len ? 1 : dvFail("empty command");
}

// "21 {ch} > u16be@2"
static int dvAsk(char *v, DevCmd *c, DevField *f) {
    char *reply = strchr(v, '>');
    if (!reply) return dvFail("expected command > field");
    *reply = 0;
    return dvCmd(v, c) && dvField(dvTrim(reply + 1), f);
}

static int dvList(char *v, unsigned short *out, unsigned char *digits, int max, int minDigits) {
    char *tok;
    int n = 0;
    while ((tok = dvCut(&v, ','))) {
        int len = (int)strlen(tok), id = dvHex(tok, 4);
        if (n == max || id < 0 || len < minDigits) return 0;
        if (digits) digits[n] = (unsigned char)len;
        out[n++] = (unsigned short)id;
    }
    return n;
}

static int dvDevice(Dev *d, const char *key, char *v, unsigned *seen) {
    static const char *const families[FAM_COUNT] = {"nuvoton", "ite", "fintek", "nct6683"};
    int n;
    if (!strcmp(key, "name")) return dvText(d->name, sizeof(d->name), v) ? (*seen |= 1, 1) : dvFail("name is empty or longer than 39 characters");
    if (!strcmp(key, "type")) {
        if (strcmp(v, "sio") && strcmp(v, "hid")) return dvFail("type is sio or hid");
        d->type = *v == 's' ? DEV_SIO : DEV_HID;
        return *seen |= 2, 1;
    }
    if (!strcmp(key, "status")) {
        if (strcmp(v, "verified") && strcmp(v, "untested")) return dvFail("status is verified or untested");
        d->verified = *v == 'v';
        return *seen |= 4, 1;
    }
    if (!strcmp(key, "tested") || !strcmp(key, "source")) return 1;
    if (!strcmp(key, "family")) {
        for (n = 0; n < FAM_COUNT && strcmp(v, families[n]); n++);
        if (n == FAM_COUNT) return dvFail("family is nuvoton, ite, fintek or nct6683");
        d->family = (unsigned char)n;
        return *seen |= 8, 1;
    }
    if (!strcmp(key, "chip")) return (d->chips = (unsigned char)dvList(v, d->chip, d->chipDigits, 4, 2)) ? 1 : dvFail("chip is 1 to 4 IDs of 2 to 4 hex digits");
    if (!strcmp(key, "ldn")) return (n = dvHex(v, 2)) < 0 ? dvFail("ldn is a hex byte") : (d->ldn = (unsigned char)n, *seen |= 16, 1);
    if (!strcmp(key, "iolock")) return (n = dvDec(v, 1)) < 0 ? dvFail("iolock is 0 or 1") : (d->iolock = (unsigned char)n, 1);
    if (!strcmp(key, "guard")) {
        char *tok;
        while ((tok = dvCut(&v, ','))) {
            char *m = strchr(tok, ':'), *e = m ? strchr(m, '=') : NULL;
            int differ = e && e > m + 1 && e[-1] == '!', reg, mask, value;
            if (!e || d->guards == 2) return dvFail("guard is up to two of reg:mask=value or reg:mask!=value");
            *m = 0, e[-differ] = 0;
            reg = dvHex(tok, 3), mask = dvHex(m + 1, 2), value = dvHex(e + 1, 2);
            if (reg < 0 || mask < 0 || value < 0) return dvFail("guard is up to two of reg:mask=value or reg:mask!=value");
            d->guard[d->guards].reg = (unsigned short)reg, d->guard[d->guards].mask = (unsigned char)mask;
            d->guard[d->guards].value = (unsigned char)value, d->guard[d->guards++].differ = (unsigned char)differ;
        }
        return 1;
    }
    if (!strcmp(key, "vid")) return (n = dvHex(v, 4)) < 0 ? dvFail("vid is 4 hex digits") : (d->vid = (unsigned short)n, *seen |= 32, 1);
    if (!strcmp(key, "pid")) return (d->pids = (unsigned char)dvList(v, d->pid, NULL, 8, 4)) ? 1 : dvFail("pid is 1 to 8 IDs of 4 hex digits");
    if (!strcmp(key, "interface")) {
        if (*v && !dvText(d->iface, sizeof(d->iface), v)) return dvFail("interface is longer than 15 characters");
        for (char *c = d->iface; *c; c++)
            if (*c >= 'A' && *c <= 'Z') *c += 32;
        return 1;
    }
    if (!strcmp(key, "id")) return (n = dvHex(v, 2)) < 0 ? dvFail("id is a hex byte") : (d->reportId = (unsigned char)n, d->hasId = 1, 1);
    if (!strcmp(key, "out")) return (n = dvDec(v, DEV_REPORT - 1)) < 1 ? dvFail("out is the output report size in bytes") : (d->outLen = (unsigned short)n, 1);
    if (!strcmp(key, "in")) return (n = dvDec(v, DEV_REPORT - 1)) < 1 ? dvFail("in is the input report size in bytes") : (d->inLen = (unsigned short)n, 1);
    return dvFail("unknown key in [device]");
}

// "duty:109 mode:102 manual:F0=00 enable:13.0 rpm:4C0"
static int dvSioFan(Dev *d, DevFan *f, char *v) {
    int wide = d->family == FAM_NUVOTON || d->family == FAM_NCT6683 ? 3 : 2, got = 0, r, r2;
    char *tok;
    while ((tok = dvCut(&v, ' '))) {
        char *arg = strchr(tok, ':'), *second;
        if (!*tok) continue;
        if (!arg) return dvFail("fan fields look like duty:109");
        *arg++ = 0;
        if (!strcmp(tok, "duty") || !strcmp(tok, "mode")) {
            if ((r = dvHex(arg, wide)) < 0) return dvFail("duty and mode take a register");
            if (*tok == 'd') f->duty = (unsigned short)r, got |= 1;
            else f->mode = (unsigned short)r, got |= 2;
        } else if (!strcmp(tok, "manual")) {
            if (!(second = strchr(arg, '='))) return dvFail("manual is mask=value");
            *second = 0;
            if ((r = dvHex(arg, 2)) < 1 || (r2 = dvHex(second + 1, 2)) < 0 || (r2 & ~r)) return dvFail("manual is mask=value, value inside the mask");
            f->mask = (unsigned char)r, f->value = (unsigned char)r2, got |= 4;
        } else if (!strcmp(tok, "enable")) {
            if (!(second = strchr(arg, '.'))) return dvFail("enable is register.bit");
            *second = 0;
            if ((r = dvHex(arg, wide)) < 0 || (r2 = dvDec(second + 1, 7)) < 0) return dvFail("enable is register.bit");
            f->enable = (unsigned short)r, f->enableBit = (unsigned char)(1 << r2), f->hasEnable = 1;
        } else if (!strcmp(tok, "rpm")) {
            if ((second = strchr(arg, ','))) *second = 0;
            if ((r = dvHex(arg, wide)) < 0 || (second && (r2 = dvHex(second + 1, wide)) < 0)) return dvFail("rpm takes a register, or low,high on ite");
            f->rpm = (unsigned short)r, f->hasRpm = 1;
            if (second) f->rpmHi = (unsigned short)r2, f->hasRpm = 2;
        } else {
            return dvFail("unknown fan field");
        }
    }
    if (got && got != 7) return dvFail("duty, mode and manual go together");
    f->control = got == 7;
    if (!f->control && f->hasEnable) return dvFail("enable needs duty, mode and manual");
    if (!f->control && !f->hasRpm) return dvFail("a fan needs rpm, or duty, mode and manual");
    if (f->control && d->family != FAM_NUVOTON && d->family != FAM_ITE) return dvFail("fans of this family are read only: leave out duty, mode and manual");
    if (f->hasRpm && (f->hasRpm == 2) != (d->family == FAM_ITE)) return dvFail(d->family == FAM_ITE ? "ite rpm is rpm:low,high" : "rpm takes one register here");
    return 1;
}

// "150,151.7"
static int dvSioTemp(Dev *d, DevTemp *t, char *v) {
    int wide = d->family == FAM_NUVOTON || d->family == FAM_NCT6683 ? 3 : 2, r, r2, bit;
    char *half = strchr(v, ','), *dot = half ? strchr(half, '.') : NULL;
    if (half) *half = 0;
    if ((r = dvHex(dvTrim(v), wide)) < 0) return dvFail("a temperature is a register, or register,register.bit");
    t->reg = (unsigned short)r;
    if (!half) return 1;
    if (!dot) return dvFail("a temperature is a register, or register,register.bit");
    *dot = 0;
    if ((r2 = dvHex(dvTrim(half + 1), wide)) < 0 || (bit = dvDec(dvTrim(dot + 1), 7)) < 0) return dvFail("a temperature is a register, or register,register.bit");
    t->half = (unsigned short)r2, t->halfBit = (unsigned char)bit, t->hasHalf = 1;
    return 1;
}

// Keys shared by [fans] and [temps] of a HID definition. 1 = taken, 0 = error, -1 = not one of them.
static int dvStream(DevStream *s, const char *key, char *v) {
    int n;
    if (!strcmp(key, "listen")) return (n = dvDec(v, 1)) < 0 ? dvFail("listen is 0 or 1") : (s->listen = (unsigned char)n, 1);
    if (!strcmp(key, "count")) return (n = dvDec(v, DEV_ITEMS)) < 1 ? dvFail("count is 1 to 16") : (s->count = (unsigned char)n, 1);
    if (!strcmp(key, "invalid")) return (n = dvHex(v, 4)) < 0 ? dvFail("invalid is a hex value") : (s->invalid = (unsigned)n, s->hasInvalid = 1, 1);
    if (!strcmp(key, "match")) {
        char *tok;
        s->matchLen = 0;
        while ((tok = dvCut(&v, ' '))) {
            if (!*tok) continue;
            if (s->matchLen == 4 || (n = dvHex(tok, 2)) < 0) return dvFail("match is 1 to 4 hex bytes");
            s->match[s->matchLen++] = (unsigned char)n;
        }
        return 1;
    }
    return -1;
}

static void dvNumbered(char *name, const char *word, int n) {
    int k = 0;
    while (*word) name[k++] = *word++;
    name[k++] = ' ';
    if (n >= 10) name[k++] = (char)('0' + n / 10);
    name[k++] = (char)('0' + n % 10);
    name[k] = 0;
}

static int dvEnd(const DevField *f) { return f->set ? f->at + (f->type == FT_U8 ? 1 : 2) : 0; }

// Fills d from the text of one definition file (which is cut up in the process). 0 = rejected, devErr says why.
static int devParse(Dev *d, char *text, const char *file) {
    enum { S_NONE, S_DEVICE, S_FANS, S_TEMPS } sect = S_NONE;
    unsigned seen = 0;
    int r, need = 0, cmdMax;
    memset(d, 0, sizeof(*d));
    devLine = 0;
    if (!dvText(d->file, sizeof(d->file), file)) return dvFail("file name is longer than 39 characters");
    while (text) {
        char *line = dvCut(&text, '\n'), *v, *note = strchr(line, ';');
        devLine++;
        if (note) *note = 0, line = dvTrim(line);
        if (!*line) continue;
        if (*line == '[') {
            sect = !strcmp(line, "[device]") ? S_DEVICE : !strcmp(line, "[fans]") ? S_FANS : !strcmp(line, "[temps]") ? S_TEMPS : S_NONE;
            if (sect == S_NONE) return dvFail("sections are [device], [fans] and [temps]");
            if (sect != S_DEVICE && (seen & 2) == 0) return dvFail("[device] with type= comes first");
            if (sect != S_DEVICE && d->type == DEV_SIO && !(seen & 8)) return dvFail("family= comes before [fans] and [temps]");
            continue;
        }
        if (!(v = strchr(line, '=')) || sect == S_NONE) return dvFail("expected key=value inside a section");
        *v++ = 0, line = dvTrim(line), v = dvTrim(v);
        if (sect == S_DEVICE) {
            if (!dvDevice(d, line, v, &seen)) return 0;
        } else if (sect == S_FANS) {
            if (d->type == DEV_HID) {
                if ((r = dvStream(&d->fanIn, line, v)) >= 0) {
                    if (!r) return 0;
                    continue;
                }
                if (!strcmp(line, "set")) {
                    if (!dvCmd(v, &d->set)) return 0;
                    continue;
                }
                if (!strcmp(line, "rpm")) {
                    if (!dvAsk(v, &d->rpmCmd, &d->rpmField)) return 0;
                    continue;
                }
            }
            if (d->fanCount == DEV_ITEMS) return dvFail("more than 16 fans");
            if (!dvText(d->fans[d->fanCount].name, DEV_NAME, line)) return dvFail("name is empty or longer than 23 characters");
            if (!(d->type == DEV_HID ? dvField(v, &d->fans[d->fanCount].field) : dvSioFan(d, &d->fans[d->fanCount], v))) return 0;
            d->fanCount++;
        } else {
            if (d->type == DEV_HID) {
                if ((r = dvStream(&d->tempIn, line, v)) >= 0) {
                    if (!r) return 0;
                    continue;
                }
                if (!strcmp(line, "read")) {
                    if (!dvAsk(v, &d->tempCmd, &d->tempField)) return 0;
                    continue;
                }
            }
            if (d->tempCount == DEV_ITEMS) return dvFail("more than 16 temperatures");
            if (!dvText(d->temps[d->tempCount].name, DEV_NAME, line)) return dvFail("name is empty or longer than 23 characters");
            if (!(d->type == DEV_HID ? dvField(v, &d->temps[d->tempCount].field) : dvSioTemp(d, &d->temps[d->tempCount], v))) return 0;
            d->tempCount++;
        }
    }
    devLine = 0;
    if ((seen & 7) != 7) return dvFail("[device] needs name, type and status");
    if (d->type == DEV_SIO) {
        if (!(seen & 16) || !d->chips) return dvFail("a sio definition needs family, chip and ldn");
        if (d->vid || d->pids || d->hasId || d->outLen || d->inLen) return dvFail("vid, pid, id, out and in belong to type=hid");
        if (!d->fanCount && !d->tempCount) return dvFail("no fans and no temperatures");
        return 1;
    }
    if (!(seen & 32) || !d->pids) return dvFail("a hid definition needs vid and pid");
    if (seen & 24 || d->chips || d->guards) return dvFail("family, chip, ldn and guard belong to type=sio");
    // Fans: named fields from a stream, or count= with commands.
    if (d->fanIn.listen ? d->fanIn.count || d->rpmCmd.len : d->fanCount) return dvFail("[fans]: listen=1 with named fields, or count= with set= and rpm=");
    if (!d->fanIn.listen && d->fanIn.count && !d->set.len && !d->rpmCmd.len) return dvFail("[fans]: count= needs set= or rpm=");
    if (!d->fanIn.listen && !d->fanIn.count && (d->set.len || d->rpmCmd.len)) return dvFail("[fans]: set= and rpm= need count=");
    if (!d->fanIn.listen)
        for (d->fanCount = d->fanIn.count, r = 0; r < d->fanCount; r++) dvNumbered(d->fans[r].name, "Fan", r + 1);
    for (r = 0; r < d->fanCount; r++) {
        d->fans[r].control = d->set.len != 0, d->fans[r].hasRpm = d->fanIn.listen || d->rpmCmd.len;
        if (dvEnd(&d->fans[r].field) > need) need = dvEnd(&d->fans[r].field);
    }
    if (d->tempIn.listen ? d->tempIn.count || d->tempCmd.len : d->tempCount) return dvFail("[temps]: listen=1 with named fields, or count= with read=");
    if (!d->tempIn.listen && !d->tempIn.count != !d->tempCmd.len) return dvFail("[temps]: count= and read= go together");
    if (!d->tempIn.listen)
        for (d->tempCount = d->tempIn.count, r = 0; r < d->tempCount; r++) dvNumbered(d->temps[r].name, "Temp", r + 1);
    for (r = 0; r < d->tempCount; r++)
        if (dvEnd(&d->temps[r].field) > need) need = dvEnd(&d->temps[r].field);
    if (!d->fanCount && !d->tempCount) return dvFail("no fans and no temperatures");
    if (dvEnd(&d->rpmField) > need) need = dvEnd(&d->rpmField);
    if (dvEnd(&d->tempField) > need) need = dvEnd(&d->tempField);
    if (!d->inLen) return dvFail("in= is missing");
    if (need > d->inLen + 1) return dvFail("a field lies beyond the input report (in= bytes plus the report ID)");
    d->inNeed = (unsigned short)need;
    cmdMax = d->set.len > d->rpmCmd.len ? d->set.len : d->rpmCmd.len;
    if (d->tempCmd.len > cmdMax) cmdMax = d->tempCmd.len;
    if (cmdMax && (!d->hasId || !d->outLen)) return dvFail("commands need id= and out=");
    if (cmdMax > d->outLen) return dvFail("a command is longer than the output report (out=)");
    return 1;
}

// A fan or temperature named in blackout.ini: its number from 1, or its name. -1 = not in this definition.
static int devFindFan(const Dev *d, const char *ref) {
    int n = dvDec(ref, DEV_ITEMS);
    if (n >= 1 && n <= d->fanCount) return n - 1;
    for (n = 0; n < d->fanCount; n++)
        if (dvSame(d->fans[n].name, ref)) return n;
    return -1;
}

static int devFindTemp(const Dev *d, const char *ref) {
    int n = dvDec(ref, DEV_ITEMS);
    if (n >= 1 && n <= d->tempCount) return n - 1;
    for (n = 0; n < d->tempCount; n++)
        if (dvSame(d->temps[n].name, ref)) return n;
    return -1;
}

// ---- Super I/O chips ----

static int cfgIn(void *h, int port, int reg) {
    hostOut(h, port, reg);
    return hostIn(h, port + 1);
}

static void cfgOut(void *h, int port, int reg, int v) {
    hostOut(h, port, reg);
    hostOut(h, port + 1, v);
}

static int cfgId(void *h, int port) {
    int hi = cfgIn(h, port, 0x20);
    return hi << 8 | cfgIn(h, port, 0x21);
}

// The definition for a chip ID: the longest chip= prefix wins. ite picks between ITE and the 87 87 families.
static int sioMatch(const Dev *defs, int n, int id, int ite) {
    int best = -1, digits = 0;
    for (int i = 0; i < n; i++) {
        if (defs[i].type != DEV_SIO || (defs[i].family == FAM_ITE) != ite) continue;
        for (int k = 0; k < defs[i].chips; k++)
            if (defs[i].chipDigits[k] > digits && id >> (16 - 4 * defs[i].chipDigits[k]) == defs[i].chip[k]) best = i, digits = defs[i].chipDigits[k];
    }
    return best;
}

// In config mode: where the hardware monitor lives. 0 = nowhere usable.
static int sioSetup(void *h, int port, const Dev *d) {
    int base, lock;
    cfgOut(h, port, 0x07, d->ldn);
    base = cfgIn(h, port, 0x60) << 8;
    base |= cfgIn(h, port, 0x61);
    if (d->iolock && ((lock = cfgIn(h, port, 0x28)) & 0x10)) cfgOut(h, port, 0x28, lock & ~0x10);
    hostBars(h);
    base &= ~7; // some Fintek chips report the base with the index offset already added
    return base >= 0x100 && base <= 0xFFF0 ? base : 0;
}

// Looks for a chip behind one config port and readies the matching definition (base, slot, host). Returns its index
// or -1; *id is the chip ID seen, 0 for none. The order and the key sequences are LibreHardwareMonitor's: Winbond,
// Nuvoton and Fintek first (87 87, left with AA), then ITE. An ITE chip is asked for its ID before anything is sent,
// because some boards leave the second chip (port 4E) in config mode and it must then be left exactly so; the ITE
// exit is never sent to port 4E.
static int sioProbe(void *h, int slot, Dev *defs, int n, int *id) {
    int port = slot ? 0x4E : 0x2E, seen, first, m, base = 0, entered = 0;
    hostSlot(h, slot);
    hostOut(h, port, 0x87);
    hostOut(h, port, 0x87);
    seen = cfgId(h, port);
    m = sioMatch(defs, n, seen, 0);
    if (m >= 0 && defs[m].family == FAM_FINTEK) {
        int vendor = cfgIn(h, port, 0x23) << 8;
        if ((vendor | cfgIn(h, port, 0x24)) != 0x1934) m = -1;
    }
    if (m >= 0) base = sioSetup(h, port, &defs[m]);
    hostOut(h, port, 0xAA);
    if (m < 0) {
        first = seen;
        seen = cfgId(h, port);
        if (seen == 0xFFFF) {
            hostOut(h, port, 0x87);
            hostOut(h, port, 0x01);
            hostOut(h, port, 0x55);
            hostOut(h, port, slot ? 0xAA : 0x55);
            entered = 1;
            seen = cfgId(h, port);
        }
        m = sioMatch(defs, n, seen, 1);
        if (m >= 0) base = sioSetup(h, port, &defs[m]);
        if (entered && !slot && seen != 0xFFFF) cfgOut(h, port, 0x02, 0x02);
        if (seen == 0xFFFF || !seen) seen = first;
    }
    *id = seen == 0xFFFF ? 0 : seen;
    if (m < 0 || !base) return -1;
    defs[m].base = (unsigned short)base, defs[m].slot = (unsigned char)slot, defs[m].host = h;
    return m;
}

static void sioSelect(const Dev *d, int reg) {
    if (d->family == FAM_NUVOTON) {
        hostOut(d->host, d->base + 5, 0x4E); // bank select
        hostOut(d->host, d->base + 6, reg >> 8);
        hostOut(d->host, d->base + 5, reg & 0xFF);
    } else if (d->family == FAM_NCT6683) {
        hostOut(d->host, d->base + 4, 0xFF);
        hostOut(d->host, d->base + 4, reg >> 8);
        hostOut(d->host, d->base + 5, reg & 0xFF);
    } else {
        hostOut(d->host, d->base + 5, reg);
    }
}

static int sioRead(const Dev *d, int reg) {
    sioSelect(d, reg);
    return hostIn(d->host, d->base + 6);
}

static void sioWrite(const Dev *d, int reg, int v) {
    sioSelect(d, reg);
    hostOut(d->host, d->base + 6, v);
}

// ---- USB controllers ----

static int hidValue(const DevField *f, const unsigned char *b, int n, long *value, unsigned *raw) {
    int at = f->at;
    if (!f->set || dvEnd(f) > n) return 0;
    *raw = f->type == FT_U8 ? b[at] : f->type == FT_U16BE || f->type == FT_S16BE ? (unsigned)(b[at] << 8 | b[at + 1]) : (unsigned)(b[at] | b[at + 1] << 8);
    *value = f->type >= FT_S16BE ? (short)*raw : (long)*raw;
    return 1;
}

// Sends a command for channel ch. With a field, the answer is read and the field taken from it.
static int hidAsk(const Dev *d, const DevCmd *c, int ch, int percent, const DevField *f, const DevStream *s, long *value) {
    unsigned char out[DEV_REPORT] = {0}, in[DEV_REPORT];
    unsigned raw;
    // A controller that answers every command has its answer read even when nothing is wanted from it, so the next
    // answer is not a stale one. With a stream running, the stream reader takes it.
    int answered = !d->fanIn.listen && !d->tempIn.listen;
    if (!c->len) return 0;
    out[0] = d->reportId;
    for (int k = 0; k < c->len; k++) {
        static const int scale[5] = {0, 0, 0, 100, 255};
        out[1 + k] = (unsigned char)(c->kind[k] == CK_BYTE ? c->byte[k] : c->kind[k] == CK_CH ? ch : c->kind[k] == CK_CH1 ? ch + 1 : percent * scale[c->kind[k]] / 100);
    }
    if (!hostHid(d->host, out, d->outLen + 1, f || answered ? in : NULL, d->inLen + 1)) return 0;
    if (!f) return 1;
    return hidValue(f, in, d->inLen + 1, value, &raw) && !(s->hasInvalid && raw == s->invalid);
}

static int hidMatches(const DevStream *s, const unsigned char *b, int n) { return s->listen && n >= s->matchLen && !memcmp(b, s->match, s->matchLen); }

// One input report from a controller that sends them on its own: keep what the definition asks for.
static void devHidReport(Dev *d, const unsigned char *b, int n) {
    long v;
    unsigned raw;
    if (hidMatches(&d->fanIn, b, n))
        for (int i = 0; i < d->fanCount; i++)
            if (hidValue(&d->fans[i].field, b, n, &v, &raw) && !(d->fanIn.hasInvalid && raw == d->fanIn.invalid)) d->fans[i].seen = (int)(v / d->fans[i].field.div), d->fans[i].fresh = 2;
    if (hidMatches(&d->tempIn, b, n))
        for (int i = 0; i < d->tempCount; i++)
            if (hidValue(&d->temps[i].field, b, n, &v, &raw) && !(d->tempIn.hasInvalid && raw == d->tempIn.invalid)) d->temps[i].seen = (int)(v * 100 / d->temps[i].field.div), d->temps[i].fresh = 2;
}

// ---- Using a definition ----

// Is the chip in a state the definition's control registers can be trusted in (guard=)? Reads only.
static int devSafe(const Dev *d) {
    for (int i = 0; i < d->guards; i++)
        if (((sioRead(d, d->guard[i].reg) & d->guard[i].mask) == d->guard[i].value) == d->guard[i].differ) return 0;
    return 1;
}

// After a run of register accesses: leave the chip the way other software expects to find it.
static void devEnd(const Dev *d) {
    if (d->type == DEV_SIO && d->family == FAM_NUVOTON) sioSelect(d, 0);
}

// Before a motherboard fan is taken: note what its registers hold, to put back later. Reads only.
// A fan found in manual mode is either the BIOS's own setting or what an instance that was killed left behind. Only
// the caller can tell: died is what such an instance had noted (mode, duty, enable bit), or NULL. It is believed only
// while the fan is still in manual mode.
static void devFanSave(Dev *d, int i, const unsigned char *died) {
    DevFan *f = &d->fans[i];
    if (d->type != DEV_SIO || !f->control || f->taken) return;
    f->savedMode = (unsigned char)sioRead(d, f->mode);
    f->savedDuty = (unsigned char)sioRead(d, f->duty);
    f->savedEnable = f->hasEnable && (sioRead(d, f->enable) & f->enableBit);
    if (died && (f->savedMode & f->mask) == f->value) f->savedMode = died[0], f->savedDuty = died[1], f->savedEnable = died[2] != 0;
    f->taken = 1;
}

// Sets one fan to a duty in percent.
static int devFanSet(Dev *d, int i, int percent) {
    DevFan *f = &d->fans[i];
    if (!f->control) return 0;
    if (d->type == DEV_HID) return hidAsk(d, &d->set, i, percent, NULL, NULL, NULL);
    devFanSave(d, i, NULL);
    if (f->hasEnable) {
        int now = sioRead(d, f->enable);
        if (!(now & f->enableBit)) sioWrite(d, f->enable, now | f->enableBit);
    }
    sioWrite(d, f->mode, (f->savedMode & ~f->mask) | f->value);
    sioWrite(d, f->duty, percent * 255 / 100);
    return 1;
}

// Hands one fan back exactly as found. A USB controller has no BIOS curve to return to, so it goes to full speed.
static void devFanRelease(Dev *d, int i) {
    DevFan *f = &d->fans[i];
    if (d->type == DEV_HID) {
        if (f->control) hidAsk(d, &d->set, i, 100, NULL, NULL, NULL);
        return;
    }
    if (!f->taken) return;
    sioWrite(d, f->duty, f->savedDuty);
    sioWrite(d, f->mode, f->savedMode);
    if (f->hasEnable && !f->savedEnable) sioWrite(d, f->enable, sioRead(d, f->enable) & ~f->enableBit);
    f->taken = 0;
}

static int devFanRpm(Dev *d, int i) {
    DevFan *f = &d->fans[i];
    long v;
    int hi, n;
    if (!f->hasRpm) return 0;
    if (d->type == DEV_HID) {
        if (d->fanIn.listen) {
            if (!f->fresh) return 0;
            f->fresh--;
            return f->seen;
        }
        return hidAsk(d, &d->rpmCmd, i, 0, &d->rpmField, &d->fanIn, &v) ? (int)(v / d->rpmField.div) : 0;
    }
    if (d->family == FAM_ITE) {
        n = sioRead(d, f->rpm);
        n |= sioRead(d, f->rpmHi) << 8;
        return n > 0x3F && n < 0xFFFF ? 1350000 / (n * 2) : 0; // a count
    }
    hi = sioRead(d, f->rpm);
    n = hi << 8 | sioRead(d, f->rpm + 1);
    if (d->family == FAM_FINTEK) return n > 0 && n < 0xFFF ? 1500000 / n : 0; // a count
    return n;
}

// Degrees C. 0 = no reading.
static int devTempRead(Dev *d, int i, double *c) {
    DevTemp *t = &d->temps[i];
    long v;
    if (d->type == DEV_HID) {
        if (!d->tempIn.listen) {
            if (!hidAsk(d, &d->tempCmd, i, 0, &d->tempField, &d->tempIn, &v)) return 0;
            *c = (double)v / d->tempField.div;
            return 1;
        }
        if (!t->fresh) return 0;
        t->fresh--;
        *c = t->seen / 100.0;
        return 1;
    }
    *c = (signed char)sioRead(d, t->reg);
    if (t->hasHalf && (sioRead(d, t->half) >> t->halfBit & 1)) *c += 0.5;
    return 1;
}
