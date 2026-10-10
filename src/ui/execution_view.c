#include "execution_view.h"
#include "../localization.h"
#include <string.h>

static unsigned language_index(int language)
{
    return (unsigned)vco_language_normalize(language);
}
static bool newer(uint32_t value, uint32_t previous)
{
    return value != previous && (uint32_t)(value - previous) < 0x80000000u;
}
bool vco_execution_view_matches(const VcoCatalog *catalog, const VcoExecutionView *view)
{
    return catalog && view && catalog->state == VCO_LIST_READY && view->session &&
           catalog->execution_session == view->session && view->list_revision &&
           catalog->revision && view->catalog_revision == catalog->revision &&
           view->count == catalog->count && view->count <= VCO_ENGINE_ENTRIES;
}
void vco_execution_ui_sync(VcoExecutionUi *ui, const VcoExecutionView *view)
{
    if (!ui || !view)
        return;
    if (ui->seeded && view->revision != ui->view_revision &&
        !newer(view->revision, ui->view_revision))
        return;
    if (ui->seeded && view->revision == ui->view_revision &&
        (ui->session != view->session || ui->list_revision != view->list_revision))
        return;
    if (ui->session != view->session || ui->list_revision != view->list_revision) {
        memset(ui, 0, sizeof(*ui));
        ui->session = view->session;
        ui->list_revision = view->list_revision;
    }
    ui->seeded = true;
    ui->view_revision = view->revision;
    for (unsigned i = 0; i < VCO_ENGINE_ENTRIES; ++i)
        if (ui->pending[i] && newer(view->revision, ui->submitted_revision[i]))
            ui->pending[i] = false;
}
void vco_execution_ui_submitted(VcoExecutionUi *ui, const VcoExecutionView *view, unsigned entry)
{
    if (!ui || !view || entry >= view->count || entry >= VCO_ENGINE_ENTRIES)
        return;
    vco_execution_ui_sync(ui, view);
    if (ui->session != view->session || ui->list_revision != view->list_revision ||
        ui->view_revision != view->revision)
        return;
    ui->pending[entry] = true;
    ui->submitted_revision[entry] = view->revision;
}
static bool queued(const VcoExecutionView *view, const VcoExecutionUi *ui, unsigned entry)
{
    return view->queued[entry] || (ui && ui->session == view->session &&
                                   ui->list_revision == view->list_revision && ui->pending[entry]);
}
VcoExecutionDisplay vco_execution_display(const VcoCatalog *catalog, const VcoExecutionView *view,
                                          const VcoExecutionUi *ui, unsigned entry)
{
    if (!catalog || !view || entry >= catalog->count || entry >= VCO_ENGINE_ENTRIES)
        return VCO_DISPLAY_UNKNOWN;
    if (!vco_execution_view_matches(catalog, view) || !view->known[entry]) {
        const VcoCatalogRow *row = &catalog->rows[entry];
        if (row->diagnostic.code == VCO_E_UNVERIFIED || row->needs_review)
            return VCO_DISPLAY_UNSUPPORTED;
        return row->diagnostic.code ? VCO_DISPLAY_ERROR : VCO_DISPLAY_UNKNOWN;
    }
    if (view->state != VCO_EXECUTION_READY && view->state != VCO_EXECUTION_BACKGROUND) {
        if (view->rows[entry].state == VCO_ENTRY_ERROR)
            return VCO_DISPLAY_ERROR;
        if (view->rows[entry].state == VCO_ENTRY_UNSUPPORTED)
            return VCO_DISPLAY_UNSUPPORTED;
        return VCO_DISPLAY_UNKNOWN;
    }
    if (queued(view, ui, entry))
        return VCO_DISPLAY_QUEUED;
    const VcoControlRow *row = &view->rows[entry];
    switch (row->state) {
    case VCO_ENTRY_OFF:
        return VCO_DISPLAY_OFF;
    case VCO_ENTRY_ENABLING:
        return VCO_DISPLAY_ENABLING;
    case VCO_ENTRY_ON:
        return view->state == VCO_EXECUTION_BACKGROUND ? VCO_DISPLAY_PAUSED : VCO_DISPLAY_ON;
    case VCO_ENTRY_DISABLING:
        return VCO_DISPLAY_DISABLING;
    case VCO_ENTRY_PAUSED:
        return VCO_DISPLAY_PAUSED;
    case VCO_ENTRY_ERROR:
        return VCO_DISPLAY_ERROR;
    case VCO_ENTRY_UNSUPPORTED:
        return VCO_DISPLAY_UNSUPPORTED;
    default:
        return VCO_DISPLAY_UNKNOWN;
    }
}
bool vco_execution_toggle_target(const VcoCatalog *catalog, const VcoExecutionView *view,
                                 const VcoExecutionUi *ui, unsigned entry, bool *enabled)
{
    if (!enabled || !vco_execution_view_matches(catalog, view) || entry >= view->count ||
        !view->known[entry] || queued(view, ui, entry) || view->rows[entry].pending)
        return false;
    const VcoControlRow *row = &view->rows[entry];
    if (view->state != VCO_EXECUTION_READY && view->state != VCO_EXECUTION_BACKGROUND)
        return false;
    if (row->state == VCO_ENTRY_ON || row->state == VCO_ENTRY_PAUSED) {
        *enabled = false;
        return true;
    }
    if (view->state == VCO_EXECUTION_READY &&
        (row->state == VCO_ENTRY_OFF ||
         (row->state == VCO_ENTRY_ERROR && row->error != VCO_E_RESTORE &&
          row->error != VCO_E_METADATA)) &&
        catalog->compatibility != VCO_COMPAT_MISMATCH &&
        catalog->compatibility != VCO_COMPAT_CONFLICT) {
        *enabled = true;
        return true;
    }
    return false;
}
const char *vco_execution_display_text(VcoExecutionDisplay state, int language)
{
    static const char *const text[VCO_LANGUAGE_COUNT][9] = {
        {"UNKNOWN", "OFF", "ENABLING…", "ON", "DISABLING…", "PAUSED", "ERROR", "UNSUPPORTED",
         "QUEUED…"},
        {"未确认", "关闭", "开启中…", "开启", "关闭中…", "已暂停", "错误", "不支持", "排队中…"},
        {"未確認", "關閉", "開啟中…", "開啟", "關閉中…", "已暫停", "錯誤", "不支援", "排隊中…"},
        {"未確認", "オフ", "有効化中…", "オン", "無効化中…", "一時停止", "エラー", "未対応",
         "処理待ち…"},
        {"미확인", "꺼짐", "켜는 중…", "켜짐", "끄는 중…", "일시정지", "오류", "미지원",
         "대기 중…"}};
    return text[language_index(language)]
               [(unsigned)state <= VCO_DISPLAY_QUEUED ? state : VCO_DISPLAY_UNKNOWN];
}
const char *vco_execution_state_text(uint32_t state, int language)
{
    unsigned lang = language_index(language);
    static const char *const preparing[] = {"Preparing execution…", "正在准备执行服务…",
                                            "正在準備執行服務…", "実行サービスを準備中…",
                                            "실행 서비스 준비 중…"};
    static const char *const ready[] = {
        "Closing the overlay keeps enabled entries running", "关闭界面后，已开启条目继续生效",
        "關閉介面後，已開啟項目繼續生效", "一覧を閉じてもオンの項目は動作します",
        "목록을 닫아도 켜진 항목은 계속 적용됩니다"};
    static const char *const background[] = {
        "Execution paused · waiting for the game foreground", "已暂停执行 · 等待确认游戏前台状态",
        "已暫停執行 · 等待確認遊戲前景狀態", "実行を一時停止 · ゲームの前面表示を待機中",
        "실행 일시정지 · 게임의 전면 표시 대기 중"};
    static const char *const no_game[] = {"Launch a game to enable cheats", "请先启动游戏",
                                          "請先啟動遊戲", "先にゲームを起動してください",
                                          "먼저 게임을 실행하세요"};
    static const char *const error[] = {"Execution unavailable · reload the list to retry",
                                        "执行服务不可用 · 可重新加载列表重试",
                                        "執行服務無法使用 · 可重新載入清單重試",
                                        "実行サービスが利用不可 · 一覧を再読込してください",
                                        "실행 서비스 사용 불가 · 목록을 다시 불러오세요"};
    static const char *const conflict[] = {"Execution service conflict", "执行服务存在冲突",
                                           "執行服務存在衝突", "実行サービスが競合しています",
                                           "실행 서비스 충돌"};
    static const char *const unavailable[] = {
        "Execution service not started · restart PSV", "执行服务尚未启动 · 请重启 PSV",
        "執行服務尚未啟動 · 請重新啟動 PSV", "実行サービス未起動 · PSVを再起動してください",
        "실행 서비스가 시작되지 않음 · PSV를 재시작하세요"};
    switch (state) {
    case VCO_EXECUTION_READY:
        return ready[lang];
    case VCO_EXECUTION_BACKGROUND:
        return background[lang];
    case VCO_EXECUTION_NO_GAME:
        return no_game[lang];
    case VCO_EXECUTION_CONFLICT:
        return conflict[lang];
    case VCO_EXECUTION_ERROR:
        return error[lang];
    case VCO_EXECUTION_UNAVAILABLE:
        return unavailable[lang];
    default:
        return preparing[lang];
    }
}
const char *vco_execution_error_text(uint32_t error, int language)
{
    static const char *const text[VCO_LANGUAGE_COUNT][15] = {
        {"", "Invalid request", "Resource limit", "Invalid text encoding", "Invalid file syntax",
         "Unsupported code format", "Execution capability unavailable or unverified",
         "Address unavailable", "Memory or input operation failed", "Game session changed",
         "Execution budget exceeded", "Patch conflict or injection failed",
         "Unable to restore original instructions",
         "Cheat file game/version mismatch or conflicting metadata", "Execution failed"},
        {"", "请求无效", "超出资源限制", "文字编码错误", "文件语法错误", "不支持的代码格式",
         "执行能力不可用或尚未确认支持", "目标地址不可用", "内存或输入操作失败", "游戏会话已改变",
         "超出单次执行预算", "补丁冲突或注入失败", "无法恢复原始指令",
         "金手指文件的游戏/版本不匹配或声明有冲突", "执行失败"},
        {"", "請求無效", "超出資源限制", "文字編碼錯誤", "檔案語法錯誤", "不支援的代碼格式",
         "執行能力無法使用或尚未確認支援", "目標位址無法使用", "記憶體或輸入操作失敗",
         "遊戲工作階段已改變", "超出單次執行預算", "修補衝突或注入失敗", "無法還原原始指令",
         "金手指檔案的遊戲/版本不符或宣告有衝突", "執行失敗"},
        {"", "無効な要求", "リソース上限を超えています", "文字コードエラー", "ファイル構文エラー",
         "未対応のコード形式", "実行機能が利用不可または未確認", "対象アドレスが利用できません",
         "メモリまたは入力操作に失敗", "ゲームセッションが変わりました",
         "実行処理の上限を超えました", "パッチ競合または適用失敗", "元の命令を復元できません",
         "チートファイルのゲーム／バージョン不一致または情報競合", "実行に失敗しました"},
        {"", "잘못된 요청", "리소스 한도 초과", "인코딩 오류", "파일 구문 오류",
         "지원하지 않는 코드 형식", "실행 기능을 사용할 수 없거나 확인되지 않음",
         "대상 주소를 사용할 수 없음", "메모리 또는 입력 작업 실패", "게임 세션이 변경됨",
         "실행 처리 한도 초과", "패치 충돌 또는 적용 실패", "원래 명령어를 복원할 수 없음",
         "치트 파일의 게임/버전 불일치 또는 정보 충돌", "실행 실패"}};
    return text[language_index(language)][error <= VCO_E_METADATA ? error : 14];
}
const char *vco_execution_notice_text(VcoExecutionNotice notice, int language)
{
    static const char *const text[VCO_LANGUAGE_COUNT][4] = {
        {"", "Request queue busy · try again", "This entry cannot be changed now",
         "Reloading · all entries will start OFF"},
        {"", "请求队列繁忙 · 请稍后重试", "当前无法切换此条目", "正在重新加载 · 所有条目将关闭"},
        {"", "請求佇列忙碌 · 請稍後重試", "目前無法切換此項目", "正在重新載入 · 所有項目將關閉"},
        {"", "要求が混雑中 · 再試行してください", "この項目は現在変更できません",
         "再読込中 · 全項目をオフにします"},
        {"", "요청 대기열이 사용 중 · 다시 시도하세요", "현재 이 항목을 변경할 수 없습니다",
         "다시 불러오는 중 · 모든 항목이 꺼집니다"}};
    return text[language_index(language)]
               [(unsigned)notice <= VCO_NOTICE_RELOADING ? notice : VCO_NOTICE_UNAVAILABLE];
}
static size_t append_header(char *out, size_t capacity, size_t used, const char *text, size_t limit)
{
    for (size_t i = 0; i < limit && text[i] && used + 1 < capacity; ++i)
        out[used++] = text[i];
    out[used] = 0;
    return used;
}
void vco_catalog_identity_text(const VcoCatalog *catalog, char *out, size_t capacity)
{
    if (!out || !capacity)
        return;
    out[0] = 0;
    if (!catalog || !catalog->game.title_id[0])
        return;
    size_t used =
        append_header(out, capacity, 0, catalog->game.title_id, sizeof(catalog->game.title_id));
    if (catalog->game.version[0]) {
        used = append_header(out, capacity, used, " · v", sizeof(" · v") - 1);
        append_header(out, capacity, used, catalog->game.version, sizeof(catalog->game.version));
    }
}
const char *vco_catalog_compatibility_text(VcoCompatibility compatibility, int language)
{
    static const char *const text[VCO_LANGUAGE_COUNT][4] = {
        {"Version unverified", "Declared version matches", "Game/version mismatch",
         "Conflicting file metadata"},
        {"版本未核实", "版本声明匹配", "游戏或版本不匹配", "文件声明有冲突"},
        {"版本未核實", "版本宣告相符", "遊戲或版本不符", "檔案宣告有衝突"},
        {"バージョン未確認", "指定バージョンと一致", "ゲーム／バージョン不一致",
         "ファイル情報が競合"},
        {"버전 미확인", "지정 버전과 일치", "게임/버전 불일치", "파일 정보 충돌"}};
    return text[language_index(language)][(unsigned)compatibility <= VCO_COMPAT_CONFLICT
                                              ? compatibility
                                              : VCO_COMPAT_UNVERIFIED];
}
const char *vco_catalog_source_text(VcoSourceCheckState state, int language)
{
    static const char *const checking[] = {"Checking cheat file…", "正在核对金手指文件…",
                                           "正在核對金手指檔案…", "チートファイルを確認中…",
                                           "치트 파일 확인 중…"};
    static const char *const changed[] = {
        "File changed · Settings → Reload cheat list", "文件已变更 · 请在设置中重新加载列表",
        "檔案已變更 · 請在設定中重新載入清單", "ファイル変更あり · 設定から一覧を再読込",
        "파일 변경됨 · 설정에서 목록을 다시 불러오세요"};
    static const char *const unavailable[] = {
        "File check failed · loaded list kept · reload to retry",
        "文件核对失败 · 已保留当前列表 · 可重新加载重试",
        "檔案核對失敗 · 已保留目前清單 · 可重新載入重試",
        "ファイル確認失敗 · 現在の一覧を保持 · 再読込で再試行",
        "파일 확인 실패 · 현재 목록 유지 · 다시 불러오기로 재시도"};
    unsigned lang = language_index(language);
    if (state == VCO_SOURCE_CHECKING)
        return checking[lang];
    if (state == VCO_SOURCE_CHANGED)
        return changed[lang];
    if (state == VCO_SOURCE_UNAVAILABLE)
        return unavailable[lang];
    return "";
}
