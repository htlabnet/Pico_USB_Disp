//
// ######################################################################
//
//    DisplayInfo - 環境情報を画面に表示するサンプル
//
//    書き込んでアダプタを繋ぐだけで、MCU / USB ホスト種別 / チップ /
//    解像度 / リフレッシュレート / VID:PID / EDID の有無 / フルフレーム
//    転送速度を画面に表示する。転送速度は2値 (実性能はこの間に入る)
//      Max FPS = 単色全面塗り
//      Min FPS = 圧縮が全く効かない内容の実測値
//    シリアル (115200bps) には毎秒、画面と同じ内容のミラーを出力する。
//
//    Copyright (C) 2026
//      Hideto Kikuchi / PJラボ (@pcjpnet) - https://pc-jp.net/
//
// ######################################################################
//

#include <usb_disp.h>
#include "font16x32.h"

// USB D+/D- の GPIO 指定 (Pico のみ有効)
// ビルドフラグ -DUSB_DISP_PIN_DP=n -DUSB_DISP_PIN_DM=m で差し替え可能

#ifndef USB_DISP_PIN_DP
#define USB_DISP_PIN_DP 16
#endif
#ifndef USB_DISP_PIN_DM
#define USB_DISP_PIN_DM 17
#endif

// 表示解像度 (ビルドフラグで差し替え可)
// 既定は 720p 固定で設定できないチップではフォールバックする
#ifndef USB_DISP_INFO_WIDTH
#define USB_DISP_INFO_WIDTH 1280
#endif
#ifndef USB_DISP_INFO_HEIGHT
#define USB_DISP_INFO_HEIGHT 720
#endif

// 解像度の自動選択ポリシー
//   0 = EDID 記載の最大解像度 (既定)
//   1 = ベースブロックの preferred timing (モニタのネイティブ解像度)
#ifndef USB_DISP_INFO_EDID_POLICY
#define USB_DISP_INFO_EDID_POLICY 0
#endif

// 1 にすると EDID を一切読まず、上の解像度に固定する
// (-DUSB_DISP_INFO_IGNORE_EDID=1)。
// 実際に出せる解像度より小さいEDIDを返す装置対策
// 指定解像度を強制したいときに使う
#ifndef USB_DISP_INFO_IGNORE_EDID
#define USB_DISP_INFO_IGNORE_EDID 0
#endif

// 1 にすると EDID 128 バイトの16進ダンプと主要フィールドを起動時に
// 1回だけシリアルへ出す (-DUSB_DISP_INFO_EDID_DUMP=1)
#ifndef USB_DISP_INFO_EDID_DUMP
#define USB_DISP_INFO_EDID_DUMP 0
#endif

#if USB_DISP_PORT_PICO
#include "hardware/clocks.h"   // clock_get_hz (CPU 速度の表示用)
#include "hardware/vreg.h"     // クロックアップ用の昇圧
#include "usb_disp_udh_bus.h"  // HS 失敗内訳カウンタ (USB_DISP_UDH_HS_DIAG 時)
#endif

static usb_disp_t *disp;
static bool shown = false;

// 背景/文字色 (RGB565)
#define COL_BG    0x0126   // 濃紺
#define COL_TITLE 0xFFE0   // 黄
#define COL_TEXT  0xFFFF   // 白
#define COL_SUB   0x5D9F   // 水色
#define COL_EDGE  0x07E0   // 緑 (画面の外周フチ)

#define BAND_LINES 8
static uint16_t band[USB_DISP_MAX_WIDTH * BAND_LINES];

static uint8_t g_scale = 1;   // 文字の拡大率 (超高解像度のみ2倍)
static uint16_t g_status_y = 0;   // ステータス行群の Y 位置 (draw_info が決める)
static uint32_t g_frame_ms = 0;   // フルフレーム転送時間: 単色 (draw_info が実測)
static uint32_t g_worst_ms = 0;   // 同: 非圧縮の全画面換算 (measure_worst)
static int8_t g_est = -1;         // EDID 状態キャッシュ (3/2/1/0, -1=未取得)
static int g_elen = 0;
static uint16_t g_epw = 0, g_eph = 0;   // EDID の希望解像度 (DTD1)
static uint8_t g_eext = 0;              // 拡張ブロック数 (EDID byte 126)
static uint16_t g_y = 0;      // 次の行の Y 位置

// ---- 16x32 フォントの描画 (band バッファで部分転送) ----
#define FONT_W 16
#define FONT_H 32

