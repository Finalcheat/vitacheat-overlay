#include "../version.h"
#include "../storage_paths.h"
#include "overlay_view.h"
#include "settings_text.h"
#include "../platform/resource_profile.h"
extern "C" {
#include "text.h"
}
#include <psp2/kernel/clib.h>
#include <paf/std/ngp/string.h>
#include <paf/std/ngp/wchar.h>
#undef WEOF
#include <paf/plugin.h>
#include <paf/widget/w_text.h>
#include <paf/widget/w_scene.h>
#include "resource_ids.h"
#include "diagnostics.h"
using namespace paf;
extern "C" int text_set_string(ui::Widget *, const paf::wstring &, int) asm(
    "_ZN3paf2ui4Text9SetStringERKNS_12basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEEEi");
static Plugin *plugin;
static ui::Scene *page;
static ui::Widget *title, *game_version, *identity, *eyebrow, *footer, *credit, *message,
    *highlight, *pagination, *empty_state, *background, *focus_marker;
static ui::Widget *names[VCO_PAGE_SIZE], *numbers[VCO_PAGE_SIZE], *states[VCO_PAGE_SIZE];
static ui::Widget *enabled_background[VCO_PAGE_SIZE];
static ui::Widget *switch_on[VCO_PAGE_SIZE], *switch_off[VCO_PAGE_SIZE],
    *switch_thumb[VCO_PAGE_SIZE];

namespace palette
{
/* This suprx enters module_start directly, without a C++ startup pass.
 * Construct PAF vectors at use time: global v4 objects would remain zero. */
static math::v4 text()
{
    return math::v4(0.95f, 0.97f, 1.0f, 1.0f);
}
static math::v4 index()
{
    return math::v4(0.57f, 0.64f, 0.69f, 1.0f);
}
static math::v4 enabled_row()
{
    return math::v4(0.16f, 0.46f, 0.35f, 0.60f);
}
static math::v4 enabled_name()
{
    return math::v4(0.70f, 0.95f, 0.82f, 1.0f);
}
static math::v4 enabled_index()
{
    return math::v4(0.49f, 0.77f, 0.64f, 1.0f);
}
static math::v4 warning()
{
    return math::v4(1.0f, 0.73f, 0.50f, 1.0f);
}
} // namespace palette

enum {
    OPACITY_LIGHT,
    OPACITY_DARK,
    OPACITY_TINT,
    OPACITY_TRACK,
    OPACITY_FILL,
    OPACITY_THUMB,
    OPACITY_PART_COUNT
};
static ui::Widget *opacity_parts[OPACITY_PART_COUNT], *opacity_preview;
static bool attempted, cooldown, header_has_version;
static float header_version_offset = -1;
static uint32_t reopen_after;
static const char *const PAGE_ID = "psvs_page_hud";
static_assert(sizeof(Plugin::InitParam) == 0x94, "PAF InitParam ABI changed");
static_assert(sizeof(Plugin::PageOpenParam) == 0x2c, "PAF PageOpenParam ABI changed");
static_assert(sizeof(IDParam) == 0x10, "PAF IDParam ABI changed");
static_assert(sizeof(Plugin::TemplateOpenParam) == 2 * sizeof(void *),
              "PAF TemplateOpenParam ABI changed");
static_assert(sizeof(wchar_t) == 2, "PAF requires UTF-16");
static_assert(VCO_RESOURCE_SWITCH_ROWS == VCO_PAGE_SIZE, "RCO/native page size mismatch");
struct Words {
    const char *unknown, *footer, *empty;
    const char *loading, *no_game, *ambiguous, *discovery, *missing, *io, *parse, *limit;
    const char *own, *legacy;
};
static const Words words[VCO_LANGUAGE_COUNT] = {
    {"Unknown name", "↑↓ Select   L/R Page   □ Toggle   × Back   △ Settings",
     "No entries in this file", "Loading…", "Launch a game to view its list",
     "Multiple apps running - cannot select one", "Unable to identify the running application",
     "No cheat file for this Title ID", "Unable to read cheat file", "Invalid cheat file",
     "List exceeds resource limit", "Project cheats", "VitaCheat database"},
    {"未知名称", "↑↓ 选择   L/R 翻页   □ 切换   × 返回   △ 设置", "文件中没有金手指条目",
     "正在读取…", "请先启动游戏", "多个应用正在运行，无法确定目标", "暂时无法识别运行中的应用",
     "未找到该游戏编号对应的金手指文件", "金手指文件读取失败", "金手指文件格式错误",
     "列表超出当前资源限制", "项目 cheats 目录", "VitaCheat 数据库"},
    {"未知名稱", "↑↓ 選擇   L/R 翻頁   □ 切換   × 返回   △ 設定", "檔案中沒有金手指項目",
     "正在讀取…", "請先啟動遊戲", "多個應用正在執行，無法確定目標", "暫時無法識別執行中的應用",
     "未找到對應遊戲編號的金手指檔案", "金手指檔案讀取失敗", "金手指檔案格式錯誤",
     "清單超出目前資源限制", "專案 cheats 目錄", "VitaCheat 資料庫"},
    {"名前不明", "↑↓ 選択   L/R ページ   □ 切替   × 戻る   △ 設定",
     "ファイルにチート項目がありません", "読込中…", "先にゲームを起動してください",
     "複数のアプリが実行中 · 対象を特定できません", "実行中のアプリを識別できません",
     "このゲームIDのチートファイルがありません", "チートファイルを読み込めません",
     "チートファイルの形式が不正です", "一覧がリソース上限を超えています", "プロジェクトのコード",
     "VitaCheatデータベース"},
    {"이름 없음", "↑↓ 선택   L/R 페이지   □ 전환   × 뒤로   △ 설정", "파일에 치트 항목이 없습니다",
     "불러오는 중…", "먼저 게임을 실행하세요", "여러 앱이 실행 중 · 대상을 선택할 수 없음",
     "실행 중인 앱을 식별할 수 없습니다", "이 게임 ID의 치트 파일이 없습니다",
     "치트 파일을 읽을 수 없습니다", "치트 파일 형식 오류", "목록이 리소스 한도를 초과했습니다",
     "프로젝트 치트", "VitaCheat 데이터베이스"}};
