/* EasyRadio core: easyradio.ini. A bad line is logged and ignored, never fatal: the game must start anyway. */
#include "er_core.h"

const ErStation er_stations[ER_STATIONS] = {
    {"pulse", "Horizon Pulse", 1, "Pulse"},
    {"bass", "Bass Arena", 2, "Bass"},
    {"xs", "Horizon XS", 3, "XS"},
    {"levante", "Radio Levante", 4, "Levante"},
    {"hospital", "Hospital Records", 5, "Hospital"},
    {"ninjatune", "Ninja Tune", 6, "Ninja_Tune"},
    {"innovative", "Innovative Leisure", 7, "Innovative_Leisure"},
};

int er_station_index(const char *id, size_t length) {
    if (er_equal_ci(id, length, "horizon")) return 0;
    for (int i = 0; i < ER_STATIONS; ++i)
        if (er_equal_ci(id, length, er_stations[i].id)) return i;
    return -1;
}

static const struct {
    const char *name;
    unsigned bit;
} buttons[] = {{"Up", 0x0001}, {"Down", 0x0002}, {"Left", 0x0004}, {"Right", 0x0008}, {"Menu", 0x0010}, {"View", 0x0020}, {"LS", 0x0040},
               {"RS", 0x0080}, {"LB", 0x0100},   {"RB", 0x0200},   {"A", 0x1000},    {"B", 0x2000},     {"X", 0x4000},    {"Y", 0x8000}};

void er_config_defaults(ErConfig *config) {
    for (int i = 0; i < ER_STATIONS; ++i) config->folder[i] = 0;
    config->dj_off = 0;
    config->skip_vk = 0x75; /* F6 */
    config->skip_button = 0x0040; /* LS */
    config->game_folder = 0;
    config->errors = 0;
}

void er_config_free(ErConfig *config) {
    for (int i = 0; i < ER_STATIONS; ++i) er_free(config->folder[i]);
    er_free(config->game_folder);
    er_config_defaults(config);
}

static int blank(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

/* An absolute folder anywhere on disk: C:\\Music\\party, C:/Music/party, \\\\server\\share\\party, or (host tests) /music/party. */
int er_is_absolute(const char *v, size_t n) {
    if (n >= 3 && ((v[0] | 0x20) >= 'a' && (v[0] | 0x20) <= 'z') && v[1] == ':' && (v[2] == '\\' || v[2] == '/')) return 1;
    return (n >= 3 && v[0] == '\\' && v[1] == '\\') || (n >= 2 && v[0] == '/');
}

/* A radio folder: one plain name (looked up in the music folder, then in this mod; not a generated or reserved
   name), or an absolute path. */
static int good_folder(const char *v, size_t n) {
    if (!n || v[n - 1] == '.' || v[n - 1] == ' ') return 0;
    int absolute = er_is_absolute(v, n);
    if (!absolute && v[0] == '.') return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)v[i];
        int separator = c == '/' || c == '\\' || (c == ':' && i == 1);
        if (c < 32 || (separator && !absolute) || (c == ':' && i != 1) || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return 0;
    }
    return absolute || (!er_equal_ci(v, n, "plugins") && !er_equal_ci(v, n, "media") && !er_equal_ci(v, n, "cache"));
}

static void problem(ErConfig *config, unsigned line, const char *what, const char *v, size_t n) {
    char shown[96];
    size_t k = 0;
    for (; k < n && k + 1 < sizeof(shown); ++k) shown[k] = v[k] >= 32 && v[k] < 127 ? v[k] : '?';
    shown[k] = 0;
    er_log("WARN: easyradio.ini line %u: %s ('%s'); line ignored", line, what, shown);
    ++config->errors;
}

