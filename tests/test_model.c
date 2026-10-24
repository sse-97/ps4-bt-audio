/* Host tests for the home-screen app's JSON status reader (native/model.h).
 * The app reads GET /api/status instead of the old packed binary struct, so the
 * reader must pull the right fields out of a real status body, tolerate added
 * and reordered fields, and never read past a short or malformed body. */
#include "test.h"
#include "../native/model.h"

void test_model(void) {
    /* A representative loopback status body (fields reordered vs. the writer, an
     * unknown "future" field added, pair_code present as on a console caller). */
    const char *body =
        "{\"version\":\"2.7.5\",\"api\":3,\"message\":\"Playing on AirPods Pro\","
        "\"future_field\":42,\"volume\":65,\"tv_enabled\":1,\"scanning\":0,"
        "\"pin_pending\":0,\"test\":0,\"link\":2,\"attempts\":1,\"retry_ms\":0,"
        "\"meter\":58,\"quiet_ms\":0,\"connect_rc\":0,\"audio_rc\":0,\"lan\":1,"
        "\"latency_mode\":2,\"drops\":3,\"underruns\":1,\"state\":\"streaming\","
        "\"pair_code\":\"048213\",\"selected\":\"aabbccddeeff\",\"name\":\"AirPods Pro\","
        "\"devices\":[{\"address\":\"aabbccddeeff\",\"name\":\"AirPods Pro\",\"audio\":1},"
        "{\"address\":\"112233445566\",\"name\":\"JBL Flip 6\",\"audio\":1}]}";
    struct ui_state s;
    CHECK(model_parse(body, &s) == 1);
    CHECK_EQ(s.link, LINK_STREAMING);
    CHECK_EQ(s.volume, 65);
    CHECK_EQ(s.tv, 1);
    CHECK_EQ(s.meter, 58);
    CHECK_EQ(s.lan, 1);
    CHECK(!strcmp(s.message, "Playing on AirPods Pro"));
    CHECK(!strcmp(s.name, "AirPods Pro"));
    CHECK(!strcmp(s.pair_code, "048213"));
    CHECK_EQ(s.latency_mode, 2);
    CHECK_EQ(s.drops, 3);
    CHECK_EQ(s.underruns, 1);
    CHECK(!strcmp(s.conn_state, "streaming"));
    CHECK_EQ(s.selected, 0xaabbccddeeffUL);
    CHECK_EQ(s.count, 2);
    CHECK(!strcmp(s.devices[0].name, "AirPods Pro"));
    CHECK_EQ(s.devices[0].address, 0xaabbccddeeffUL);
    CHECK_EQ(s.devices[1].address, 0x112233445566UL);
    CHECK_EQ(s.devices[1].audio, 1);

    /* Negative numbers, and a device name with escaped quote/backslash. */
    const char *esc =
        "{\"api\":3,\"audio_rc\":-14,\"name\":\"A \\\"B\\\" C\","
        "\"devices\":[{\"address\":\"010203040506\",\"name\":\"X\\\\Y\",\"audio\":0}]}";
    CHECK(model_parse(esc, &s) == 1);
    CHECK_EQ(s.audio_rc, -14);
    CHECK(!strcmp(s.name, "A \"B\" C"));
    CHECK_EQ(s.count, 1);
    CHECK(!strcmp(s.devices[0].name, "X\\Y"));

    /* A LAN caller gets no pair_code; it must read back empty, not stale. */
    const char *lanbody = "{\"api\":3,\"link\":0,\"lan\":1,\"volume\":30,\"devices\":[]}";
    CHECK(model_parse(lanbody, &s) == 1);
    CHECK_EQ(s.pair_code[0], 0);
    CHECK_EQ(s.count, 0);
    CHECK_EQ(s.volume, 30);

    /* Missing keys fall back to defaults rather than garbage. */
    const char *sparse = "{\"api\":3}";
    CHECK(model_parse(sparse, &s) == 1);
    CHECK_EQ(s.link, LINK_IDLE);
    CHECK_EQ(s.volume, 0);
    CHECK_EQ(s.count, 0);
    CHECK_EQ(s.name[0], 0);
    CHECK_EQ(s.latency_mode, 1);  /* balanced default when the service omits it */
    CHECK_EQ(s.conn_state[0], 0);

    /* Not an object, wrong/old api, and an empty string are all rejected. */
    CHECK(model_parse("not json", &s) == 0);
    CHECK(model_parse("{\"api\":2,\"link\":2}", &s) == 0);
    CHECK(model_parse("", &s) == 0);

    /* A value string that itself contains braces and a "link": decoy must not be
     * mistaken for the real fields that follow it. */
    const char *tricky =
        "{\"api\":3,\"message\":\"{not: a field} \\\"link\\\":9\",\"link\":5,\"volume\":10}";
    CHECK(model_parse(tricky, &s) == 1);
    CHECK_EQ(s.link, LINK_SEARCHING);
    CHECK_EQ(s.volume, 10);
}