static void label(ui::Widget *widget, const char *text)
{
    uint16_t buffer[192];
    vco_utf8_to_utf16(text, buffer, 192);
    paf::wstring str(reinterpret_cast<const wchar_t *>(buffer));
    text_set_string(widget, str, 0);
}
static ui::Widget *make(const char *type, const char *id, const char *style, float w, float h,
                        float x, float y)
{
    ui::Widget *widget = plugin->CreateWidget(page, type, IDParam(id), IDParam(style));
    if (widget) {
        widget->SetSize(math::v4(w, h, 0, 0));
        widget->SetPos(math::v4(x, y, 0, 0));
    }
    return widget;
}
enum TextRole { TEXT_BODY, TEXT_LARGE, TEXT_HEADER, TEXT_INDEX };
static ui::Widget *text(const char *id, float w, float h, float x, float y,
                        TextRole role = TEXT_BODY)
{
    ui::Widget *widget = make("text", id,
                              role == TEXT_HEADER  ? VCO_HEADER_STYLE
                              : role == TEXT_INDEX ? VCO_INDEX_STYLE
                              : role == TEXT_LARGE ? "psvs_style_text_impose"
                                                   : "psvs_style_text_normal",
                              w, h, x, y);
    if (widget) {
        widget->SetColor(palette::text());
        ui::Text *value = static_cast<ui::Text *>(widget);
        value->SetLayoutAttribute(graph::TextLayoutAttribute_WordWrap, false);
        value->SetLayoutAttribute(graph::TextLayoutAttribute_Ellipsis, true);
        value->SetLayoutAttribute(graph::TextLayoutAttribute_AlignX,
                                  static_cast<int32_t>(graph::TextLayoutAlignX_Left));
    }
    return widget;
}
static void hide_switches(void)
{
    for (unsigned i = 0; i < VCO_PAGE_SIZE; ++i) {
        switch_on[i]->SetColor(math::v4(0, 0, 0, 0));
        switch_off[i]->SetColor(math::v4(0, 0, 0, 0));
        switch_thumb[i]->SetColor(math::v4(0, 0, 0, 0));
        /* Settings and pending/error states need the complete value column. */
        states[i]->SetSize(math::v4(140, 28, 0, 0));
        states[i]->SetPos(math::v4(378, VCO_UI_SCREEN_HEIGHT / 2.0f - vco_ui_row_center(i), 0, 0));
        static_cast<ui::Text *>(states[i])->SetLayoutAttribute(
            graph::TextLayoutAttribute_AlignX, static_cast<int32_t>(graph::TextLayoutAlignX_Left));
    }
}
static void reset_row_appearance(void)
{
    for (unsigned i = 0; i < VCO_PAGE_SIZE; ++i) {
        enabled_background[i]->SetColor(math::v4(0, 0, 0, 0));
        names[i]->SetColor(palette::text());
        numbers[i]->SetColor(palette::index());
    }
}
static void show_focus(unsigned row, bool visible)
{
    float y = VCO_UI_SCREEN_HEIGHT / 2.0f - vco_ui_row_center(row);
    highlight->SetColor(math::v4(0.16f, 0.46f, 0.72f, visible ? 0.28f : 0));
    highlight->SetPos(math::v4(0, y, 0, 0));
    focus_marker->SetColor(math::v4(0.72f, 0.90f, 1, visible ? 1.0f : 0));
    focus_marker->SetPos(math::v4(-445, y, 0, 0));
}
static void show_switch(unsigned row, bool on)
{
    float y = VCO_UI_SCREEN_HEIGHT / 2.0f - vco_ui_row_center(row);
    states[row]->SetSize(math::v4(76, 28, 0, 0));
    states[row]->SetPos(math::v4(340, y, 0, 0));
    static_cast<ui::Text *>(states[row])
        ->SetLayoutAttribute(graph::TextLayoutAttribute_AlignX,
                             static_cast<int32_t>(graph::TextLayoutAlignX_Right));
    switch_on[row]->SetColor(math::v4(0.47f, 0.84f, 0.69f, on ? 1.0f : 0.0f));
    switch_off[row]->SetColor(math::v4(0.45f, 0.51f, 0.57f, on ? 0.0f : 1.0f));
    switch_thumb[row]->SetPos(math::v4(on ? 434 : 398, y, 0, 0));
    switch_thumb[row]->SetColor(on ? math::v4(0.07f, 0.23f, 0.17f, 1)
                                   : math::v4(0.70f, 0.75f, 0.79f, 1));
}
static bool create_switches(void)
{
    /* Same TemplateOpen/FindChild path as the upstream PSVshellPlus HUD.
     * PAF's resource manager decodes/retains the three shared PNGs. VCO does
     * not call the unverified Surface::Load or virtual SetTexture boundary. */
    Plugin::TemplateOpenParam open;
    open.event_handler_table = NULL;
    open.sibling = NULL;
    int result = plugin->TemplateOpen(page, IDParam(VCO_SWITCH_TEMPLATE), open);
    if (result < 0) {
        vco_log("switch_template_failure", (unsigned)result, 0);
        return false;
    }
    ui::Widget *group = page->FindChild(IDParam(VCO_SWITCH_GROUP));
    if (!group) {
        vco_log("switch_group_missing", 0, 0);
        return false;
    }
    group->SetSize(math::v4(VCO_UI_SCREEN_WIDTH, VCO_UI_SCREEN_HEIGHT, 0, 0));
    group->SetPos(math::v4(0, 0, 0, 0));
    group->SetColor(math::v4(1, 1, 1, 1));
    for (unsigned row = 0; row < VCO_PAGE_SIZE; ++row) {
        ui::Widget **widgets[] = {&switch_on[row], &switch_off[row], &switch_thumb[row]};
        float y = VCO_UI_SCREEN_HEIGHT / 2.0f - vco_ui_row_center(row);
        for (unsigned i = 0; i < 3; ++i) {
            *widgets[i] = page->FindChild(IDParam(vco_switch_ids[row][i]));
            if (!*widgets[i]) {
                vco_log("switch_plane_missing", row * 3 + i, 0);
                return false;
            }
            (*widgets[i])->SetSize(math::v4(i == 2 ? 20 : 64, i == 2 ? 20 : 28, 0, 0));
            (*widgets[i])->SetPos(math::v4(416, y, 0, 0));
        }
    }
    hide_switches();
    return true;
}
static void hide_opacity(void)
{
    for (unsigned i = 0; i < OPACITY_PART_COUNT; ++i)
        opacity_parts[i]->SetColor(math::v4(0, 0, 0, 0));
    label(opacity_preview, "");
}
static void opacity_part(unsigned index, float x, float y, float w, float h, const math::v4 &color)
{
    ui::Widget *widget = opacity_parts[index];
    widget->SetSize(math::v4(w, h, 0, 0));
    widget->SetPos(math::v4(x - VCO_UI_SCREEN_WIDTH / 2.0f, y, 0, 0));
    widget->SetColor(color);
}
static void show_opacity(unsigned value, unsigned row, int language)
{
    float y = VCO_UI_SCREEN_HEIGHT / 2.0f - vco_ui_row_center(row);
    float length = (VCO_UI_OPACITY_RIGHT - VCO_UI_OPACITY_LEFT) * value / 100.0f;
    /* An opaque light/dark reference makes the local preview independent of
     * the readable settings background and the current game frame. */
    opacity_part(OPACITY_LIGHT, 380, y, 56, 32, math::v4(0.80f, 0.84f, 0.88f, 1));
    opacity_part(OPACITY_DARK, 436, y, 56, 32, math::v4(0.18f, 0.22f, 0.26f, 1));
    opacity_part(OPACITY_TINT, 408, y, 112, 32, math::v4(0.055f, 0.086f, 0.118f, value / 100.0f));
    opacity_part(OPACITY_TRACK, (VCO_UI_OPACITY_LEFT + VCO_UI_OPACITY_RIGHT) / 2.0f, y,
                 VCO_UI_OPACITY_RIGHT - VCO_UI_OPACITY_LEFT, 6, math::v4(0.34f, 0.40f, 0.46f, 1));
    opacity_part(OPACITY_FILL, VCO_UI_OPACITY_LEFT + length / 2.0f, y, length, 6,
                 math::v4(0.35f, 0.82f, 0.70f, value ? 1.0f : 0.0f));
    opacity_part(OPACITY_THUMB, VCO_UI_OPACITY_LEFT + length, y, 12, 26,
                 math::v4(0.95f, 0.97f, 1, 1));
    opacity_preview->SetPos(math::v4(408 - VCO_UI_SCREEN_WIDTH / 2.0f, y, 0, 0));
    label(opacity_preview, vco_settings_text(language).preview);
}
static void name_geometry(unsigned row, bool opacity)
{
    names[row]->SetSize(math::v4(opacity ? 244 : 668, 38, 0, 0));
    names[row]->SetPos(
        math::v4(opacity ? -274 : -62, VCO_UI_SCREEN_HEIGHT / 2.0f - vco_ui_row_center(row), 0, 0));
}
/* The installed version follows the rendered name, within the original left
 * column. PAF keeps the long name ellipsis; only empty trailing space moves. */
