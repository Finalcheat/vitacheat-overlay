#ifndef VCO_SETTINGS_TEXT_H
#define VCO_SETTINGS_TEXT_H
#include "../localization.h"
#include "layout.h"
/* UI-only immutable text. The field layout keeps every locale complete. */
struct VcoSettingsText {
    const char *heading;
    const char *labels[8];
    const char *prompts[3];
    const char *navigation;
    const char *unsaved;
    const char *pair_hint;
    const char *binding_help;
    const char *capture_footer;
    const char *capture_shortcut;
    const char *capture_single;
    const char *apply_neutral;
    const char *reload_help;
    const char *encoding_help;
    const char *encoding_source;
    const char *opacity_help;
    const char *key_count;
    const char *button_modes[2];
    const char *system;
    const char *automatic;
    const char *preview;
    const char *encoding_error;
};
static inline const VcoSettingsText &vco_settings_text(int language)
{
    static const VcoSettingsText text[VCO_LANGUAGE_COUNT] = {
        {
            /* English */
            "SETTINGS",
            {"Language", "Panel opacity", "Confirm / back", "Open / close shortcut",
             "File encoding", "Apply settings", "Restore default settings",
             "Reload cheat list (all entries OFF)"},
            {"Apply and return", "Discard and return", "Continue editing"},
            "↑↓ Select   ←→ Adjust   %s Confirm   %s Back",
            "Changes not applied",
            "Apply Confirm/Back · □ still toggles the list",
            "%s Change · %s",
            "Release controls · press shortcut · release to confirm · ↓ Cancel",
            "Shortcut requires at least two buttons held together",
            "Press one button · ↑ and ↓ stay reserved for navigation",
            "Release all buttons and sticks to apply",
            "Reloading stops all entries and restores instruction patches",
            "Apply, then reload · Auto tries UTF-8 before GBK",
            "%s · Current %s · Apply, then reload",
            "Use ←→ · 5% steps · preview on light / dark",
            "%u keys",
            {"○ Confirm / × Back", "× Confirm / ○ Back"},
            "System",
            "Auto",
            "Aa",
            "Encoding error: check settings, then reload",
        },
        {
            /* 简体中文 */
            "设置",
            {"语言", "面板不透明度", "确认 / 返回", "唤起 / 关闭快捷键", "文件编码", "应用设置",
             "恢复默认设置", "重新加载列表（关闭全部条目）"},
            {"应用并返回", "放弃修改并返回", "继续编辑"},
            "↑↓ 选择   ←→ 调整   %s 确认   %s 返回",
            "修改尚未应用",
            "应用确认／返回习惯 · 主列表仍用□切换",
            "%s 改键 · %s",
            "释放全部输入 · 按下新键 · 释放确认 · ↓ 取消",
            "快捷键须同时按住至少两个按键",
            "请按一个按键 · ↑ 和 ↓ 保留用于导航",
            "释放按键和摇杆后应用设置",
            "重新加载会关闭全部条目并恢复指令补丁",
            "应用后重新加载生效 · 自动优先 UTF-8，再尝试 GBK",
            "%s · 当前 %s · 应用后重新加载",
            "按 ←→ 调整 · 每格 5% · 明暗底色局部预览",
            "%u 键",
            {"○ 确认 / × 返回", "× 确认 / ○ 返回"},
            "跟随系统",
            "自动",
            "预览",
            "编码错误：检查文件编码设置后重新加载",
        },
        {
            /* 繁體中文 */
            "設定",
            {"語言", "面板不透明度", "確認 / 返回", "喚起 / 關閉快捷鍵", "檔案編碼", "套用設定",
             "恢復預設設定", "重新載入清單（關閉全部項目）"},
            {"套用並返回", "放棄修改並返回", "繼續編輯"},
            "↑↓ 選擇   ←→ 調整   %s 確認   %s 返回",
            "修改尚未套用",
            "套用確認／返回習慣 · 主清單仍用□切換",
            "%s 改鍵 · %s",
            "釋放全部輸入 · 按下新鍵 · 釋放確認 · ↓ 取消",
            "快捷鍵須同時按住至少兩個按鍵",
            "請按一個按鍵 · ↑ 和 ↓ 保留用於導覽",
            "釋放按鍵與搖桿後套用設定",
            "重新載入會關閉全部項目並還原指令修補",
            "套用後重新載入生效 · 自動優先 UTF-8，再嘗試 GBK",
            "%s · 目前 %s · 套用後重新載入",
            "按 ←→ 調整 · 每格 5% · 明暗底色局部預覽",
            "%u 鍵",
            {"○ 確認 / × 返回", "× 確認 / ○ 返回"},
            "跟隨系統",
            "自動",
            "預覽",
            "編碼錯誤：檢查檔案編碼設定後重新載入",
        },
        {
            /* 日本語 */
            "設定",
            {"言語", "パネル不透明度", "決定 / 戻る", "表示 / 閉じるキー", "文字コード",
             "設定を適用", "初期設定に戻す", "チート一覧を再読込（すべてオフ）"},
            {"適用して戻る", "変更を破棄して戻る", "編集を続ける"},
            "↑↓ 選択   ←→ 調整   %s 決定   %s 戻る",
            "変更は未適用です",
            "決定／戻るを適用 · 一覧の切替は□",
            "%s 変更 · %s",
            "キーを放す · 組合せを押す · 放して決定 · ↓ キャンセル",
            "2つ以上のキーを同時に押してください",
            "キーを1つ押してください · ↑↓は選択用です",
            "すべてのキーとスティックを放して適用",
            "再読込で全項目を停止し、命令パッチを復元します",
            "適用後に再読込 · 自動はUTF-8、次にGBKを試します",
            "%s · 現在 %s · 適用後に再読込",
            "←→で調整 · 5%刻み · 明暗の背景でプレビュー",
            "%u キー",
            {"○ 決定 / × 戻る", "× 決定 / ○ 戻る"},
            "システム",
            "自動",
            "表示例",
            "文字コードエラー：設定を確認して再読込",
        },
        {
            /* 한국어 */
            "설정",
            {"언어", "패널 불투명도", "확인 / 뒤로", "열기 / 닫기 단축키", "파일 인코딩",
             "설정 적용", "기본 설정 복원", "치트 목록 다시 불러오기（모두 끄기）"},
            {"적용 후 돌아가기", "변경 취소 후 돌아가기", "계속 편집"},
            "↑↓ 선택   ←→ 조절   %s 확인   %s 뒤로",
            "변경 사항이 적용되지 않았습니다",
            "확인/뒤로 적용 · 목록 전환은 □",
            "%s 변경 · %s",
            "버튼 놓기 · 단축키 누르기 · 놓아서 확인 · ↓ 취소",
            "두 개 이상의 버튼을 동시에 누르세요",
            "버튼 하나를 누르세요 · ↑↓는 선택용입니다",
            "모든 버튼과 스틱을 놓으면 적용됩니다",
            "다시 불러오면 모든 항목을 끄고 명령어 패치를 복원합니다",
            "적용 후 다시 불러오기 · 자동은 UTF-8, 다음으로 GBK",
            "%s · 현재 %s · 적용 후 다시 불러오기",
            "←→로 조절 · 5% 단위 · 밝고 어두운 배경 미리보기",
            "%u개 버튼",
            {"○ 확인 / × 뒤로", "× 확인 / ○ 뒤로"},
            "시스템",
            "자동",
            "미리보기",
            "인코딩 오류: 설정 확인 후 다시 불러오세요",
        },
    };
    return text[vco_language_normalize(language)];
}
#endif