static void draw_text(uint16_t x, uint16_t y, const char *s,
                      uint8_t scale, uint16_t fg, uint16_t bg) {
    uint16_t w = usb_disp_width(disp);
    uint16_t n = strlen(s);
    uint32_t tw = (uint32_t)n * FONT_W * scale;
    if (x >= w) return;
    if (tw > (uint32_t)(w - x)) tw = w - x;          // 右端クリップ
    uint16_t gh = FONT_H * scale;                     // 1行の高さ [px]

    for (uint16_t py0 = 0; py0 < gh; py0 += BAND_LINES) {
        uint16_t lines = (uint16_t)((py0 + BAND_LINES <= gh) ? BAND_LINES
                                                             : gh - py0);
        for (uint16_t ln = 0; ln < lines; ln++) {
            uint16_t frow = (uint16_t)((py0 + ln) / scale);   // フォントの行 0..31
            uint16_t *row = &band[(uint32_t)ln * tw];
            uint32_t px = 0;
            for (uint16_t i = 0; i < n && px < tw; i++) {
                uint8_t c = (uint8_t)s[i];
                uint16_t bits = (c >= FONT16X32_FIRST && c <= FONT16X32_LAST)
                                    ? font16x32[c - FONT16X32_FIRST][frow]
                                    : 0;
                for (uint8_t fx = 0; fx < FONT_W && px < tw; fx++) {
                    uint16_t col = (bits >> (15 - fx)) & 1 ? fg : bg;
                    for (uint8_t r = 0; r < scale && px < tw; r++)
                        row[px++] = col;
                }
            }
        }
        usb_disp_update(disp, x, (uint16_t)(y + py0), (uint16_t)tw, lines,
                        band, (uint16_t)tw);
    }
}

// 1行出力 (Y 位置は自動送り)
static void put_line(const char *s, uint16_t color) {
    draw_text(24, g_y, s, g_scale, color, COL_BG);
    g_y = (uint16_t)(g_y + FONT_H * g_scale + 6 * g_scale);
}

// 背景を全面塗り (兼: フルフレーム転送時間の実測 = 単色ベストケース)
static uint32_t paint_background(void) {
    uint16_t w = usb_disp_width(disp), h = usb_disp_height(disp);
    uint32_t t0 = millis();
    for (uint32_t i = 0; i < (uint32_t)w * BAND_LINES; i++) band[i] = COL_BG;
    for (uint16_t y = 0; y < h; y += BAND_LINES) {
        uint16_t lines = (uint16_t)((y + BAND_LINES <= h) ? BAND_LINES : h - y);
        usb_disp_update(disp, 0, y, w, lines, band, w);
    }
    usb_disp_flush(disp, 1000);
    return millis() - t0;
}

// 圧縮が全く効かない内容だった場合の1フレーム転送時間を換算実測する。
// 背景色と青の最下位ビットだけ違う色を1ピクセルおきに交互に並べる。
static uint32_t measure_worst(void) {
    uint16_t w = usb_disp_width(disp), h = usb_disp_height(disp);
    if (w == 0 || h == 0) return 0;
    for (uint32_t i = 0; i < (uint32_t)w * BAND_LINES; i++) {
        band[i] = (i & 1) ? (COL_BG ^ 1) : COL_BG;   // 隣接不一致 = 非圧縮
    }
    uint16_t rows = (uint16_t)((192000 + w - 1) / w);
    rows = (uint16_t)((rows + BAND_LINES - 1) / BAND_LINES * BAND_LINES);
    if (rows > h) rows = h;
    // フルフレーム型プロトコル (T6/MS91xx) は部分更新でも flush ごとに
    // フレーム全体相当を送るため、部分計測からの換算が過大になる。
    // よって全面で測る (これらは HS ホスト専用なので全面でも速い)
    usb_disp_chip_t chip = usb_disp_chip(disp);
    if (chip == USB_DISP_CHIP_T6 || chip == USB_DISP_CHIP_MS912X ||
        chip == USB_DISP_CHIP_MS913X) {
        rows = h;
    }
    usb_disp_flush(disp, 1000);   // 計測前にパイプラインを空にする
    uint32_t t0 = millis();
    for (uint16_t y = 0; y < rows; y += BAND_LINES) {
        uint16_t lines = (uint16_t)((y + BAND_LINES <= rows) ? BAND_LINES
                                                             : rows - y);
        usb_disp_update(disp, 0, y, w, lines, band, w);
    }
    usb_disp_flush(disp, 10000);
    uint32_t dt = millis() - t0;
    return (uint32_t)(((uint64_t)dt * h + rows / 2) / rows);   // 全画面換算
}