static void game_title_geometry(bool has_version)
{
    header_has_version = has_version;
    const float width = has_version ? 180.0f : 268.0f;
    title->SetSize(math::v4(width, 28, 0, 0));
    title->SetPos(math::v4(32 + width / 2 - 480, 232, 0, 0));
    header_version_offset = -1;
    if (has_version)
        game_version->SetPos(math::v4(-224, 232, 0, 0));
}
void vco_view_update_header_layout(void)
{
    if (!page || !header_has_version)
        return;
    math::v2 bounds = {};
    /* Layout can finish after SetString. Retry on the existing UI callback;
     * invalid/pending measurements retain the bounded initial position. */
    if (static_cast<ui::Text *>(title)->GetBounds(bounds) < 0)
        return;
    float width = bounds.extract_x();
    if (!(width > 0)) /* Includes pending zero and NaN. */
        return;
    if (width > 180)
        width = 180;
    if (header_version_offset != width) {
        game_version->SetPos(math::v4(32 + width + 88 / 2.0f - 480, 232, 0, 0));
        header_version_offset = width;
    }
}
void vco_view_close(uint32_t tick)
{
    vco_resource_profile_visible(false);
    if (!page)
        return;
    Plugin::PageCloseParam param;
    param.fade = false;
    plugin->PageClose(IDParam(PAGE_ID), param);
    page = NULL;
    header_has_version = false;
    header_version_offset = -1;
    reopen_after = tick + 300000u;
    cooldown = true;
}
static void loaded(Plugin *value)
{
    plugin = value;
}
bool vco_view_open(uint32_t tick)
{
    if (page)
        return true;
    if (cooldown && (int32_t)(tick - reopen_after) < 0)
        return false;
    cooldown = false;
    if (!plugin && !attempted) {
        attempted = true;
        Plugin::InitParam init;
        init.name = "vco_overlay_plugin";
        init.caller_name = "__main__";
        init.resource_file = VCO_RESOURCE_PATH;
        init.start_func = loaded;
        Plugin::LoadSync(init);
    }
    if (!plugin)
        return false;
    Plugin::PageOpenParam open;
    open.overwrite_draw_priority = 6;
    open.fade = false;
    page = plugin->PageOpen(IDParam(PAGE_ID), open);
    if (!page)
        return false;
    /* Background is a sibling: its 70% alpha must not dim text descendants. */
    background = make("plane", "vco_panel", "psvs_style_plane_hud_bg", VCO_UI_SCREEN_WIDTH,
                      VCO_UI_SCREEN_HEIGHT, 0, 0);
    if (!background) {
        vco_view_close(tick);
        return false;
    }
    background->SetColor(math::v4(0.055f, 0.086f, 0.118f, 0.70f));
    /* State tints are siblings behind focus and text, so their opacity never
     * fades labels or switches. Only six fixed planes are retained. */
    for (unsigned i = 0; i < VCO_PAGE_SIZE; ++i) {
        char id[32];
        sceClibSnprintf(id, sizeof(id), "vco_enabled_row_%u", i);
        enabled_background[i] = make("plane", id, "psvs_style_plane_hud_bg", 896, VCO_UI_ROW_HEIGHT,
                                     0, VCO_UI_SCREEN_HEIGHT / 2.0f - vco_ui_row_center(i));
        if (!enabled_background[i]) {
            vco_view_close(tick);
            return false;
        }
        enabled_background[i]->SetColor(math::v4(0, 0, 0, 0));
    }
    highlight = make("plane", "vco_focus", "psvs_style_plane_hud_bg", 904, VCO_UI_ROW_HEIGHT, 0,
                     VCO_UI_SCREEN_HEIGHT / 2.0f - vco_ui_row_center(0));
    if (!highlight) {
        vco_view_close(tick);
        return false;
    }
    focus_marker = make("plane", "vco_focus_marker", "psvs_style_plane_hud_bg", 6, 34, -445,
                        VCO_UI_SCREEN_HEIGHT / 2.0f - vco_ui_row_center(0));
    if (!focus_marker) {
        vco_view_close(tick);
        return false;
    }
    show_focus(0, true);
    /* Bounded columns keep long game names away from the centered brand. */
    title = text("vco_game", 268, 28, -314, 232, TEXT_HEADER);
    game_version = text("vco_game_version", 88, 28, -224, 232, TEXT_HEADER);
    eyebrow = text("vco_brand", 340, 28, 0, 232, TEXT_HEADER);
    identity = text("vco_id", 206, 28, 345, 232, TEXT_HEADER);
    empty_state = text("vco_empty", 896, 40, 0, 8, TEXT_LARGE);
    message = text("vco_message", 704, 28, -96, -212);
    pagination = text("vco_pagination", 184, 28, 356, -212);
    footer = text("vco_footer", 744, 28, -76, -251);
    credit = text("vco_credit", 136, 28, 380, -251, TEXT_INDEX);
    if (!eyebrow || !title || !game_version || !identity || !message || !footer || !credit ||
        !pagination || !empty_state) {
        vco_view_close(tick);
        return false;
    }
    static_cast<ui::Text *>(credit)->SetLayoutAttribute(
        graph::TextLayoutAttribute_AlignX, static_cast<int32_t>(graph::TextLayoutAlignX_Right));
    credit->SetColor(palette::index());
    label(credit, "by FinalCheat");
    label(game_version, "");
    static_cast<ui::Text *>(eyebrow)->SetLayoutAttribute(
        graph::TextLayoutAttribute_AlignX, static_cast<int32_t>(graph::TextLayoutAlignX_Center));
    static_cast<ui::Text *>(identity)->SetLayoutAttribute(
        graph::TextLayoutAttribute_AlignX, static_cast<int32_t>(graph::TextLayoutAlignX_Right));
    static_cast<ui::Text *>(pagination)
        ->SetLayoutAttribute(graph::TextLayoutAttribute_AlignX,
                             static_cast<int32_t>(graph::TextLayoutAlignX_Right));
    const float divider_y[] = {196.0f, -182.0f};
    for (unsigned i = 0; i < 2; ++i) {
        const char *id = i ? "vco_list_bottom_rule" : "vco_list_top_rule";
        ui::Widget *rule = make("plane", id, "psvs_style_plane_hud_bg", 896, 3, 0, divider_y[i]);
        if (!rule) {
            vco_view_close(tick);
            return false;
        }
        rule->SetColor(math::v4(0.78f, 0.85f, 0.90f, 0.88f));
    }
    for (unsigned i = 0; i < VCO_PAGE_SIZE; ++i) {
        char id[32];
        float y = VCO_UI_SCREEN_HEIGHT / 2.0f - vco_ui_row_center(i);
        sceClibSnprintf(id, sizeof(id), "vco_name_%u", i);
        names[i] = text(id, 668, 38, -62, y, TEXT_LARGE);
        sceClibSnprintf(id, sizeof(id), "vco_number_%u", i);
        numbers[i] = text(id, 36, 28, -417, y, TEXT_INDEX);
        sceClibSnprintf(id, sizeof(id), "vco_state_%u", i);
        states[i] = text(id, 140, 28, 378, y);
        if (!names[i] || !numbers[i] || !states[i]) {
            vco_view_close(tick);
            return false;
        }
    }
    if (!create_switches()) {
        vco_view_close(tick);
        return false;
    }
    reset_row_appearance();
    for (unsigned i = 0; i < OPACITY_PART_COUNT; ++i) {
        char id[32];
        sceClibSnprintf(id, sizeof(id), "vco_opacity_%u", i);
        opacity_parts[i] = make("plane", id, "psvs_style_plane_hud_bg", 1, 1, 0, 0);
        if (!opacity_parts[i]) {
            vco_view_close(tick);
            return false;
        }
    }
    opacity_preview = text("vco_opacity_preview", 112, 28, 0, 0);
    if (!opacity_preview) {
        vco_view_close(tick);
        return false;
    }
    static_cast<ui::Text *>(opacity_preview)
        ->SetLayoutAttribute(graph::TextLayoutAttribute_AlignX,
                             static_cast<int32_t>(graph::TextLayoutAlignX_Center));
    hide_opacity();
    vco_log("page_ready_execution", 1, 0);
    vco_resource_profile_visible(true);
    return true;
}
void vco_view_render(const VcoCatalog &catalog, const VcoExecutionView &execution,
                     const VcoExecutionUi &commands, const VcoCatalogSourceView &file_source,
                     unsigned page_index, unsigned selected, int language,
                     VcoExecutionNotice notice)
{
    if (!page)
        return;
    hide_opacity();
    hide_switches();
    reset_row_appearance();
    for (unsigned i = 0; i < VCO_PAGE_SIZE; ++i)
        name_geometry(i, false);
    language = vco_language_normalize(language);
    const Words &t = words[language];
    char buffer[256];
    label(eyebrow, "VitaCheat Overlay " VCO_VERSION);
    bool has_version = catalog.game.title_id[0] && catalog.game.version[0];
    game_title_geometry(has_version);
    if (has_version) {
        /* APP_VER comes from installed game metadata, never cheat comments. */
        sceClibSnprintf(buffer, sizeof(buffer), " · v%.*s", (int)sizeof(catalog.game.version),
                        catalog.game.version);
        label(game_version, buffer);
    } else
        label(game_version, "");
    label(title, catalog.game.title[0]      ? catalog.game.title
                 : catalog.game.title_id[0] ? t.unknown
                                            : "—");
    vco_view_update_header_layout();
    label(identity, catalog.game.title_id[0] ? catalog.game.title_id : "—");
    bool matched = vco_execution_view_matches(&catalog, &execution);
    unsigned pages = catalog.count ? (catalog.count + VCO_PAGE_SIZE - 1) / VCO_PAGE_SIZE : 1;
    sceClibSnprintf(buffer, sizeof(buffer), "< %u/%u >", page_index + 1, pages);
    label(pagination, buffer);
    label(footer, t.footer);
    const char *list_status = "";
    switch (catalog.state) {
    case VCO_LIST_LOADING:
        list_status = t.loading;
        break;
    case VCO_LIST_NO_GAME:
        list_status = t.no_game;
        break;
    case VCO_LIST_AMBIGUOUS:
        list_status = t.ambiguous;
        break;
    case VCO_LIST_DISCOVERY_ERROR:
        list_status = t.discovery;
        break;
    case VCO_LIST_MISSING:
        list_status = t.missing;
        break;
    case VCO_LIST_IO_ERROR:
        list_status = t.io;
        break;
    case VCO_LIST_PARSE_ERROR:
        list_status = catalog.diagnostic.code == VCO_E_ENCODING
                          ? vco_settings_text(language).encoding_error
                          : t.parse;
        break;
    case VCO_LIST_LIMIT:
        list_status = t.limit;
        break;
    case VCO_LIST_READY:
        list_status = catalog.count ? "" : t.empty;
        break;
    }
    uint32_t selected_error = 0, selected_line = 0;
    for (unsigned i = 0; i < VCO_PAGE_SIZE; ++i) {
        unsigned index = page_index * VCO_PAGE_SIZE + i;
        if (catalog.state == VCO_LIST_READY && index < catalog.count &&
            index < VCO_CATALOG_ENTRIES) {
            const VcoCatalogRow &row = catalog.rows[index];
            VcoExecutionDisplay state =
                vco_execution_display(&catalog, &execution, &commands, index);
            sceClibSnprintf(buffer, sizeof(buffer), "%02u", index + 1);
            label(numbers[i], buffer);
            label(names[i], row.name);
            label(states[i], vco_execution_display_text(state, language));
            math::v4 color(0.66f, 0.73f, 0.79f, 1);
            if (state == VCO_DISPLAY_ON) {
                color = math::v4(0.40f, 0.94f, 0.73f, 1);
                enabled_background[i]->SetColor(palette::enabled_row());
                names[i]->SetColor(palette::enabled_name());
                numbers[i]->SetColor(palette::enabled_index());
            } else if (state == VCO_DISPLAY_ERROR || state == VCO_DISPLAY_UNSUPPORTED ||
                       state == VCO_DISPLAY_PAUSED)
                color = palette::warning();
            else if (state == VCO_DISPLAY_QUEUED || state == VCO_DISPLAY_ENABLING ||
                     state == VCO_DISPLAY_DISABLING)
                color = math::v4(0.55f, 0.82f, 1.0f, 1);
            states[i]->SetColor(color);
            if (state == VCO_DISPLAY_ON || state == VCO_DISPLAY_OFF)
                show_switch(i, state == VCO_DISPLAY_ON);
            if (i == selected) {
                if (matched && execution.known[index]) {
                    selected_error = execution.rows[index].error;
                    selected_line = execution.rows[index].line;
                } else {
                    selected_error = row.diagnostic.code ? row.diagnostic.code
                                     : row.needs_review  ? VCO_E_UNVERIFIED
                                                         : 0;
                    selected_line = row.diagnostic.line;
                }
            }
        } else {
            label(numbers[i], "");
            label(names[i], "");
            label(states[i], "");
        }
    }
    bool have_rows = catalog.state == VCO_LIST_READY && catalog.count;
    label(empty_state, have_rows ? "" : list_status);
    show_focus(selected, have_rows);
    bool service_error = execution.state == VCO_EXECUTION_ERROR ||
                         execution.state == VCO_EXECUTION_UNAVAILABLE ||
                         execution.state == VCO_EXECUTION_CONFLICT;
    const char *status = vco_execution_state_text(execution.state, language);
    if (service_error) {
        label(message, execution.state == VCO_EXECUTION_ERROR && execution.error
                           ? vco_execution_error_text(execution.error, language)
                           : status);
        message->SetColor(palette::warning());
        return;
    }
    message->SetColor(palette::text());
    if (notice == VCO_NOTICE_RELOADING) {
        label(message, vco_execution_notice_text(notice, language));
        return;
    }
    if (have_rows && !matched)
        status = vco_execution_state_text(VCO_EXECUTION_LOADING, language);
    if (have_rows && matched && !selected_error && execution.error)
        selected_error = execution.error;
    if (have_rows && selected_error) {
        message->SetColor(palette::warning());
        if (selected_line)
            sceClibSnprintf(buffer, sizeof(buffer), "%s · L%u",
                            vco_execution_error_text(selected_error, language), selected_line);
        else
            sceClibSnprintf(buffer, sizeof(buffer), "%s",
                            vco_execution_error_text(selected_error, language));
        label(message, buffer);
        return;
    }
    if (notice != VCO_NOTICE_NONE) {
        label(message, vco_execution_notice_text(notice, language));
        return;
    }
    if (file_source.catalog_revision == catalog.revision) {
        const char *source_status = vco_catalog_source_text(file_source.state, language);
        if (source_status[0]) {
            label(message, source_status);
            return;
        }
    }
    const char *source = catalog.source == VCO_SOURCE_OWN         ? t.own
                         : catalog.source == VCO_SOURCE_VITACHEAT ? t.legacy
                                                                  : "";
    const char *encoding = catalog.encoding == VCO_ENCODING_GBK    ? " · GBK"
                           : catalog.encoding == VCO_ENCODING_UTF8 ? " · UTF-8"
                                                                   : "";
    if (have_rows) {
        label(message, status);
    } else {
        /* Keep file context available alongside the central loading error. */
        sceClibSnprintf(buffer, sizeof(buffer), "%s%s", source, encoding);
        label(message, buffer);
    }
}

