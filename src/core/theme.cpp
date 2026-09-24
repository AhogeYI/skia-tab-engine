#include "tabengine/theme.h"

namespace tabengine {

Theme Theme::light() {
    Theme theme;
    theme.strip = 0xFFE2ECFD;
    theme.body = 0xFFFFFFFF;
    theme.tab_active = 0xFFFFFFFF;
    theme.tab_inactive = 0xFFE9F1FF;
    theme.tab_hover = 0xFFF0F6FF;
    theme.text = 0xFF2B3656;
    theme.text_muted = 0xFF5E6B8C;
    theme.tab_close = 0xFF5E6B8C;
    theme.separator = 0x332B3656;
    theme.new_tab = 0xFFDCE9FB;
    theme.new_tab_hover = 0xFFCFE0F8;
    theme.hover_card = 0xFFF2F7FF;
    theme.hover_card_border = 0xFFBFCEE5;
    theme.caption_hover = 0xFFCFE0F8;
    theme.caption_close_hover = 0xFFE81123;
    return theme;
}

} // namespace tabengine
