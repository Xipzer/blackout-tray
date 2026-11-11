// Read-only Nuvoton Super I/O probe: chip ID, HWM base, and per-channel fan mode, PWM and RPM.
// Needs PawnIO and LpcIO.bin, run elevated. Usage: sioprobe.exe <LpcIO.bin>
#include <windows.h>
#include <stdio.h>

typedef HRESULT(WINAPI *open_t)(PHANDLE);
typedef HRESULT(WINAPI *load_t)(HANDLE, const UCHAR *, SIZE_T);
typedef HRESULT(WINAPI *exec_t)(HANDLE, PCSTR, const ULONG64 *, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
static exec_t px;
static HANDLE h;

static ULONG64 call(const char *fn, ULONG64 a, ULONG64 b, int nin, HRESULT *hr) {
    ULONG64 in[2] = {a, b}, out[1] = {0};
    SIZE_T n = 0;
    HRESULT r = px(h, fn, in, nin, out, 1, &n);
    if (hr) *hr = r;
    return out[0];
}
static int sioIn(int reg) { return (int)call("ioctl_superio_inb", reg, 0, 1, 0); }
static void sioOut(int reg, int v) { call("ioctl_superio_outb", reg, v, 2, 0); }
static void pOut(int port, int v) { call("ioctl_pio_outb", port, v, 2, 0); }
static int pIn(int port) { return (int)call("ioctl_pio_inb", port, 0, 1, 0); }
static int base;
static int hwm(int reg) {
    pOut(base + 5, 0x4E); pOut(base + 6, reg >> 8);
    pOut(base + 5, reg & 0xFF); return pIn(base + 6);
}

int main(int argc, char **argv) {
    HMODULE lib = LoadLibraryA("C:\\Program Files\\PawnIO\\PawnIOLib.dll");
    if (!lib || argc < 2) { puts("PawnIOLib.dll or blob path missing"); return 1; }
    open_t po = (open_t)GetProcAddress(lib, "pawnio_open");
    load_t pl = (load_t)GetProcAddress(lib, "pawnio_load");
    px = (exec_t)GetProcAddress(lib, "pawnio_execute");
    HANDLE f = CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    DWORD sz = GetFileSize(f, 0), rd;
    UCHAR *blob = HeapAlloc(GetProcessHeap(), 0, sz);
    ReadFile(f, blob, sz, &rd, 0); CloseHandle(f);
    HRESULT hr = po(&h); printf("pawnio_open %08lx\n", hr); if (hr) return 1;
    hr = pl(h, blob, sz); printf("pawnio_load %08lx\n", hr); if (hr) return 1;
    HANDLE mx = CreateMutexA(0, FALSE, "Global\\Access_ISABUS.HTP.Method");
    WaitForSingleObject(mx, 2000);
    for (int slot = 0; slot < 2; slot++) {
        int port = slot ? 0x4E : 0x2E;
        call("ioctl_select_slot", slot, 0, 1, 0);
        pOut(port, 0x87); pOut(port, 0x87);
        int id = (sioIn(0x20) << 8) | sioIn(0x21);
        printf("slot %d (0x%X): chip id %04X\n", slot, port, id);
        if (id == 0 || id == 0xFFFF) { pOut(port, 0xAA); continue; }
        sioOut(0x07, 0x0B);
        base = (sioIn(0x60) << 8) | sioIn(0x61);
        printf("  HWM base %04X  LDN0B active %d\n", base, sioIn(0x30));
        call("ioctl_find_bars", 0, 0, 0, &hr);
        printf("  find_bars %08lx\n", hr);
        pOut(port, 0xAA);
        if (!base) continue;
        static const int page[7] = {0x100, 0x200, 0x300, 0x800, 0x900, 0xA00, 0xB00};
        for (int c = 0; c < 7; c++) {
            int mode = hwm(page[c] + 0x02), pwm = hwm(page[c] + 0x09);
            int rpm = (hwm(0x4C0 + c * 2) << 8) | hwm(0x4C1 + c * 2);
            printf("  ch%d  mode %X (reg %02X)  pwm %3d (%3d%%)  rpm %d\n", c, mode >> 4, mode, pwm, pwm * 100 / 255, rpm);
        }
        printf("  bank sel back to 0\n"); pOut(base + 5, 0x4E); pOut(base + 6, 0);
    }
    ReleaseMutex(mx);
    return 0;
}
