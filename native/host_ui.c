/* PC harness: renders one scene with the app's real drawing code (ui.h) and writes a
 * PPM, so the screen can be checked without the console.
 *   cl /nologo /O2 /TC /I build\native /Fe:host_ui.exe native\host_ui.c   (or make host-ui)
 *   host_ui scene.txt out.ppm
 * scene.txt is key=value lines (see apply()). Addresses are truncated to the host's
 * 32-bit `unsigned long`; scenes use addresses that stay distinct after that. */
#include "ui.h"

static struct ui_ctx ctx;
static char pin_text[5] = "0000";

static void apply(const char *k, const char *v) {
    /* context (input-dependent) */
    if (!strcmp(k, "screen")) ctx.screen = atoi(v);
    else if (!strcmp(k, "focus")) ctx.focus = atoi(v);
    else if (!strcmp(k, "top")) ctx.top = atoi(v);
    else if (!strcmp(k, "online")) ctx.online = atoi(v);
    else if (!strcmp(k, "busy")) ctx.busy = atoi(v);
    else if (!strcmp(k, "confirm_circle")) ctx.confirm_circle = atoi(v);
    else if (!strcmp(k, "menu")) ctx.menu = atoi(v);
    else if (!strcmp(k, "menu_focus")) ctx.menu_focus = atoi(v);
    else if (!strcmp(k, "dialog")) ctx.dialog = atoi(v);
    else if (!strcmp(k, "dialog_focus")) ctx.dialog_focus = atoi(v);
    else if (!strcmp(k, "theme")) { int t = atoi(v); ui_theme = t >= 0 && t < 3 ? t : 0; }
    else if (!strcmp(k, "pin")) ctx.pinmode = atoi(v);
    else if (!strcmp(k, "pin_text")) snprintf(pin_text, sizeof(pin_text), "%s", v);
    else if (!strcmp(k, "pin_digit")) ctx.digit = atoi(v);
    /* state (what the service would report) */
    else if (!strcmp(k, "link")) state.link = atoi(v);
    else if (!strcmp(k, "volume")) state.volume = atoi(v);
    else if (!strcmp(k, "tv")) state.tv = atoi(v);
    else if (!strcmp(k, "scanning")) state.scanning = atoi(v);
    else if (!strcmp(k, "pin_pending")) state.pin_pending = atoi(v);
    else if (!strcmp(k, "test")) state.test = atoi(v);
    else if (!strcmp(k, "lan")) state.lan = atoi(v);
    else if (!strcmp(k, "pair_code")) snprintf(state.pair_code, sizeof(state.pair_code), "%s", v);
    else if (!strcmp(k, "ip")) snprintf(state.ip, sizeof(state.ip), "%s", v);
    else if (!strcmp(k, "attempts")) state.attempts = atoi(v);
    else if (!strcmp(k, "retry_ms")) state.retry_ms = atoi(v);
    else if (!strcmp(k, "meter")) state.meter = atoi(v);
    else if (!strcmp(k, "quiet_ms")) state.quiet_ms = atoi(v);
    else if (!strcmp(k, "connect_rc")) state.connect_rc = atoi(v);
    else if (!strcmp(k, "audio_rc")) state.audio_rc = atoi(v);
    else if (!strcmp(k, "latency_mode")) state.latency_mode = atoi(v);
    else if (!strcmp(k, "drops")) state.drops = atoi(v);
    else if (!strcmp(k, "underruns")) state.underruns = atoi(v);
    else if (!strcmp(k, "selected")) state.selected = (unsigned long)strtoull(v, 0, 16);
    else if (!strcmp(k, "name")) snprintf(state.name, sizeof(state.name), "%s", v);
    else if (!strcmp(k, "state")) snprintf(state.conn_state, sizeof(state.conn_state), "%s", v);
    else if (!strcmp(k, "message")) snprintf(state.message, sizeof(state.message), "%s", v);
    else if (!strcmp(k, "t")) g_now = (uint64_t)atoll(v) * 1000u;
    else if (!strcmp(k, "notice")) snprintf(notice, sizeof(notice), "%s", v);
    else if (!strcmp(k, "kernel")) snprintf(state.kernel, sizeof(state.kernel), "%s", v);
    else if (!strcmp(k, "kernel_caps")) state.kernel_caps = atoi(v);
    else if (!strcmp(k, "kernel_sigs")) state.kernel_sigs = atoi(v);
    else if (!strcmp(k, "fw_version")) state.fw_version = (unsigned long)strtoull(v, 0, 0);
    else if (!strcmp(k, "dev")) {
        struct ui_device *d = &state.devices[state.count];
        const char *bar = strchr(v, '|');
        if (!bar || state.count >= UI_DEVICE_LIMIT) return;
        size_t n = (size_t)(bar - v); if (n > 95) n = 95;
        memcpy(d->name, v, n); d->name[n] = 0;
        d->address = (unsigned long)strtoull(bar + 1, 0, 16);
        const char *bar2 = strchr(bar + 1, '|');
        d->audio = bar2 ? atoi(bar2 + 1) : 1;
        /* dev=name|address|audio|known|found|rssi; a scene's devices are remembered and
         * answered the search unless it says 0, at -60 dBm unless it gives one */
        const char *bar3 = bar2 ? strchr(bar2 + 1, '|') : 0;
        d->known = bar3 ? atoi(bar3 + 1) : 1;
        const char *bar4 = bar3 ? strchr(bar3 + 1, '|') : 0;
        d->found = bar4 ? atoi(bar4 + 1) : 1;
        const char *bar5 = bar4 ? strchr(bar4 + 1, '|') : 0;
        d->rssi = bar5 ? atoi(bar5 + 1) : -60;
        state.count++;
    }
}
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: host_ui scene.txt out.ppm\n"); return 2; }
    FILE *f = fopen(argv[1], "r"); if (!f) { fprintf(stderr, "cannot open scene\n"); return 2; }
    ctx.online = 1;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line); while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        char *eq = strchr(line, '='); if (!eq) continue; *eq = 0; apply(line, eq + 1);
    }
    fclose(f);
    ctx.pin = pin_text;
    notice_until = notice[0] ? g_now + 1000000u : 0;
    draw(&ctx);
    FILE *o = fopen(argv[2], "wb"); if (!o) return 3;
    fprintf(o, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int i = 0; i < WIDTH*HEIGHT; i++) { unsigned char px[3]; px[0] = (unsigned char)(back[i]>>16); px[1] = (unsigned char)(back[i]>>8); px[2] = (unsigned char)back[i]; fwrite(px, 1, 3, o); }
    fclose(o); return 0;
}
