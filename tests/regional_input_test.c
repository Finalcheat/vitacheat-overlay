#include "input_state.h"
#include <assert.h>
#include <string.h>
int main(int argc, char **argv)
{
    assert(argc == 2);
    VcoSettings mapping = vco_settings_defaults();
    bool western = !strcmp(argv[1], "western");
    vco_settings_set_button_mode(&mapping, western ? VCO_BUTTON_WESTERN : VCO_BUTTON_ASIAN);
    assert(vco_settings_valid(&mapping));
    assert(mapping.bindings.toggle == VCO_SQUARE);
    const uint32_t confirm = vco_settings_confirm_button(&mapping);
    assert(vco_input_button_action(VCO_SQUARE, false, &mapping) == VCO_TOGGLE);
    assert(vco_input_button_action(confirm, false, &mapping) == VCO_NONE);
    assert(vco_input_button_action(VCO_SQUARE, true, &mapping) == VCO_NONE);
    assert(vco_input_button_action(confirm, true, &mapping) == VCO_TOGGLE);
    for (unsigned settings = 0; settings < 2; ++settings) {
        assert(vco_input_button_action(mapping.bindings.close, settings, &mapping) ==
               (settings ? VCO_BACK : VCO_CLOSE));
    }
    return 0;
}