// EDID の状態: 3=読まない設定 / 2=有効 / 1=読めるが無効 / 0=読めない / -1=判定不能
// 有効なら DTD1 (モニタの希望解像度) も g_epw/g_eph に控える
// (表示のたびに読み直さないよう、読み出しはこの関数だけで行う)
//
// EDID 読み出しは 1バイト = コントロール転送1回。描画で帯域が埋まっていると
// 途中の1回が落ちて部分長で返ることがある (1080p 表示中は頻繁に起きる)
// これを「EDID が無効になった」と誤解すると表示がちらつき、
// 再設定まで走ってしまうので、中途半端な長さは判定不能として捨てる
static int edid_state(int *len_out) {
    static uint8_t edid[128];
    static const uint8_t magic[8] = {0x00, 0xFF, 0xFF, 0xFF,
                                     0xFF, 0xFF, 0xFF, 0x00};
    if (usb_disp_ignore_edid(disp)) {   // 固定解像度モード: バスに出さない
        g_epw = g_eph = 0;
        g_eext = 0;
        if (len_out) *len_out = 0;
        return 3;
    }
    // ここではベースブロックだけ読む (拡張まで読むと数百回のコントロール転送になる)
    // 解像度判定はライブラリ側の usb_disp_edid_supports() が拡張ブロック込みで行う
    int elen = usb_disp_read_edid(disp, edid, sizeof(edid));
    if (elen > 0 && elen < 128) return -1;   // 途中で切れた場合、前回値を維持
    if (len_out) *len_out = elen;
    g_epw = g_eph = 0;
    g_eext = 0;
    if (elen <= 0) return 0;
    if (memcmp(edid, magic, 8) != 0) return 1;
    {
        g_eext = edid[126];
        if (edid[54] || edid[55]) {   // DTD1 = preferred timing
            g_epw = (uint16_t)(edid[56] | ((edid[58] & 0xF0) << 4));
            g_eph = (uint16_t)(edid[59] | ((edid[61] & 0xF0) << 4));
        }
    }
    return 2;
}

#if USB_DISP_INFO_EDID_DUMP
// EDID (拡張ブロック込み) の16進ダンプをシリアルへ
// 起動直後に出すとターミナルを繋ぐ前に流れてしまうので数秒待ってから出す
#define EDID_DUMP_MAX 256   // DL-1xx の読み出し上限 (オフセットが 8bit)
static uint8_t edump[EDID_DUMP_MAX];

// 18B ディスクリプタ 1個を表示
static void edid_dump_dtd(const char *label, int idx, const uint8_t *t) {
    uint32_t pclk10 = (uint32_t)(t[0] | (t[1] << 8));
    if (pclk10 == 0) {   // タイミングではない記述子 (モニタ名など)
        Serial.printf("[EDID] %s%d: descriptor tag=%02X\r\n", label, idx, t[3]);
        return;
    }
    Serial.printf("[EDID] %s%d: %ux%u pclk=%lu kHz\r\n", label, idx,
                  (unsigned)(t[2] | ((t[4] >> 4) << 8)),
                  (unsigned)(t[5] | ((t[7] >> 4) << 8)),
                  (unsigned long)(pclk10 * 10));
}

// CEA-861 拡張ブロックの中身 (データブロックコレクション + DTD)
static void edid_dump_cea(const uint8_t *b) {
    Serial.printf("[EDID] CEA ext rev=%u dtd_off=%u flags=%02X\r\n", b[1], b[2],
                  b[3]);
    uint8_t dtd_off = b[2];
    if (b[1] >= 3 && dtd_off > 4) {
        uint8_t p = 4;
        while (p < dtd_off && p < 127) {
            uint8_t tag = (uint8_t)(b[p] >> 5), len = (uint8_t)(b[p] & 0x1F);
            if (len == 0 && tag == 0) break;
            static const char *k_tag[] = {"resv", "audio", "video", "vendor",
                                          "speaker", "vesa", "resv", "ext"};
            Serial.printf("[EDID]   block tag=%u(%s) len=%u", tag, k_tag[tag],
                          len);
            if (tag == 2) {   // Video Data Block: SVD (VIC) の列
                Serial.print("  VIC:");
                for (uint8_t i = 1; i <= len && p + i < 127; i++) {
                    // 129..192 はバイト全体が VIC (native フラグ無し)
                    uint8_t v = b[p + i];
                    if (v >= 129 && v <= 192) Serial.printf(" %u", v);
                    else Serial.printf(" %u%s", v & 0x7F,
                                       (v & 0x80) ? "*" : "");
                }
            }
            Serial.print("\r\n");
            p = (uint8_t)(p + 1 + len);
        }
    }
    if (dtd_off >= 4 && dtd_off < 127) {
        uint8_t n = (uint8_t)((127 - dtd_off) / 18);
        for (uint8_t i = 0; i < n; i++) edid_dump_dtd("ext dtd", i,
                                                      b + dtd_off + i * 18);
    }
}

