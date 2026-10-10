#ifndef VCO_LOCALIZATION_H
#define VCO_LOCALIZATION_H
/* Stable persisted IDs. Append languages; never reorder existing IDs. */
typedef enum {
    VCO_LANGUAGE_ENGLISH,
    VCO_LANGUAGE_CHINESE_S,
    VCO_LANGUAGE_CHINESE_T,
    VCO_LANGUAGE_JAPANESE,
    VCO_LANGUAGE_KOREAN,
    VCO_LANGUAGE_COUNT
} VcoLanguage;
static inline int vco_language_normalize(int language)
{
    return language >= 0 && language < VCO_LANGUAGE_COUNT ? language : VCO_LANGUAGE_ENGLISH;
}
#endif
