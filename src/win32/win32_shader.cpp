#include "win32_shader.hpp"

#include "win32_platform.hpp" // <windows.h> with the project's defines

#include <d3dcompiler.h>

#include <cstdint>
#include <format>
#include <string>

namespace Midori {

namespace {

// FXC asks for each #include by name.
struct Include final : ID3DInclude {
    explicit Include(std::span<const Win32ShaderSource> includes) noexcept : includes(includes) {}

    HRESULT __stdcall Open(D3D_INCLUDE_TYPE /*type*/, LPCSTR name, LPCVOID /*parent*/, LPCVOID* data,
                           UINT* bytes) noexcept override {
        const std::string_view wanted(name);
        for (const Win32ShaderSource& source : includes) {
            if (source.name == wanted) {
                *data = source.text.data();
                *bytes = static_cast<UINT>(source.text.size());
                return S_OK;
            }
        }
        return E_FAIL;
    }

    HRESULT __stdcall Close(LPCVOID /*data*/) noexcept override { return S_OK; }

    std::span<const Win32ShaderSource> includes;
};

} // namespace

Win32Shader Win32ComputeShaderCompile(const Win32ShaderSource& source,
                                      std::span<const Win32ShaderSource> includes) noexcept {
    Win32Shader shader;
    Include include(includes);
    const D3D_SHADER_MACRO defines[]{{.Name = "MIDORI_RUNTIME", .Definition = "1"},
                                     {.Name = nullptr, .Definition = nullptr}};
    const std::string name(source.name);
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(source.text.data(), source.text.size(), name.c_str(), defines, &include, "CSMain",
                                  "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors != nullptr) {
        shader.log = static_cast<const char*>(errors->GetBufferPointer());
        errors->Release();
    }
    if (SUCCEEDED(hr) && code != nullptr) {
        const auto* bytes = static_cast<const std::byte*>(code->GetBufferPointer());
        shader.code.assign(bytes, bytes + code->GetBufferSize());
    } else if (shader.log.empty()) {
        shader.log = std::format("D3DCompile failed: 0x{:08X}", static_cast<std::uint32_t>(hr));
    }
    if (code != nullptr) {
        code->Release();
    }
    return shader;
}

} // namespace Midori