void er_config_parse(ErConfig *config, const char *text, size_t length) {
    enum { NONE, RADIOS, OPTIONS, OTHER } section = NONE;
    size_t at = 0;
    unsigned line_number = 0;
    if (length >= 3 && (unsigned char)text[0] == 0xef && (unsigned char)text[1] == 0xbb && (unsigned char)text[2] == 0xbf) at = 3;
    while (at < length) {
        size_t end = at;
        while (end < length && text[end] != '\n') ++end;
        size_t next = end < length ? end + 1 : end;
        ++line_number;
        const char *p = text + at, *e = text + end;
        at = next;
        while (p < e && blank(*p)) ++p;
        while (e > p && blank(e[-1])) --e;
        if (p == e || *p == ';' || *p == '#') continue;
        if (*p == '[') {
            const char *close = p;
            while (close < e && *close != ']') ++close;
            size_t n = (size_t)(close - p - 1);
            section = close == e                        ? (problem(config, line_number, "unclosed section", p, (size_t)(e - p)), OTHER)
                      : er_equal_ci(p + 1, n, "radios")  ? RADIOS
                      : er_equal_ci(p + 1, n, "options") ? OPTIONS
                                                         : OTHER;
            if (section == OTHER && close != e) problem(config, line_number, "unknown section", p, (size_t)(e - p));
            continue;
        }
        const char *eq = p;
        while (eq < e && *eq != '=' && *eq != ':') ++eq;
        if (eq == e) {
            problem(config, line_number, "not 'name = value'", p, (size_t)(e - p));
            continue;
        }
        const char *key_end = eq;
        while (key_end > p && blank(key_end[-1])) --key_end;
        const char *v = eq + 1;
        while (v < e && blank(*v)) ++v;
        /* an inline comment needs a blank before ';' or '#' (configparser's rule) */
        const char *v_end = v;
        while (v_end < e && !((*v_end == ';' || *v_end == '#') && v_end > v && blank(v_end[-1]))) ++v_end;
        while (v_end > v && blank(v_end[-1])) --v_end;
        size_t kn = (size_t)(key_end - p), vn = (size_t)(v_end - v);
        if (section == RADIOS) {
            int station = er_station_index(p, kn);
            if (station < 0) {
                problem(config, line_number, "unknown station id; ids: horizon (= pulse), pulse, bass, xs, levante, hospital, ninjatune, innovative",
                        p, kn);
            } else if (er_equal_ci(v, vn, "stock")) {
                er_free(config->folder[station]); /* the launcher's "keep the station's own music" */
                config->folder[station] = 0;
            } else if (!good_folder(v, vn)) {
                problem(config, line_number, "not a folder name or an absolute folder path", v, vn);
            } else {
                if (config->folder[station]) {
                    er_log("WARN: easyradio.ini line %u: %s mapped again; the later line wins", line_number, er_stations[station].id);
                    er_free(config->folder[station]);
                }
                config->folder[station] = er_strdup(v, vn);
            }
        } else if (section == OPTIONS) {
            if (er_equal_ci(p, kn, "dj")) {
                if (er_equal_ci(v, vn, "keep")) config->dj_off = 0;
                else if (er_equal_ci(v, vn, "off")) config->dj_off = 1;
                else problem(config, line_number, "dj must be keep or off", v, vn);
            } else if (er_equal_ci(p, kn, "skip_key")) {
                unsigned f = 0;
                for (size_t i = 1; i < vn && v[i] >= '0' && v[i] <= '9'; ++i) f = f * 10 + (unsigned)(v[i] - '0');
                int digits_only = vn >= 2 && vn <= 3 && (v[0] == 'F' || v[0] == 'f') && v[1] >= '0' && v[1] <= '9' &&
                                  (vn == 2 || (v[2] >= '0' && v[2] <= '9'));
                if (er_equal_ci(v, vn, "off")) config->skip_vk = 0;
                else if (digits_only && f >= 1 && f <= 12) config->skip_vk = (int)(0x70 + f - 1);
                else problem(config, line_number, "skip_key must be F1-F12 or off", v, vn);
            } else if (er_equal_ci(p, kn, "skip_pad")) {
                unsigned bit = 0;
                for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i)
                    if (er_equal_ci(v, vn, buttons[i].name)) bit = buttons[i].bit;
                if (bit || er_equal_ci(v, vn, "off")) config->skip_button = bit;
                else problem(config, line_number, "skip_pad must be A B X Y LB RB LS RS View Menu Up Down Left Right or off", v, vn);
            } else if (er_equal_ci(p, kn, "game_folder")) {
                er_free(config->game_folder);
                config->game_folder = vn ? er_strdup(v, vn) : 0;
            } else {
                problem(config, line_number, "unknown option", p, kn);
            }
        } else {
            problem(config, line_number, "setting outside [radios] or [options]", p, (size_t)(e - p));
        }
    }
}
