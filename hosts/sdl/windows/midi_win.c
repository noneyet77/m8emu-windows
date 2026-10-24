/* External MIDI devices on Windows (WinMM), routed to the M8's USB MIDI.
 *
 * In: every MIDI input (keyboards, controllers, interfaces) is opened, and
 *     its channel, clock and transport messages go to the M8. Active sensing
 *     is dropped; SysEx is not routed.
 * Out: what the M8 sends goes to every MIDI output except Windows' built-in
 *     software synth (so M8 notes do not play through the PC's GS synth).
 * Devices are rescanned every two seconds, so plugging a device in or out
 * while the emulator runs works. --midi-in/--midi-out NAME restrict routing
 * to devices whose name contains NAME ("none" disables that direction). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include "midi_win.h"
#include "win_utf8.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#define MAX_DEVICES 32
typedef struct { HMIDIIN in; char name[128]; } input_port;
typedef struct { HMIDIOUT out; char name[128]; } output_port;

static m8_worker *routed;
static input_port inputs[MAX_DEVICES];
static output_port outputs[MAX_DEVICES];
static unsigned input_count, output_count;
static char in_filter[128], out_filter[128];
static unsigned long long fingerprint;
static uint64_t next_scan;

static unsigned message_length(unsigned char status)
{
    if (status < 0x80) return 0;
    if (status < 0xf0) return (status & 0xe0) == 0xc0 ? 2 : 3;   /* Cx, Dx: 2 bytes */
    switch (status) {
    case 0xf1: case 0xf3: return 2;
    case 0xf2: return 3;
    case 0xf6: case 0xf8: case 0xfa: case 0xfb: case 0xfc: case 0xff: return 1;
    default: return 0;   /* SysEx, undefined, active sensing (0xfe) */
    }
}
static void CALLBACK input_callback(HMIDIIN in, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR param2)
{
    (void)in; (void)instance; (void)param2;
    if (message != MIM_DATA || !routed) return;
    unsigned char m[3] = {(unsigned char)(param1 & 0xff), (unsigned char)(param1 >> 8 & 0x7f),
                          (unsigned char)(param1 >> 16 & 0x7f)};
    unsigned n = message_length(m[0]);
    if (n) m8_worker_midi_in(routed, m, n);
}

static int wanted(const char *name, const char *filter)
{
    if (!*filter) return 1;
    if (!_stricmp(filter, "none")) return 0;
    char a[128], b[128];
    snprintf(a, sizeof a, "%s", name); snprintf(b, sizeof b, "%s", filter);
    _strlwr(a); _strlwr(b);
    return strstr(a, b) != NULL;
}
static void close_all(void)
{
    for (unsigned i = 0; i < input_count; ++i) { midiInStop(inputs[i].in); midiInReset(inputs[i].in); midiInClose(inputs[i].in); }
    for (unsigned i = 0; i < output_count; ++i) { midiOutReset(outputs[i].out); midiOutClose(outputs[i].out); }
    input_count = output_count = 0;
}
static unsigned long long scan_fingerprint(void)
{
    unsigned long long h = 1469598103934665603ULL;
    UINT ni = midiInGetNumDevs(), no = midiOutGetNumDevs();
    for (UINT i = 0; i < ni; ++i) {
        MIDIINCAPSW caps;
        if (midiInGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR)
            for (const WCHAR *p = caps.szPname; *p; ++p) { h ^= *p; h *= 1099511628211ULL; }
        h ^= 0x100; h *= 1099511628211ULL;
    }
    for (UINT i = 0; i < no; ++i) {
        MIDIOUTCAPSW caps;
        if (midiOutGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR) {
            for (const WCHAR *p = caps.szPname; *p; ++p) { h ^= *p; h *= 1099511628211ULL; }
            h ^= caps.wTechnology;
        }
        h ^= 0x200; h *= 1099511628211ULL;
    }
    return h;
}
static void open_all(void)
{
    UINT ni = midiInGetNumDevs(), no = midiOutGetNumDevs();
    for (UINT i = 0; i < ni && input_count < MAX_DEVICES; ++i) {
        MIDIINCAPSW caps;
        if (midiInGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
        char *name = u8_narrow(caps.szPname);
        if (!name) continue;
        input_port *p = &inputs[input_count];
        snprintf(p->name, sizeof p->name, "%s", name);
        free(name);
        if (!wanted(p->name, in_filter)) continue;
        MMRESULT r = midiInOpen(&p->in, i, (DWORD_PTR)input_callback, 0, CALLBACK_FUNCTION);
        if (r == MMSYSERR_NOERROR && midiInStart(p->in) == MMSYSERR_NOERROR) {
            printf("midi_in=\"%s\"\n", p->name);
            ++input_count;
        } else {
            printf("midi_in_unavailable=\"%s\" (in use by another program?)\n", p->name);
            if (r == MMSYSERR_NOERROR) midiInClose(p->in);
        }
    }
    for (UINT i = 0; i < no && output_count < MAX_DEVICES; ++i) {
        MIDIOUTCAPSW caps;
        if (midiOutGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
        if (caps.wTechnology == MOD_SWSYNTH || caps.wTechnology == MOD_SYNTH) continue;  /* GS Wavetable */
        char *name = u8_narrow(caps.szPname);
        if (!name) continue;
        output_port *p = &outputs[output_count];
        snprintf(p->name, sizeof p->name, "%s", name);
        free(name);
        if (!wanted(p->name, out_filter)) continue;
        if (midiOutOpen(&p->out, i, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) {
            printf("midi_out=\"%s\"\n", p->name);
            ++output_count;
        } else printf("midi_out_unavailable=\"%s\" (in use by another program?)\n", p->name);
    }
    fflush(stdout);
}

void m8_midi_start(m8_worker *worker, const char *input_filter, const char *output_filter)
{
    snprintf(in_filter, sizeof in_filter, "%s", input_filter ? input_filter : "");
    snprintf(out_filter, sizeof out_filter, "%s", output_filter ? output_filter : "");
    routed = worker;
    fingerprint = scan_fingerprint();
    open_all();
    next_scan = SDL_GetTicks() + 2000;
}
void m8_midi_poll(void)
{
    if (!routed) return;
    unsigned char m[4];
    size_t n;
    while ((n = m8_worker_midi_out(routed, m, sizeof m)) > 0) {
        DWORD packed = m[0] | (n > 1 ? (DWORD)m[1] << 8 : 0) | (n > 2 ? (DWORD)m[2] << 16 : 0);
        for (unsigned i = 0; i < output_count; ++i) midiOutShortMsg(outputs[i].out, packed);
    }
    uint64_t now = SDL_GetTicks();
    if (now >= next_scan) {
        next_scan = now + 2000;
        unsigned long long f = scan_fingerprint();
        if (f != fingerprint) {               /* a device was plugged in or out */
            fingerprint = f;
            close_all();
            open_all();
        }
    }
}
void m8_midi_stop(void)
{
    if (!routed) return;
    close_all();                              /* no callbacks run after midiInClose */
    routed = NULL;
}