static void edid_dump_once(void) {
    static bool done = false;
    if (done || millis() < 8000) return;

    int n = usb_disp_read_edid(disp, edump, 128);
    Serial.printf("\r\n[EDID] base block: %d bytes\r\n", n);
    if (n < 128) return;
    done = true;
    uint8_t ext = edump[126];
    // 拡張ブロックを読み足す (バッファに入るぶんだけ)
    uint16_t total = 128;
    for (uint8_t i = 0; i < ext && total + 128 <= EDID_DUMP_MAX; i++) {
        if (usb_disp_read_edid_at(disp, total, edump + total, 128) != 128) break;
        total = (uint16_t)(total + 128);
    }
    Serial.printf("[EDID] extensions=%u, read %u bytes total "
                  "(usb_disp_edid_size=%u)\r\n",
                  ext, total, usb_disp_edid_size(disp));

    for (uint16_t i = 0; i < total; i += 16) {
        Serial.printf("[EDID] %03X:", i);
        for (int j = 0; j < 16; j++) Serial.printf(" %02X", edump[i + j]);
        Serial.print("\r\n");
    }

    Serial.printf("[EDID] mfg=%02X%02X prod=%02X%02X ver=%u.%u\r\n", edump[8],
                  edump[9], edump[11], edump[10], edump[18], edump[19]);
    Serial.printf("[EDID] established: %02X %02X %02X\r\n", edump[0x23],
                  edump[0x24], edump[0x25]);
    for (int i = 0; i < 8; i++) {
        const uint8_t *t = &edump[0x26 + i * 2];
        if (t[0] <= 0x01) continue;
        uint16_t sw = (uint16_t)(((uint16_t)t[0] + 31) * 8);
        uint16_t sh = (t[1] >> 6) == 0 ? sw * 10 / 16
                    : (t[1] >> 6) == 1 ? sw * 3 / 4
                    : (t[1] >> 6) == 2 ? sw * 4 / 5 : sw * 9 / 16;
        Serial.printf("[EDID] std%d: %ux%u @%uHz\r\n", i, sw, sh,
                      (unsigned)((t[1] & 0x3F) + 60));
    }
    for (int i = 0; i < 4; i++) edid_dump_dtd("dtd", i, &edump[54 + i * 18]);

    for (uint16_t off = 128; off + 128 <= total; off += 128) {
        Serial.printf("[EDID] ext block @%u tag=%02X\r\n", off, edump[off]);
        if (edump[off] == 0x02) edid_dump_cea(&edump[off]);
    }

    // ライブラリの判定結果 (fit_mode がこれを見る)
    static const struct { uint16_t w, h; } k_ask[] = {
        {1920, 1080}, {1600, 1200}, {1280, 720}, {1024, 768}, {800, 600},
        {720, 480}, {640, 480},
    };
    Serial.print("[EDID] usb_disp_edid_supports:");
    for (unsigned i = 0; i < sizeof(k_ask) / sizeof(k_ask[0]); i++)
        Serial.printf(" %ux%u=%d", k_ask[i].w, k_ask[i].h,
                      usb_disp_edid_supports(disp, k_ask[i].w, k_ask[i].h));
    Serial.print("\r\n");
}
#endif

// モニタが出せない解像度を要求していたら自動選択へ落とす
// (例: 800x600 しか出せない内蔵パネルに 720p を設定してしまった場合)
//   - EDID (CEA 拡張ブロック込み) が対応を明示していればそのまま使う
//   - 最大がこれ以上ならスケーリングして受けられるとしてそのまま使う
//     (1080p モニタが 720p を EDID に列挙していないのは普通)
//   - 申告最大より明らかに大きい場合だけ選び直す
//   - 固定解像度モード (ignore_edid) では何もしない
static void fit_mode(void) {
    if (usb_disp_ignore_edid(disp)) return;
    uint16_t w = usb_disp_width(disp), h = usb_disp_height(disp);
    if (usb_disp_edid_supports(disp, w, h)) return;   // EDID に記載あり
    uint16_t mw = 0, mh = 0;
    if (!usb_disp_edid_max_mode(disp, &mw, &mh)) return;  // EDID 不明 → 触らない
    if (mw >= w && mh >= h) return;                       // 収まる → そのまま
    usb_disp_set_auto_mode(disp);   // 大きすぎる → ポリシーに従って選び直す
}

// 画面の外周に 4px のフチを描く (写真で画面の境目が分かるように)
static void paint_border(void) {
    uint16_t w = usb_disp_width(disp), h = usb_disp_height(disp);
    const uint16_t bw = 4;
    for (uint32_t i = 0; i < (uint32_t)w * bw; i++) band[i] = COL_EDGE;
    usb_disp_update(disp, 0, 0, w, bw, band, w);                   // 上
    usb_disp_update(disp, 0, (uint16_t)(h - bw), w, bw, band, w);  // 下
    for (uint32_t i = 0; i < (uint32_t)bw * BAND_LINES; i++) band[i] = COL_EDGE;
    for (uint16_t y = 0; y < h; y += BAND_LINES) {
        uint16_t lines = (uint16_t)((y + BAND_LINES <= h) ? BAND_LINES : h - y);
        usb_disp_update(disp, 0, y, bw, lines, band, bw);                    // 左
        usb_disp_update(disp, (uint16_t)(w - bw), y, bw, lines, band, bw);   // 右
    }
    usb_disp_flush(disp, 1000);
}

