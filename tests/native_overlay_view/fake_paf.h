#ifndef VCO_TEST_OVERLAY_PAF_H
#define VCO_TEST_OVERLAY_PAF_H
#include <cstdio>
#include <cstring>
#include <memory>
#include <map>
#include <string>
#include <vector>
#include "../../src/ui/resource_ids.h"
#define sceClibSnprintf std::snprintf
#define sceClibStrnlen strnlen
namespace paf
{
struct IDParam;
namespace math
{
struct v2 {
    float x = 0, y = 0;
    float extract_x() const
    {
        return x;
    }
};
struct v4 {
    float x, y, z, w;
    v4(float a = 0, float b = 0, float c = 0, float d = 0) : x(a), y(b), z(c), w(d) {}
};
} // namespace math
/* Do not use the host's std::wstring ABI with -fshort-wchar. Production label()
 * supplies UTF-16; this boundary copies those code units into a native string. */
struct wstring {
    std::u16string value;
    explicit wstring(const wchar_t *text)
    {
        while (*text)
            value.push_back((char16_t)*text++);
    }
};
namespace graph
{
enum {
    TextLayoutAttribute_WordWrap,
    TextLayoutAttribute_Ellipsis,
    TextLayoutAttribute_AlignX,
    TextLayoutAlignX_Left,
    TextLayoutAlignX_Center,
    TextLayoutAlignX_Right
};
} // namespace graph
namespace ui
{
struct Widget {
    static unsigned lookups, fail_lookup;
    math::v4 size, pos, color;
    std::u16string value;
    std::string style, texture_id;
    std::map<std::string, Widget *> children;
    virtual ~Widget() {}
    void SetSize(const math::v4 &v)
    {
        size = v;
    }
    void SetPos(const math::v4 &v)
    {
        pos = v;
    }
    void SetColor(const math::v4 &v)
    {
        color = v;
    }
    Widget *FindChild(const IDParam &id, int = 0);
    Widget *find(const std::string &name)
    {
        auto direct = children.find(name);
        if (direct != children.end())
            return direct->second;
        for (auto &child : children)
            if (Widget *found = child.second->find(name))
                return found;
        return NULL;
    }
};
struct Text : Widget {
    float font_size = 0;
    bool bold = true;
    bool wrap = true, ellipsis = false;
    int align = graph::TextLayoutAlignX_Left;
    int bounds_result = 0;
    float measured_width = -1;
    int GetBounds(math::v2 &out)
    {
        float width = measured_width;
        if (width < 0) {
            width = 0;
            for (char16_t character : value)
                width += character < 128 ? font_size / 2 : font_size;
        }
        out.x = width;
        return bounds_result;
    }
    void SetLayoutAttribute(int attr, int value)
    {
        if (attr == graph::TextLayoutAttribute_WordWrap)
            wrap = value;
        if (attr == graph::TextLayoutAttribute_Ellipsis)
            ellipsis = value;
        if (attr == graph::TextLayoutAttribute_AlignX)
            align = value;
    }
};
struct Scene : Widget {};
} // namespace ui
#pragma pack(push, 4)
struct IDParam {
    const char *name;
    char padding[16 - sizeof(const char *)];
    explicit IDParam(const char *id) : name(id), padding{} {}
};
#pragma pack(pop)
inline ui::Widget *ui::Widget::FindChild(const IDParam &id, int)
{
    if (++lookups == fail_lookup)
        return NULL;
    return find(id.name);
}
struct Plugin {
#pragma pack(push, 4)
    struct InitParam {
        const char *name, *caller_name, *resource_file;
        void (*start_func)(Plugin *);
        char padding[0x94 - 3 * sizeof(const char *) - sizeof(void (*)(Plugin *))];
    };
    struct PageOpenParam {
        int overwrite_draw_priority;
        bool fade;
        char padding[0x2c - sizeof(int) - sizeof(bool)];
    };
#pragma pack(pop)
    struct PageCloseParam {
        bool fade;
    };
    struct TemplateOpenParam {
        void *event_handler_table = NULL;
        ui::Widget *sibling = NULL;
    };
    static unsigned creations, fail_on;
    static bool template_failure;
    std::vector<std::unique_ptr<ui::Widget>> widgets;
    ui::Scene scene;
    static void LoadSync(const InitParam &init)
    {
        static Plugin instance;
        init.start_func(&instance);
    }
    ui::Scene *PageOpen(const IDParam &, const PageOpenParam &)
    {
        creations = 0;
        ui::Widget::lookups = 0;
        return &scene;
    }
    void PageClose(const IDParam &, const PageCloseParam &)
    {
        scene.children.clear();
        widgets.clear();
    }
    ui::Widget *CreateWidget(ui::Widget *parent, const char *type, const IDParam &id,
                             const IDParam &style)
    {
        if (++creations == fail_on)
            return NULL;
        std::unique_ptr<ui::Widget> widget(
            !std::strcmp(type, "text") ? static_cast<ui::Widget *>(new ui::Text) : new ui::Widget);
        ui::Widget *out = widget.get();
        out->style = style.name;
        if (!std::strcmp(type, "text")) {
            ui::Text *text = static_cast<ui::Text *>(out);
            text->font_size = !std::strcmp(style.name, VCO_HEADER_STYLE)           ? 18
                              : !std::strcmp(style.name, VCO_INDEX_STYLE)          ? 16
                              : !std::strcmp(style.name, "psvs_style_text_impose") ? 24
                                                                                   : 19;
            text->bold = !std::strcmp(style.name, "psvs_style_text_impose");
        }
        parent->children[id.name] = out;
        widgets.push_back(std::move(widget));
        return out;
    }
    int TemplateOpen(ui::Widget *parent, const IDParam &id, const TemplateOpenParam &)
    {
        // Models only native resource/factory ownership boundaries. RCO bytes
        // and PNG payloads have independent Python/decompiler validation.
        if (template_failure || std::strcmp(id.name, VCO_SWITCH_TEMPLATE))
            return -1;
        ui::Widget *group = CreateWidget(parent, "group", IDParam(VCO_SWITCH_GROUP), IDParam(""));
        if (!group)
            return -1;
        const char *textures[] = {"vco_texture_track_on", "vco_texture_track_off",
                                  "vco_texture_thumb"};
        for (unsigned row = 0; row < VCO_RESOURCE_SWITCH_ROWS; ++row)
            for (unsigned part = 0; part < 3; ++part) {
                ui::Widget *plane = CreateWidget(group, "plane", IDParam(vco_switch_ids[row][part]),
                                                 IDParam("psvs_style_plane_icon"));
                if (!plane)
                    return -1;
                plane->texture_id = textures[part];
            }
        return 0;
    }
};
} // namespace paf
#endif
