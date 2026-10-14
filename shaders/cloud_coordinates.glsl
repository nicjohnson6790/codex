// SDL presents a top-left framebuffer while clip-space +Y points upward.
// Vulkan implements this with a negative-height viewport.
vec2 cloudPixelToNdc(vec2 pixelCenter,vec2 fullExtent)
{
    vec2 uv=pixelCenter/fullExtent;
    return vec2(uv.x*2.0f-1.0f,1.0f-uv.y*2.0f);
}