// ---- MCU / USB ホスト情報の整形 (画面とシリアルで共用) ----
static void fmt_mcu(char *buf, size_t n) {
#if USB_DISP_PORT_PICO
#if defined(PICO_RP2040)
    snprintf(buf, n, "MCU    : RP2040 @ %lu MHz",
             (unsigned long)(clock_get_hz(clk_sys) / 1000000));
#else
    snprintf(buf, n, "MCU    : RP2350 @ %lu MHz",
             (unsigned long)(clock_get_hz(clk_sys) / 1000000));
#endif
#elif USB_DISP_PORT_ESP32
    snprintf(buf, n, "MCU    : %s @ %u MHz", CONFIG_IDF_TARGET,
             (unsigned)getCpuFrequencyMhz());
#elif USB_DISP_PORT_TEENSY
#if defined(ARDUINO_TEENSY41)
    snprintf(buf, n, "MCU    : Teensy 4.1 @ %lu MHz",
             (unsigned long)(F_CPU_ACTUAL / 1000000));
#elif defined(ARDUINO_TEENSY40)
    snprintf(buf, n, "MCU    : Teensy 4.0 @ %lu MHz",
             (unsigned long)(F_CPU_ACTUAL / 1000000));
#else
    snprintf(buf, n, "MCU    : Teensy 4.x @ %lu MHz",
             (unsigned long)(F_CPU_ACTUAL / 1000000));
#endif
#else
    snprintf(buf, n, "MCU    : PC (libusb)");
#endif
}

static void fmt_usb(char *buf, size_t n) {
#if USB_DISP_PORT_PICO
    snprintf(buf, n, "USB    : PIO Full-Speed Host (12 Mbps)");
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
    snprintf(buf, n, "USB    : OTG High-Speed Host (480 Mbps)");
#elif USB_DISP_PORT_ESP32
    snprintf(buf, n, "USB    : OTG Full-Speed Host (12 Mbps)");
#elif USB_DISP_PORT_TEENSY
    snprintf(buf, n, "USB    : EHCI High-Speed Host (480 Mbps)");
#else
    snprintf(buf, n, "USB    : libusb");
#endif
}

#if USB_DISP_PORT_ESP32
// PSRAM は「チップに載っているか」だけでなく「ビルドで有効か」に依存する
// ツール > PSRAM が Disabled のビルドでは psramFound() は搭載品でも必ず
// false になる。誤解を招くので3状態を区別して表示する:
//   %u KB           = 有効ビルド + 初期化成功 (実際に使える)
//   Init failed     = 有効ビルドだが初期化失敗 (非搭載 or QSPI/OPI 種別違い)
//   Off (build opt) = 無効ビルド (ツール > PSRAM で有効化すること)
static void fmt_psram(char *buf, size_t n) {
#if defined(BOARD_HAS_PSRAM)
    if (psramFound()) {
        snprintf(buf, n, "PSRAM  : %u KB", (unsigned)(ESP.getPsramSize() / 1024));
    } else {
        snprintf(buf, n, "PSRAM  : Init failed (wrong type?)");
    }
#else
    snprintf(buf, n, "PSRAM  : Off (build option)");
#endif
}
#endif

static void fmt_edid(char *buf, size_t n) {
    if (g_est == 3) {
        snprintf(buf, n, "EDID   : Ignored (fixed mode)");
    } else if (g_est == 2) {
        // 拡張ブロックがあると「ベースの希望解像度」より上を出せることが多い
        char ext[16];
        if (g_eext) snprintf(ext, sizeof(ext), "+%uext", g_eext);
        else        ext[0] = 0;
        if (g_epw) {
            snprintf(buf, n, "EDID   : OK (%d B%s)  wants %ux%u", g_elen, ext,
                     g_epw, g_eph);
        } else {
            snprintf(buf, n, "EDID   : OK (%d B%s)", g_elen, ext);
        }
    } else if (g_est == 1) {
        snprintf(buf, n, "EDID   : Invalid (%d Bytes)", g_elen);
    } else {
        snprintf(buf, n, "EDID   : Not available");
    }
}

// 全画面1枚の転送性能 (label = "Max FPS" 単色 / "Min FPS" 非圧縮換算)
static void fmt_frame(char *buf, size_t n, const char *label, uint32_t ms) {
    uint32_t fps10 = (ms > 0) ? 10000 / ms : 0;
    snprintf(buf, n, "%s: %lu.%lu fps  (%lu ms/frame)", label,
             (unsigned long)(fps10 / 10), (unsigned long)(fps10 % 10),
             (unsigned long)ms);
}