void vco_view_opacity(unsigned opacity)
{
    if (page)
        background->SetColor(math::v4(0.055f, 0.086f, 0.118f, opacity / 100.0f));
}
static const char *binding_error_text(VcoBindingError error, int language)
{
    static const char *const messages[VCO_LANGUAGE_COUNT][7] = {
        {"", "Reserved button · choose another", "This action accepts one button",
         "Shortcut requires two or more buttons", "Button already assigned · choose another",
         "Opposite directions cannot form a shortcut", "Hold the shortcut buttons together"},
        {"", "该按键保留用于系统或导航，请选择其他按键", "此操作只能绑定一个按键",
         "快捷键须包含至少两个按键", "按键已被其他操作使用，请选择其他按键",
         "快捷键不能包含相反方向", "请同时按住快捷键中的全部按键"},
        {"", "該按鍵保留用於系統或導覽，請選擇其他按鍵", "此操作只能綁定一個按鍵",
         "快捷鍵須包含至少兩個按鍵", "按鍵已被其他操作使用，請選擇其他按鍵",
         "快捷鍵不能包含相反方向", "請同時按住快捷鍵中的全部按鍵"},
        {"", "このキーはシステムまたは操作用に予約されています", "キーは1つだけ指定してください",
         "ショートカットには2つ以上のキーが必要です",
         "このキーは使用済み · 別のキーを選んでください", "反対方向のキーは組み合わせられません",
         "組合せのすべてのキーを同時に押してください"},
        {"", "시스템 또는 탐색용으로 예약된 버튼입니다", "버튼 하나만 지정하세요",
         "단축키에는 두 개 이상의 버튼이 필요합니다",
         "이미 사용 중인 버튼 · 다른 버튼을 선택하세요",
         "반대 방향의 버튼은 함께 사용할 수 없습니다", "단축키의 모든 버튼을 동시에 누르세요"}};
    unsigned index = (unsigned)error;
    language = vco_language_normalize(language);
    return index < 7 ? messages[language][index] : messages[language][VCO_BINDING_RESERVED];
}
void vco_view_settings(const VcoSettingsState &settings, const VcoCatalog &catalog,
                       int system_language, bool reloading, unsigned first)
{
    if (!page)
        return;
    hide_opacity();
    hide_switches();
    reset_row_appearance();
    message->SetColor(palette::text());
    int language = vco_settings_language(&settings.draft, system_language);
    const VcoSettingsText &t = vco_settings_text(language);
    static const char *const languages[VCO_LANGUAGE_COUNT] = {"English", "简体中文", "繁體中文",
                                                              "日本語", "한국어"};
    game_title_geometry(false);
    label(game_version, "");
    label(title, t.heading);
    label(pagination, "");
    label(empty_state, "");
    char confirm_button[8], back_button[8], navigation[192];
    vco_binding_name(vco_settings_confirm_button(&settings.applied), confirm_button,
                     sizeof(confirm_button));
    vco_binding_name(vco_settings_back_button(&settings.applied), back_button, sizeof(back_button));
    sceClibSnprintf(navigation, sizeof(navigation), t.navigation, confirm_button, back_button);
    label(footer, navigation);
    label(message, t.unsaved);
    if (!settings.confirm && settings.selected == VCO_SETTING_BUTTON_MODE)
        label(message, t.pair_hint);
    int selected_binding = settings.confirm ? -1 : vco_settings_binding_row(settings.selected);
    if (selected_binding >= 0) {
        char binding[160], help[256];
        vco_binding_name(vco_binding_value(&settings.draft.bindings, (unsigned)selected_binding),
                         binding, sizeof(binding));
        sceClibSnprintf(help, sizeof(help), t.binding_help, confirm_button, binding);
        label(message, help);
    }
    if (settings.capture.active) {
        label(footer, t.capture_footer);
        label(message, selected_binding == VCO_BIND_OPEN ? t.capture_shortcut : t.capture_single);
        if (settings.capture_error != VCO_BINDING_OK)
            label(message, binding_error_text(settings.capture_error, language));
    }
    if (settings.apply_pending) {
        label(message, t.apply_neutral);
        label(footer, "");
    }
    if (!settings.confirm && !settings.capture.active && !settings.apply_pending &&
        settings.selected == VCO_SETTING_RELOAD)
        label(message, reloading ? vco_execution_notice_text(VCO_NOTICE_RELOADING, language)
                                 : t.reload_help);
    if (!settings.confirm && !settings.capture.active && !settings.apply_pending &&
        settings.selected == VCO_SETTING_ENCODING) {
        label(message, t.encoding_help);
        const char *encoding = catalog.encoding == VCO_ENCODING_GBK    ? "GBK"
                               : catalog.encoding == VCO_ENCODING_UTF8 ? "UTF-8"
                                                                       : "";
        const Words &list_text = words[language];
        const char *source = catalog.source == VCO_SOURCE_OWN         ? list_text.own
                             : catalog.source == VCO_SOURCE_VITACHEAT ? list_text.legacy
                                                                      : "";
        if (source[0] && encoding[0]) {
            char help[256];
            sceClibSnprintf(help, sizeof(help), t.encoding_source, source, encoding);
            label(message, help);
        }
    }
    if (!settings.confirm && !settings.capture.active && !settings.apply_pending &&
        settings.selected == VCO_SETTING_OPACITY)
        label(message, t.opacity_help);
    unsigned count = settings.confirm ? 3 : VCO_VIEW_SETTING_COUNT;
    if (!settings.confirm) {
        char position[24];
        sceClibSnprintf(position, sizeof(position), "%u / %u", settings.selected + 1, count);
        label(pagination, position);
    }
    for (unsigned i = 0; i < VCO_PAGE_SIZE; ++i) {
        label(numbers[i], "");
        label(states[i], "");
        states[i]->SetColor(palette::text());
        unsigned row = first + i;
        bool opacity_row = !settings.confirm && row == VCO_SETTING_OPACITY;
        name_geometry(i, opacity_row);
        if (opacity_row)
            show_opacity(settings.draft.opacity, i, language);
        label(names[i], row < count ? (settings.confirm ? t.prompts[row] : t.labels[row]) : "");
        if (!settings.confirm) {
            const char *value = "";
            char opacity[16], button[160];
            int binding = vco_settings_binding_row(row);
            if (binding >= 0) {
                uint32_t mask = vco_binding_value(&settings.draft.bindings, (unsigned)binding);
                size_t length = vco_binding_name(mask, button, sizeof(button));
                /* The selected-row help above keeps the complete chord. */
                if (vco_button_count(mask) >= 4 || length > 14)
                    sceClibSnprintf(button, sizeof(button), t.key_count, vco_button_count(mask));
                value = settings.capture.active && row == settings.selected ? "…" : button;
            }
            switch (row) {
            case VCO_SETTING_BUTTON_MODE: {
                value = t.button_modes[settings.draft.button_mode == VCO_BUTTON_WESTERN];
                names[i]->SetSize(math::v4(440, 38, 0, 0));
                names[i]->SetPos(math::v4(84 + 440 / 2.0f - 480,
                                          VCO_UI_SCREEN_HEIGHT / 2.0f - vco_ui_row_center(i), 0,
                                          0));
                states[i]->SetSize(math::v4(300, 28, 0, 0));
                states[i]->SetPos(math::v4(928 - 300 / 2.0f - 480,
                                           VCO_UI_SCREEN_HEIGHT / 2.0f - vco_ui_row_center(i), 0,
                                           0));
                break;
            }
            case VCO_SETTING_LANGUAGE:
                value = settings.draft.language < 0
                            ? t.system
                            : languages[vco_language_normalize(settings.draft.language)];
                break;
            case VCO_SETTING_OPACITY:
                sceClibSnprintf(opacity, sizeof(opacity), "%u%%", settings.draft.opacity);
                value = opacity;
                break;
            case VCO_SETTING_ENCODING:
                value = settings.draft.encoding == VCO_ENCODING_UTF8  ? "UTF-8"
                        : settings.draft.encoding == VCO_ENCODING_GBK ? "GBK"
                                                                      : t.automatic;
                break;
            case VCO_SETTING_RELOAD:
                value = reloading ? "…" : confirm_button;
                break;
            default:
                break;
            }
            if (row < VCO_SETTING_APPLY && binding < 0) {
                char adjustable[64];
                sceClibSnprintf(adjustable, sizeof(adjustable), "‹ %s ›", value);
                label(states[i], adjustable);
            } else
                label(states[i], value);
        }
    }
    show_focus(settings.selected - first, true);
    /* Only the list and local sample follow the user's opacity. Navigation
     * stays readable even when the draft or applied value is zero. */
    vco_view_opacity(100);
}

