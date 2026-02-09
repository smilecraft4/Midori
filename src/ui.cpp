#include "ui.h"

#include <SDL3/SDL_Log.h>
#include <SDL3/SDL_assert.h>
#include <hb-ft.h>
#include <utf8.h>

#include "app.h"

namespace Midori {
UI::UI(App &app) : app(app) {}

void UI::Init() {
    /*
    FT_Error ftErr{};

    ftErr = FT_Init_FreeType(&ftLib);
    if(ftErr != FT_Err_Ok) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed initialize Freetype");
    }

    ftErr = FT_New_Face(ftLib, "C:/Windows/Fonts/Segoeui.ttf", 0, &ftFace);
    ftErr = FT_Set_Pixel_Sizes(ftFace, 0, 16);

    std::string text = "Random utf8 text 漢字";

    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, text.c_str(), -1, 0, -1);
    hb_buffer_set_direction(buf, HB_DIRECTION_RTL);
    hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
    hb_buffer_set_language(buf, hb_language_from_string("en", -1));

    hb_font_t *font = hb_ft_font_create_referenced(ftFace);
    hb_ft_font_set_funcs(font);

    hb_shape(font, buf, nullptr, 0);
    unsigned int glyphCount{};
    // hb_glyph_info_t *glyphInfo = hb_buffer_get_glyph_infos(buf, &glyphCount);
    hb_glyph_position_t *glyphPos = hb_buffer_get_glyph_positions(buf, &glyphCount);

    glm::ivec2 size{};

    for(unsigned int i = 0; i < glyphCount; i++) {
          size.x  += glyphPos[i].x_advance;
          size.y  += glyphPos[i].y_advance;
    }
    
    SDL_Log("Rect box is: (%d, %d)", size.x, size.y);

    hb_buffer_destroy(buf);
    hb_font_destroy(font);
    */
}

void UI::Quit() {
    /*
    FT_Error ftErr{};    
    ftErr = FT_Done_Face(ftFace);
    ftErr = FT_Done_FreeType(ftLib);
    if(ftErr != FT_Err_Ok) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to stop Freetype");
    }
    */
}

// bool UI::HasGlyph(TextStyleID textId, uint32_t glyph) const { 
//     SDL_assert(textId > 0 && "Invalid textId");
//     SDL_assert(textStyleInfo.contains(textId) && "textId not found");

//     // return textStyleInfo[textId].glyphsTexAtlasCoords.contains(glyph);
//     return false;
// }

// bool ReserveGlyphAtlasSlot(UI::TextStyleID textId, uint32_t glyph) {
//     const int atlasIndex{};
//     const glm::ivec4 coords{};
//     // textStyleInfo[textId].glyphsTexAtlasIndex[glyph] = atlasIndex;
//     // textStyleInfo[textId].glyphsTexAtlasCoords[glyph] = coords;

//     return true;
// }

// void UI::AddGlyph(TextStyleID textId, uint32_t glyph) {
//     if(!ReserveGlyphAtlasSlot(textId, glyph)) {
//         // Handle Error
//     }
    
//     FT_Load_Glyph(textStyleInfo[textId].ftFace, glyph, 0);
//     FT_Render_Glyph(textStyleInfo[textId].ftFace->glyph, FT_RENDER_MODE_NORMAL); /* render mode */

//     // app.renderer.UploadFontGlyph(textStyleInfo[textId].ftFace->glyph->bitmap, textStyleInfo[textId].glyphsTexAtlasCoords);
//     // Upload bitmap to the reseverd place on the glyph
// }
    
}  // namespace Midori