// ---- 情報画面 ----
static void draw_info(void) {
    char buf[96];
    uint16_t w = usb_disp_width(disp), h = usb_disp_height(disp);

    g_scale = (w >= 2560) ? 2 : 1;   // 16x32 は等倍で十分大きい
    g_y = 24;

    g_worst_ms = measure_worst();      // 非圧縮 (写真相当) の換算実測
    g_frame_ms = paint_background();   // 単色の全画面実測 (兼 背景塗り)
    paint_border();

    put_line("PICO_USB_DISP  -  DISPLAY INFO", COL_TITLE);
    put_line("", COL_TEXT);

    // --- MCU ---
    fmt_mcu(buf, sizeof(buf));
    put_line(buf, COL_TEXT);
    fmt_usb(buf, sizeof(buf));
    put_line(buf, COL_TEXT);
#if USB_DISP_PORT_ESP32
    fmt_psram(buf, sizeof(buf));
    put_line(buf, COL_TEXT);
#endif

    // --- ディスプレイ ---
    snprintf(buf, sizeof(buf), "Chip   : %s   (Max %lu px)",
             usb_disp_chip_name(disp), (unsigned long)usb_disp_max_area(disp));
    put_line(buf, COL_SUB);

    // 型番 (usb_disp_model.h の VID/PID リストに一致した既知製品のみ)
    const char *model = usb_disp_model(disp);
    snprintf(buf, sizeof(buf), "Model  : %s", model ? model : "Unknown");
    put_line(buf, COL_SUB);

    snprintf(buf, sizeof(buf), "USB ID : %04X:%04X   Index %d",
             usb_disp_vid(disp), usb_disp_pid(disp), usb_disp_index(disp));
    put_line(buf, COL_SUB);

    snprintf(buf, sizeof(buf), "Mode   : %ux%u @ %u Hz", w, h,
             usb_disp_refresh_hz(disp));
    put_line(buf, COL_SUB);

    {
        int st = edid_state(&g_elen);
        if (st >= 0) g_est = (int8_t)st;   // 判定不能 (-1) は前回値を維持
    }
    fmt_edid(buf, sizeof(buf));
    put_line(buf, COL_SUB);

    // --- 性能 (EDID 直後の空行だった位置を使って2行。総行数は従来通り) ---
    fmt_frame(buf, sizeof(buf), "Max FPS", g_frame_ms);
    put_line(buf, COL_TEXT);
    fmt_frame(buf, sizeof(buf), "Min FPS", g_worst_ms);
    put_line(buf, COL_TEXT);

    g_status_y = g_y;   // TX/Resend/Uptime はこの位置から毎秒描く

    usb_disp_flush(disp, 1000);
    (void)h;
}

