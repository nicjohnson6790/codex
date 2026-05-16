#pragma once

#include "EngineRendererBase.hpp"
#include "Position.hpp"

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

class WorldTextRenderer : private EngineRendererBase
{
public:
    enum class HorizontalJustify
    {
        Left,
        Center,
        Right,
    };

    struct Style
    {
        float lineSpacing = 1.2f;
        glm::vec4 baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        glm::vec4 strokeColor{ 0.0f, 0.0f, 0.0f, 1.0f };
        float strokeWidth = 0.0f;
        glm::vec4 glowColor{ 1.0f, 1.0f, 1.0f, 0.0f };
        float glowWidth = 0.0f;
        glm::vec2 glowOffset{ 0.0f, 0.0f };
    };

    struct VertexUniforms
    {
        glm::mat4 viewProjection{ 1.0f };
    };

    struct FragmentUniforms
    {
        float distanceRange = 8.0f;
        float padding0 = 0.0f;
        float padding1 = 0.0f;
        float padding2 = 0.0f;
    };

    WorldTextRenderer() = default;
    ~WorldTextRenderer() = default;

    WorldTextRenderer(const WorldTextRenderer&) = delete;
    WorldTextRenderer& operator=(const WorldTextRenderer&) = delete;

    void initialize(
        SDL_GPUDevice* device,
        SDL_GPUTextureFormat colorFormat,
        SDL_GPUTextureFormat depthFormat,
        const std::filesystem::path& shaderDirectory
    );
    void shutdown();

    void clear();
    void setActiveCamera(const Position& cameraPosition, const glm::vec3& cameraForward, const glm::vec3& cameraUp);

    void addLineBaseline(
        const Position& baseline,
        std::string_view text,
        const Style& style = {});
    void addLineBaseline(
        const Position& baseline,
        const glm::vec3& baselineDirection,
        const glm::vec3& upDirection,
        std::string_view text,
        HorizontalJustify justify = HorizontalJustify::Left,
        const Style& style = {});
    void addLineCentered(
        const Position& center,
        std::string_view text,
        const Style& style = {});
    void addLineCentered(
        const Position& center,
        const glm::vec3& baselineDirection,
        const glm::vec3& upDirection,
        std::string_view text,
        const Style& style = {});
    void addMultilineBaseline(
        const Position& firstBaseline,
        std::string_view text,
        HorizontalJustify justify,
        const Style& style = {});
    void addMultilineBaseline(
        const Position& firstBaseline,
        const glm::vec3& baselineDirection,
        const glm::vec3& upDirection,
        std::string_view text,
        HorizontalJustify justify,
        const Style& style = {});
    void addMultilineCentered(
        const Position& center,
        std::string_view text,
        HorizontalJustify justify,
        const Style& style = {});
    void addMultilineCentered(
        const Position& center,
        const glm::vec3& baselineDirection,
        const glm::vec3& upDirection,
        std::string_view text,
        HorizontalJustify justify,
        const Style& style = {});

    void upload(SDL_GPUCopyPass* copyPass);
    void render(SDL_GPURenderPass* renderPass, SDL_GPUCommandBuffer* commandBuffer, const glm::mat4& viewProjection) const;

private:
    struct Glyph
    {
        bool valid = false;
        std::uint32_t codepoint = 0;
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 0.0f;
        float v1 = 0.0f;
        float planeLeft = 0.0f;
        float planeBottom = 0.0f;
        float planeRight = 0.0f;
        float planeTop = 0.0f;
        float advance = 0.0f;
    };

    struct QuadVertex
    {
        glm::vec2 corner;
    };

    struct GroupGpu
    {
        glm::vec4 origin;
        glm::vec4 right;
        glm::vec4 up;
        glm::vec4 drawBounds;
        glm::uvec4 glyphRangeAndStyle;
    };

    struct GlyphInstanceGpu
    {
        glm::vec4 offsetAndGlyph;
    };

