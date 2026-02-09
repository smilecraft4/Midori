#pragma once

#include <cstdint>
#include <string_view>
#include <vector>
#include <string>

#include <ft2build.h>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include FT_FREETYPE_H
#include <hb.h>

// Needed for this to work

namespace Midori {
class App;

class UI {
   public:
    enum class Alignment : uint8_t {
        TopLeft,
        TopCenter,
        TopRight,
        MiddleLeft,
        MiddleCenter,
        MiddleRight,
        BottomLeft,
        BottomCenter,
        BottomRight,
    };

    enum class Direction : uint8_t {
        Freeform,
        Vertical,
        Horizontal,
    };

    enum class Anchor : uint8_t {
        Left = 0x01,
        Center,
        Right,
        LeftRight,

        Top = 0x10,
        Middle,
        Bottom,
        TopBottom,
    };

    using ColorStyleID = int;
    struct ColorStyle {
        glm::vec4 color{0.0f, 0.0f, 0.0f, 1.0f};
    };

    using StrokeStyleID = int;
    struct StrokeStyle {
        ColorStyleID colorStyleID{};
        int8_t strokeRadius{};
        int8_t strokeDash{};
        int8_t strokeGap{};
    };

    struct ContainerInfo {
        glm::ivec2 pos{};
        glm::ivec2 size{};
        glm::i8vec4 corners{};
        glm::i8vec4 padding{};
        int8_t spacing{};
        Direction direction{};
        Alignment alignment{};
        Anchor anchor{};
    };

    struct StateInfo {
        bool toggled{};
        bool hover{};
        float hoverTime{};
    };

    enum class TextWeight : uint8_t {
        Regular,
        Italic,
        Bold,
        BoldItalic,
    };

    using TextStyleID = int;
    struct TextStyle {
        std::string font{"Arial"};
        int size{16};  // pixel
        TextWeight weight{};
    };

    enum class TextureScale : uint8_t {
        Fit,
        Fill,
        Crop,
    };

    using TextureStyleID = int;
    struct TextureStyle {
        TextureScale scaling;
    };

    explicit UI(App& app);

    void Init();
    void Quit();

    TextStyleID CreateTextStyle(const TextStyle& style);
    void DestroyTextStyle(TextStyleID id);

    void Text(std::string text, TextStyleID id, glm::ivec2 size, glm::ivec2 position, Alignment alignment = Alignment::TopLeft);

    // void Start(glm::ivec2 pos, glm::ivec2 size);
    // void End();  // Construct UI RenderGraph

    // void Render();

    // TextStyleID CreateTextStyle(TextStyle textStyle);
    // ColorStyleID CreateColorStyle(ColorStyle colorStyle);
    // StrokeStyleID CreateStrokeStyle(StrokeStyle strokeStyle);
    // TextureStyleID CreateTextureStyle(TextureStyle textureStyle);

    // void DestroyTextStyle(TextStyleID textStyleID);
    // void DestroyColorStyle(ColorStyleID colorStyleID);
    // void DestroyStrokeStyle(StrokeStyleID strokeStyleID);
    // void DestroyTextureStyle(TextureStyleID textureStyleID);

    // using ContainerID = int;
    // ContainerID StartContainer(const ContainerInfo& info, ColorStyleID colorStyleID = 0,
    //    StrokeStyleID strokeStyleID = 0);
    // void EndContainer();
    // bool IsOverContainer(ContainerID containerID, glm::ivec2 position) const;

    // void Text(std::string text, Alignment alignment, TextStyleID textStyleID = 0);
    // void Texture(void* textureID, TextureStyleID textureStyleID = 0);

    std::vector<ColorStyleID> containersColorStyles;
    std::vector<StrokeStyleID> containersStrokeStyles;
    std::vector<ContainerInfo> containersContainerInfos;
    std::vector<StateInfo> containersStateInfos;

   private:
    bool HasGlyph(TextStyleID textId, uint32_t glyph) const;
    void AddGlyph(TextStyleID textId, uint32_t glyph);
    void RemoveGlyph(TextStyleID textId, uint32_t glyph);

    struct TextStyleInfo {
        std::string fontname;
        TextWeight weight;
        int size;
        FT_Face ftFace;
        hb_font_t* font;
        std::unordered_map<uint32_t, int> glyphsTexAtlasIndex;
        std::unordered_map<uint32_t, glm::vec4> glyphsTexAtlasCoords;
    };
    std::unordered_map<TextStyleID, TextStyleInfo> textStyleInfo;

    App& app;
    FT_Library ftLib;
    FT_Face ftFace;
};

/*
class Demo {
   public:
    Demo() {
        colorStylePrimary = ui.CreateColorStyle(UI::ColorStyle{
            .color = {248.0f / 255.0f, 248.0f / 255.0f, 248.0f / 255.0f, 1.0f},
        });
        colorStyleSecondary = ui.CreateColorStyle(UI::ColorStyle{
            .color = {237.0f / 255.0f, 237.0f / 255.0f, 237.0f / 255.0f, 1.0f},
        });
        colorStyleHighlight = ui.CreateColorStyle(UI::ColorStyle{
            .color = {132.0f / 255.0f, 204.0f / 255.0f, 255.0f / 255.0f, 1.0f},
        });

        strokeStyle = ui.CreateStrokeStyle(UI::StrokeStyle{
            .colorStyleID = colorStyleSecondary,
            .strokeRadius = 1,
        });

        textStyle = ui.CreateTextStyle(UI::TextStyle{
            .font = "Segoeui",
            .size = 10,
            .weight = UI::TextWeight::Regular,
        });
        iconStyle = ui.CreateTextStyle(UI::TextStyle{
            .font = "Segoe Fluent Icons",
            .size = 10,
            .weight = UI::TextWeight::Regular,
        });
    }

    ~Demo() {
        ui.~UI();  // takes care of everything
    }

    void Update() {
        ui.Start({0, 0}, windowSize);
        if (Button("Demo button")) {
            // Do something on press
        }
        ui.End();
    }

    void Render() {
        ui.Render();  // either to this or read the ui layout to know how to render it
    }

   private:
    glm::ivec2 windowPos{};
    glm::ivec2 windowSize{};
    glm::vec2 cursorPos{};
    bool leftButton{};

    UI ui{};
    UI::ColorStyleID colorStylePrimary{};
    UI::ColorStyleID colorStyleSecondary{};
    UI::ColorStyleID colorStyleHighlight{};
    UI::StrokeStyleID strokeStyle{};
    UI::TextStyleID textStyle{};
    UI::TextStyleID iconStyle{};

    bool Button(std::string text) {
        const auto id = ui.StartContainer(
            UI::ContainerInfo{
                .pos = {10, 10},
                .size = {-1, 18},
                .corners = {2, 2, 2, 2},
                .padding = {10, 2, 10, 2},
                .direction = UI::Direction::Horizontal,
                .alignment = UI::Alignment::MiddleCenter,
            },
            colorStylePrimary, strokeStyle);

        ui.Text(text, UI::Alignment::MiddleCenter, textStyle);

        ui.EndContainer();

        if (ui.IsOverContainer(id, cursorPos)) {
        }

        return false;
    }
};
*/

}  // namespace Midori