// 稼働状態 (毎秒更新): 画面に TX / Resend / Uptime を出力し、
// シリアルにも画面と同じ内容のミラーを ANSI で同位置出力する
static void status_tick(void) {
    char buf[96];

    // --- 変動値の計算 (画面とシリアルで共用) ---
    static uint64_t prev_bytes = 0;
    uint64_t tx = shown ? usb_disp_stat_bytes(disp) : 0;
    uint32_t rate_kb = (uint32_t)((tx - prev_bytes) / 1024);
    prev_bytes = tx;
    char tx_line[64];
    snprintf(tx_line, sizeof(tx_line), "TX     : %lu.%02lu MB  (%lu KB/s)      ",
             (unsigned long)(tx / 1048576),
             (unsigned long)((tx % 1048576) * 100 / 1048576),
             (unsigned long)rate_kb);

    char resend_line[64];
    resend_line[0] = 0;
#if USB_DISP_PORT_PICO
    // Resend = ACK を取りこぼして同一パケットを再送した回数
    // 健全なら 0.0% 付近 (-DUSB_DISP_UDH_HS_DIAG=1 で失敗内訳を出せる)
    usb_disp_udh_host_t *host = usb_disp_get_host(disp);
    if (host && shown) {
        uint32_t resend = usb_disp_udh_host_stat_errors(host);
        uint32_t acked = (uint32_t)(tx / 64);   // 概算パケット数 (mps=64)
        uint32_t pct10 = (acked + resend) ? (uint32_t)((uint64_t)resend * 1000 /
                                                       (acked + resend)) : 0;
        snprintf(resend_line, sizeof(resend_line),
                 "Resend : %lu (%lu.%lu%%)  NAK %lu      ",
                 (unsigned long)resend, (unsigned long)(pct10 / 10),
                 (unsigned long)(pct10 % 10),
                 (unsigned long)usb_disp_udh_host_stat_naks(host));
    }
#endif
#if USB_DISP_PORT_PICO && USB_DISP_UDH_HS_DIAG
    // ハンドシェイク失敗の内訳 (-DUSB_DISP_UDH_HS_DIAG=1 のときだけ)
    // Resend が 0 でないときに、デバイスが応答していないのか
    // 自分の送信を受信しているのかを切り分ける
    {
        const usb_disp_udh_hs_diag_t *g = &g_udh_hs_diag;
        Serial.printf("\r\n[HSDIAG] total=%lu ack=%lu nak=%lu | no_start=%lu "
                      "short=%lu bad_sync=%lu | own=%lu sof=%lu data=%lu "
                      "other=%lu(last=%02X)\r\n",
                      (unsigned long)g->total, (unsigned long)g->pid_ack,
                      (unsigned long)g->pid_nak, (unsigned long)g->no_start,
                      (unsigned long)g->short_pkt, (unsigned long)g->bad_sync,
                      (unsigned long)g->pid_own, (unsigned long)g->pid_sof,
                      (unsigned long)g->pid_data, (unsigned long)g->pid_other,
                      g->last_pid);
    }
#endif
    char up_line[32];
    snprintf(up_line, sizeof(up_line), "Uptime : %lu s        ",
             (unsigned long)(millis() / 1000));

    // --- 画面 (接続中のみ)。順序: TX / Resend / Uptime (Uptime が最下行) ---
    if (shown) {
        uint16_t lh = (uint16_t)(FONT_H * g_scale + 6 * g_scale);
        uint16_t y = g_status_y;
        draw_text(24, y, tx_line, g_scale, COL_SUB, COL_BG);
        y = (uint16_t)(y + lh);
        if (resend_line[0]) {
            draw_text(24, y, resend_line, g_scale, COL_SUB, COL_BG);
            y = (uint16_t)(y + lh);
        }
        draw_text(24, y, up_line, g_scale, COL_TITLE, COL_BG);
        usb_disp_flush(disp, 1000);
    }

    // --- シリアルミラー (ANSI: 初回クリア -> 毎回ホームから上書き) ---
    static bool ansi_init = false;
    if (!ansi_init) {
        Serial.print("\x1b[2J");   // 画面クリア
        ansi_init = true;
    }
    Serial.print("\x1b[H");        // カーソルをホームへ
#define SLN(str) do { Serial.print(str); Serial.print("\x1b[K\r\n"); } while (0)
    SLN("PICO_USB_DISP  -  DISPLAY INFO");
    SLN("");
    fmt_mcu(buf, sizeof(buf));  SLN(buf);
    fmt_usb(buf, sizeof(buf));  SLN(buf);
#if USB_DISP_PORT_ESP32
    fmt_psram(buf, sizeof(buf)); SLN(buf);
#endif
    if (shown) {
        snprintf(buf, sizeof(buf), "Chip   : %s   (Max %lu px)",
                 usb_disp_chip_name(disp),
                 (unsigned long)usb_disp_max_area(disp));
        SLN(buf);
        const char *model = usb_disp_model(disp);
        snprintf(buf, sizeof(buf), "Model  : %s", model ? model : "Unknown");
        SLN(buf);
        snprintf(buf, sizeof(buf), "USB ID : %04X:%04X   Port %d",
                 usb_disp_vid(disp), usb_disp_pid(disp), usb_disp_index(disp));
        SLN(buf);
        snprintf(buf, sizeof(buf), "Mode   : %ux%u @ %u Hz",
                 usb_disp_width(disp), usb_disp_height(disp),
                 usb_disp_refresh_hz(disp));
        SLN(buf);
        fmt_edid(buf, sizeof(buf));
        SLN(buf);
        fmt_frame(buf, sizeof(buf), "Max FPS", g_frame_ms);
        SLN(buf);
        fmt_frame(buf, sizeof(buf), "Min FPS", g_worst_ms);
        SLN(buf);
        SLN(tx_line);
        if (resend_line[0]) SLN(resend_line);
    } else {
        SLN("Display: Waiting for connection...");
        SLN("");
    }
    SLN(up_line);
#undef SLN
}

// ---- 画面下部のアクティビティバー (左右に往復するグラデーションのコメット) ----
#define BAR_H     16    // バーの高さ [px]
#define COMET_LEN 160   // コメットの長さ [px]
#define COMET_STEP 12   // 1tickの移動量 [px]