    struct PendingGlyph
    {
        GlyphInstanceGpu instance;
        glm::vec4 drawBounds;
    };

    struct GlyphMetricGpu
    {
        glm::vec4 uvRect;
        glm::vec4 planeBounds;
    };

    struct StyleGpu
    {
        glm::vec4 baseColor;
        glm::vec4 strokeColor;
        glm::vec4 glowColor;
        glm::vec4 widths;
        glm::vec4 glowOffset;
    };

    struct BufferPair
    {
        SDL_GPUBuffer* buffer = nullptr;
        SDL_GPUTransferBuffer* transfer = nullptr;
        std::uint32_t capacity = 0;
        std::uint32_t elementSize = 0;
        SDL_GPUBufferUsageFlags usage = 0;
        const char* name = "";
    };

    void createPipeline(const std::filesystem::path& shaderDirectory);
    void loadFontAssets();
    void createSampler();
    void createQuadBuffers();
    void destroyFontResources();
    void destroyBufferPair(BufferPair* pair);
    void ensureBufferPairCapacity(BufferPair* pair, std::uint32_t requiredElementCount);
    void uploadBufferPair(SDL_GPUCopyPass* copyPass, BufferPair* pair, const void* data, std::uint32_t elementCount);
    void addTextGroupLocal(
        const glm::vec3& anchor,
        const glm::vec3& baselineDirection,
        const glm::vec3& upDirection,
        float sizeMeters,
        std::string_view text,
        HorizontalJustify justify,
        bool centerBlock,
        const Style& style);
    void appendLineGlyphs(
        std::string_view text,
        float lineYOffset,
        HorizontalJustify justify,
        float effectPadding,
        std::vector<PendingGlyph>* glyphs) const;
    [[nodiscard]] std::pair<glm::vec3, glm::vec3> normalizeTextBasis(
        const glm::vec3& baselineDirection,
        const glm::vec3& upDirection) const;
    [[nodiscard]] float explicitSizeFromUpVector(const glm::vec3& upDirection) const;
    [[nodiscard]] float measureLine(std::string_view text) const;
    [[nodiscard]] std::vector<std::string_view> splitLines(std::string_view text) const;
    [[nodiscard]] const Glyph* glyphFor(char character) const;
    [[nodiscard]] std::uint32_t glyphIndexFor(const Glyph& glyph) const;

    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;
    SDL_GPUBuffer* m_quadVertexBuffer = nullptr;
    SDL_GPUTransferBuffer* m_quadVertexTransferBuffer = nullptr;
    SDL_GPUBuffer* m_quadIndexBuffer = nullptr;
    SDL_GPUTransferBuffer* m_quadIndexTransferBuffer = nullptr;
    SDL_GPUTexture* m_fontTexture = nullptr;
    SDL_GPUSampler* m_fontSampler = nullptr;

    BufferPair m_groupBuffer{};
    BufferPair m_glyphInstanceBuffer{};
    BufferPair m_glyphMetricBuffer{};
    BufferPair m_styleBuffer{};
    BufferPair m_indirectBuffer{};

    std::vector<GroupGpu> m_groups;
    std::vector<GlyphInstanceGpu> m_glyphInstances;
    std::vector<GlyphMetricGpu> m_glyphMetrics;
    std::vector<StyleGpu> m_styles;
    std::vector<SDL_GPUIndexedIndirectDrawCommand> m_drawCommands;

    std::array<Glyph, 128> m_asciiGlyphs{};
    float m_defaultBillboardSizeMeters = 1.0f;
    float m_fontPixelSize = 64.0f;
    float m_distanceRange = 8.0f;
    float m_lineTop = 0.0f;
    float m_lineBottom = 0.0f;
    glm::vec3 m_billboardRight{ 1.0f, 0.0f, 0.0f };
    glm::vec3 m_billboardUp{ 0.0f, 1.0f, 0.0f };
};
