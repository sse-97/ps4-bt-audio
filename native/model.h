/* UI data model and a tiny tolerant JSON reader for the home-screen app.
 *
 * The app is a client of the control API (docs/API.md): it reads GET /api/status
 * (JSON) and keeps its own small state here, rather than the service's internal
 * layout. Pure C with no console or libc-math dependencies, so the PC harness
 * (native/host_ui.c) and the host unit tests (tests/test_model.c) run this exact
 * code. The reader ignores unknown fields, so the service may add status fields
 * without the app changing.
 */
#ifndef BTA_UI_MODEL_H
#define BTA_UI_MODEL_H
#include <stddef.h>
#include <string.h>

/* Link states: the API contract (docs/API.md "link"), mirrored by the service's
 * protocol.h. The app keeps its own copy so it needs none of the service headers.
 * OFFLINE is app-only: the service isn't answering at all. */
enum {
    LINK_IDLE = 0, LINK_CONNECTING = 1, LINK_STREAMING = 2,
    LINK_LOST = 3, LINK_FAILED = 4, LINK_SEARCHING = 5, LINK_OFFLINE = 6
};

#define UI_DEVICE_LIMIT 32
struct ui_device { unsigned long address; int audio; char name[96]; };
struct ui_state {
    int link, volume, tv, scanning, pin_pending, test, lan;
    int attempts, retry_ms, meter, quiet_ms;
    int connect_rc, audio_rc;
    int latency_mode;       /* 0 lowest, 1 balanced, 2 most stable (docs/API.md) */
    int drops, underruns;   /* recent paced-send diagnostics */
    unsigned long selected;
    char name[96];          /* active/target device name */
    char conn_state[16];    /* granular link phase: idle/connecting/pairing/configuring/... */
    char message[128];      /* human-readable status line */
    char pair_code[8];      /* six digits, present only when lan is on (loopback) */
    int count;
    struct ui_device devices[UI_DEVICE_LIMIT];
};

/* ---- minimal JSON reader (depth-aware, string-escape safe) ---- */
static const char *j_ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }
/* p points at the opening quote; returns the char just past the closing quote. */
static const char *j_skipstr(const char *p) {
    p++;
    while (*p) { if (*p == '\\' && p[1]) { p += 2; continue; } if (*p == '"') return p + 1; p++; }
    return p;
}
/* Returns the char just past a complete value (string, number, object, array,
 * or bare literal), respecting nesting and quoted strings. */
static const char *j_skip(const char *p) {
    p = j_ws(p);
    if (*p == '"') return j_skipstr(p);
    if (*p == '{' || *p == '[') {
        char open = *p, close = open == '{' ? '}' : ']';
        int depth = 0;
        for (; *p; p++) {
            if (*p == '"') { p = j_skipstr(p) - 1; continue; }
            if (*p == open) depth++;
            else if (*p == close) { depth--; if (!depth) return p + 1; }
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}
/* Value of `key` directly inside the object at `obj` ('{'), or NULL. Nested
 * objects and keys inside string values are skipped, so matching is exact. */
static const char *j_member(const char *obj, const char *key) {
    const char *p = j_ws(obj);
    if (*p != '{') return 0;
    p++;
    for (;;) {
        p = j_ws(p);
        if (*p != '"') return 0;         /* '}' or malformed */
        const char *ks = p + 1, *ke = j_skipstr(p) - 1;  /* [ks,ke) = key chars */
        size_t kl = (size_t)(ke - ks);
        int match = kl == strlen(key) && !memcmp(ks, key, kl);
        p = j_ws(j_skipstr(p));
        if (*p != ':') return 0;
        p = j_ws(p + 1);
        if (match) return p;
        p = j_ws(j_skip(p));
        if (*p == ',') { p++; continue; }
        return 0;
    }
}
static long j_int(const char *obj, const char *key, long def) {
    const char *p = j_member(obj, key);
    if (!p) return def;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    if (*p < '0' || *p > '9') return def;
    long v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return neg ? -v : v;
}
/* Copy a string value into out (always NUL-terminated); unescapes \" and \\. */
static void j_str(const char *obj, const char *key, char *out, size_t cap) {
    out[0] = 0;
    const char *p = j_member(obj, key);
    if (!p || *p != '"' || cap == 0) return;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = 0;
}
static unsigned long j_mac(const char *obj, const char *key) {
    char s[16];
    j_str(obj, key, s, sizeof(s));
    unsigned long v = 0;
    for (int i = 0; s[i]; i++) {
        int d;
        char c = s[i];
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else continue;
        v = v * 16 + (unsigned)d;
    }
    return v;
}

/* Parse a /api/status body. Returns 1 on a usable object (api >= 3), else 0. */
static int model_parse(const char *body, struct ui_state *s) {
    memset(s, 0, sizeof(*s));
    const char *p = j_ws(body);
    if (*p != '{') return 0;
    if (j_int(body, "api", 0) < 3) return 0;
    s->link = (int)j_int(body, "link", LINK_IDLE);
    s->volume = (int)j_int(body, "volume", 0);
    s->tv = (int)j_int(body, "tv_enabled", 0);
    s->scanning = (int)j_int(body, "scanning", 0);
    s->pin_pending = (int)j_int(body, "pin_pending", 0);
    s->test = (int)j_int(body, "test", 0);
    s->lan = (int)j_int(body, "lan", 0);
    s->attempts = (int)j_int(body, "attempts", 0);
    s->retry_ms = (int)j_int(body, "retry_ms", 0);
    s->meter = (int)j_int(body, "meter", 0);
    s->quiet_ms = (int)j_int(body, "quiet_ms", 0);
    s->connect_rc = (int)j_int(body, "connect_rc", 0);
    s->audio_rc = (int)j_int(body, "audio_rc", 0);
    s->latency_mode = (int)j_int(body, "latency_mode", 1);
    s->drops = (int)j_int(body, "drops", 0);
    s->underruns = (int)j_int(body, "underruns", 0);
    s->selected = j_mac(body, "selected");
    j_str(body, "name", s->name, sizeof(s->name));
    j_str(body, "state", s->conn_state, sizeof(s->conn_state));
    j_str(body, "message", s->message, sizeof(s->message));
    j_str(body, "pair_code", s->pair_code, sizeof(s->pair_code));
    const char *arr = j_member(body, "devices");
    if (arr && *(arr = j_ws(arr)) == '[') {
        p = j_ws(arr + 1);
        while (*p && *p != ']' && s->count < UI_DEVICE_LIMIT) {
            if (*p != '{') break;
            struct ui_device *d = &s->devices[s->count++];
            d->address = j_mac(p, "address");
            d->audio = (int)j_int(p, "audio", 0);
            j_str(p, "name", d->name, sizeof(d->name));
            p = j_ws(j_skip(p));
            if (*p == ',') p = j_ws(p + 1);
        }
    }
    return 1;
}
#endif