static void draw_activity_bar(void) {
    uint16_t w = usb_disp_width(disp), h = usb_disp_height(disp);
    if (w < COMET_LEN * 2) return;
    static int x = 6, dir = 1, x_prev = 6;

    uint16_t y = (uint16_t)(h - BAR_H - 10);        // 下フチ (4px) の内側
    int x0 = (x < x_prev) ? x : x_prev;
    int x1 = ((x > x_prev) ? x : x_prev) + COMET_LEN;
    if (x1 > (int)w) x1 = w;
    int ww = x1 - x0;

    // 1行を組み立て (先頭=進行方向側が明るいグラデーション)
    for (int i = 0; i < ww; i++) {
        int gx = x0 + i;
        uint16_t col = COL_BG;
        if (gx >= x && gx < x + COMET_LEN) {
            int k = gx - x;                         // 0..LEN-1
            if (dir < 0) k = COMET_LEN - 1 - k;     // 左行きは左端が頭
            // COL_BG → 明シアンへの線形補間 (RGB565 チャネル毎)
            uint32_t t = (uint32_t)k * 255 / (COMET_LEN - 1);
            uint16_t r = (uint16_t)((((COL_BG >> 11) & 0x1F) * (255 - t) + 0x08 * t) / 255);
            uint16_t g = (uint16_t)((((COL_BG >> 5) & 0x3F) * (255 - t) + 0x37 * t) / 255);
            uint16_t b = (uint16_t)(((COL_BG & 0x1F) * (255 - t) + 0x1F * t) / 255);
            col = (uint16_t)((r << 11) | (g << 5) | b);
        }
        band[i] = col;
    }
    // 同じ行を BAR_H 行ぶん複製して2チャンクで転送
    for (uint16_t ln = 1; ln < BAND_LINES; ln++)
        memcpy(&band[(uint32_t)ln * ww], band, (size_t)ww * 2);
    usb_disp_update(disp, (uint16_t)x0, y, (uint16_t)ww, BAND_LINES, band, (uint16_t)ww);
    usb_disp_update(disp, (uint16_t)x0, (uint16_t)(y + BAND_LINES), (uint16_t)ww,
                    BAR_H - BAND_LINES, band, (uint16_t)ww);
    usb_disp_flush(disp, 100);

    x_prev = x;
    x += dir * COMET_STEP;
    if (x <= 6) { x = 6; dir = 1; }                 // 左フチ内側
    if (x >= (int)w - COMET_LEN - 6) { x = w - COMET_LEN - 6; dir = -1; }
}

// モード再設定 + 全再描画 (黒画面からの復帰用)
static void remode(void) {
    if (!usb_disp_set_mode(disp, USB_DISP_INFO_WIDTH, USB_DISP_INFO_HEIGHT))
        usb_disp_set_auto_mode(disp);
    fit_mode();   // モニタが指定解像度未満なら実解像度へ (固定モードでは何もしない)
    draw_info();
}

void setup() {
#if USB_DISP_PORT_PICO
    // PIO USB は sys_clk が12MHz の倍数であることを要求
    // クロック周波数が高いほど符号化/描画に余裕が出る
    //   RP2040 (Pico)  : 144MHz (12x12, 定格133の軽度OC) + vreg 1.15V
    //   RP2350 (Pico 2): 240MHz (12x20, Pico-PIO-USB 採用値) + vreg 1.20V
    // 12MHz 倍数でない値を設定してもライブラリが下方向へ自動調整する
#if defined(PICO_RP2040)
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sleep_ms(1);
    set_sys_clock_khz(144000, true);
#else
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(1);
    set_sys_clock_khz(240000, true);
#endif
#endif
    Serial.begin(115200);
    // usb_disp_set_log(true) はシリアルミラーと出力が混ざるため既定OFF
    usb_disp_init();
    // 解像度 (既定 720p、不可なら自動へフォールバック) 
    disp = usb_disp_add(0, USB_DISP_PIN_DP, USB_DISP_PIN_DM,
                        USB_DISP_INFO_WIDTH, USB_DISP_INFO_HEIGHT,
                        USB_DISP_INFO_IGNORE_EDID);
    usb_disp_set_edid_policy(disp,
                             (usb_disp_edid_policy_t)USB_DISP_INFO_EDID_POLICY);
    usb_disp_start();
}

void loop() {
    if (usb_disp_poll(disp)) {
        if (usb_disp_ready(disp)) {
            fit_mode();   // モニタが指定解像度未満なら実解像度へ (例: DL-120 内蔵 800x600)
            draw_info();
            shown = true;
        } else {
            shown = false;
        }
    }

    // 毎秒: 画面のステータス更新 + シリアルミラー (未接続時はシリアルのみ)
    {
        static uint32_t last = 0;
        if (millis() - last >= 1000) {
            last = millis();
            status_tick();
        }
    }

    if (shown) {
#if USB_DISP_INFO_EDID_DUMP
        edid_dump_once();   // 解像度の決まり方を追うための EDID ダンプ
#endif
        // アクティビティバー (約25fps)
        static uint32_t bar_at = 0;
        if (millis() - bar_at >= 40) {
            bar_at = millis();
            draw_activity_bar();
        }

        // モニタが後から挿されたモードを設定し直して描き直す (HPD で出力が落ちるため)
        // 固定解像度モードでは EDID を読まないのでこの監視自体を行わない
        // EDID が既に有効なら監視間隔を延ばす
        if (!usb_disp_ignore_edid(disp)) {
            static uint32_t edid_at = 0;
            static int edid_prev = -2;   // -2 = 未取得
            uint32_t iv = (g_est == 2) ? 10000 : 3000;
            if (millis() - edid_at >= iv) {
                edid_at = millis();
                int est = edid_state(&g_elen);
                if (est >= 0) {          // 判定不能 (-1) は前回値を維持
                    g_est = (int8_t)est;
                    if (edid_prev >= 0 && edid_prev <= 1 && est == 2) {
                        remode();  // モニタが後から挿された -> 再設定+再描画
                    }
                    edid_prev = est;
                }
            }
        }
    }

    delay(10);
}


