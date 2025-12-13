#ifndef MIDORI_APP_H
#define MIDORI_APP_H

#include <expected>
#include <filesystem>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <optional>

class App {
  public:
    enum class Result : uint8_t {
        Success,
        MissingFile,
        Unknown,
    };

    enum class Axis {
        Scroll,
        Pressure,
        Orientation,
        Tilt,
        Roll,
    };

    enum class Motion {
        Cursor,
    };

    enum class Key : uint16_t {
        None,
    };

    enum class Mod : uint16_t {
        None,
    };

    enum class Button : uint8_t {
        None,
    };

    struct Preferences {
        // Last session settings
        struct Session {
            bool     windowFullscreened = false;
            bool     windowMaximized    = false;
            uint16_t windowX            = 0;
            uint16_t windowY            = 0;
            uint16_t windowWidth        = 0;
            uint16_t windowHeight       = 0;
        };

        Session session;

        // GLobal Settings
        bool openLastSession = false;

        std::vector<std::filesystem::path> recentFiles; // FIXME: Simplify for serialisation
    };

    enum class BlendMode : uint32_t {
        AlphaBlend,
        Additive,
        Mutliply,
    };

    struct Tile {
        uint16_t id;
        auto     operator<=>(const Tile&) const = default; // default initializer for comparaison // C++20
    };

    struct Layer {
        uint16_t id;
        auto     operator<=>(const Layer&) const = default;
    };

    App(const App&)            = delete;
    App(App&&)                 = delete;
    App& operator=(const App&) = delete;
    App& operator=(App&&)      = delete;

    App()  = default;
    ~App() = default;

    Result Init(const std::vector<std::string>& args);
    Result Terminate();

    Result OnWindowRender(float deltaTime);
    void   OnWindowMove(int x, int y);
    void   OnWindowResize(int width, int height);

    void OnKeyPress(Key key, Mod mods);
    void OnKeyRelease(Key key, Mod mods);
    void OnButtonPress(Button btn);
    void OnButtonRelease(Button btn);
    void OnCursorEnter();
    void OnCursorExit();
    void OnCursorPressure(float pressure);
    void OnCursorMove(float x, float y);

    [[nodiscard]] Preferences GetPrefs() const;
    void                      SetPrefs(Preferences prefs);

  private:
    void   ParseArgs(const std::vector<std::string>& args);
    Result PrefsSave();
    Result PrefsLoad();

    Result NewFile();
    Result SaveFile();
    Result OpenFile(const std::filesystem::path& filename);
    Result SaveFileAs(const std::filesystem::path& filename);

  private:
    Preferences prefs_ = {};

    std::optional<std::filesystem::path> filePath_ = std::nullopt;
};

namespace std {
template <>
struct hash<App::Tile> {
    std::size_t operator()(const App::Tile& tile) const noexcept {
        return std::hash<uint32_t>{}(tile.id);
    }
};
template <>
struct hash<App::Layer> {
    std::size_t operator()(const App::Layer& layer) const noexcept {
        return std::hash<uint32_t>{}(layer.id);
    }
};
} // namespace std

/*
namespace Midori {

class AppT {
  public:
    App(const App&)            = delete;
    App(App&&)                 = delete;
    App& operator=(const App&) = delete;
    App& operator=(App&&)      = delete;

    App(int argc, char* argv[]);
    ~App() = default;

    bool Init();
    bool Update();
    bool Resize(int width, int height);
    void Quit();

    void CursorMove(glm::vec2 new_pos);
    void CursorPress(Uint8 button);
    void CursorRelease(Uint8 button);
    void KeyPress(SDL_Keycode key, SDL_Keymod mods);
    void KeyRelease(SDL_Keycode key, SDL_Keymod mods);

    SDL_Window*              window;
    std::vector<std::string> args;

    Canvas   canvas;
    Renderer renderer;

    glm::vec4  bg_color    = {1.0F, 1.0F, 1.0F, 1.0F};
    glm::ivec2 window_pos  = {SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED};
    glm::ivec2 window_size = {1280, 720};

    glm::vec2 cursor_last_pos      = glm::vec2(0.0f);
    glm::vec2 cursor_current_pos   = glm::vec2(0.0f);
    glm::vec2 cursor_delta_pos     = glm::vec2(0.0f);
    bool      cursor_left_pressed  = false;
    bool      cursor_right_pressed = false;

    bool  space_pressed = false;
    bool  shift_pressed = false;
    bool  ctrl_pressed  = false;
    bool  pen_in_range  = false;
    float pen_pressure  = 1.0f;
};
} // namespace Midori
*/

#endif