void vco_view_bindings(const VcoSettings &settings, int system_language)
{
    if (!page)
        return;
    int language = vco_settings_language(&settings, system_language);
    static const char *const formats[] = {
        "↑↓ Select   %s/%s Page   %s Toggle   %s Back   %s Settings",
        "↑↓ 选择   %s/%s 翻页   %s 切换   %s 返回   %s 设置",
        "↑↓ 選擇   %s/%s 翻頁   %s 切換   %s 返回   %s 設定",
        "↑↓ 選択   %s/%s ページ   %s 切替   %s 戻る   %s 設定",
        "↑↓ 선택   %s/%s 페이지   %s 전환   %s 뒤로   %s 설정"};
    char buffer[256], binding[VCO_BIND_SETTINGS + 1u][32];
    for (unsigned i = 0; i <= VCO_BIND_SETTINGS; ++i)
        vco_binding_name(vco_binding_value(&settings.bindings, i), binding[i], sizeof(binding[i]));
    sceClibSnprintf(buffer, sizeof(buffer), formats[language], binding[VCO_BIND_PREVIOUS],
                    binding[VCO_BIND_NEXT], binding[VCO_BIND_TOGGLE], binding[VCO_BIND_CLOSE],
                    binding[VCO_BIND_SETTINGS]);
    label(footer, buffer);
}
void vco_view_save_status(VcoSaveStatus status, int language, bool in_settings)
{
    if (!page)
        return;
    static const char *const messages[VCO_LANGUAGE_COUNT][6] = {
        {"Apply settings to save", "Saving settings…", "Settings saved",
         "Save failed · current session only · apply to retry",
         "Previous settings recovered · apply to save", "Invalid settings · defaults loaded"},
        {"应用设置后保存", "正在保存设置…", "设置已保存", "保存失败 · 仅本次生效 · 请重新应用重试",
         "已恢复上次备份 · 应用设置后保存", "设置文件损坏 · 已使用默认设置"},
        {"套用設定後儲存", "正在儲存設定…", "設定已儲存", "儲存失敗 · 僅本次生效 · 請重新套用重試",
         "已恢復上次備份 · 套用設定後儲存", "設定檔案損壞 · 已使用預設設定"},
        {"設定を適用して保存", "設定を保存中…", "設定を保存しました",
         "保存失敗 · 今回のみ有効 · 適用で再試行", "前の設定を復元 · 適用して保存",
         "設定ファイルが不正 · 初期設定を使用"},
        {"설정을 적용하면 저장됩니다", "설정 저장 중…", "설정을 저장했습니다",
         "저장 실패 · 이번 실행에서만 적용 · 다시 적용하세요",
         "이전 설정 복구됨 · 적용하여 저장하세요", "설정 파일 오류 · 기본값을 불러왔습니다"}};
    if (in_settings || status == VCO_SAVE_PENDING || status >= VCO_SAVE_FAILED)
        label(message, messages[vco_language_normalize(language)]
                               [(unsigned)status < 6 ? status : VCO_SAVE_INVALID]);
}
