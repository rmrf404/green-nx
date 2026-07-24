#version 460

// H.264 NV12 -> RGB. plane0 is the R8 luma image, plane1 is the RG8 chroma
// image (U in .r, V in .g), both zero-copy views of the NVTEGRA decoder
// surface. uv_data letterboxes the frame into the screen viewport.
layout (location = 0) in vec2 vTextureCoord;
layout (location = 0) out vec4 outColor;

layout (binding = 0) uniform sampler2D plane0;
layout (binding = 1) uniform sampler2D plane1;

layout (std140, binding = 0) uniform Transformation
{
    mat3 yuvmat;
    vec3 offset;
    vec4 uv_data;
    vec4 sharp_data;
} u;

void main()
{
    vec2 uv = (vTextureCoord - u.uv_data.xy) * u.uv_data.zw;

    // Luma-only unsharp mask (chroma untouched, so no color fringing). The
    // base stream is soft -- H.264 at streaming bitrates smears fine detail.
    // sharp_data.x is the strength; .y bounds the overshoot: the result may
    // exceed the local neighborhood min/max only by that allowance, which is
    // what keeps hard edges from growing halos.
    float y = texture(plane0, uv).r;
    if (u.sharp_data.x > 0.0) {
        vec2 px = 1.0 / vec2(textureSize(plane0, 0));
        float yl = texture(plane0, uv - vec2(px.x, 0.0)).r;
        float yr = texture(plane0, uv + vec2(px.x, 0.0)).r;
        float yu = texture(plane0, uv - vec2(0.0, px.y)).r;
        float yd = texture(plane0, uv + vec2(0.0, px.y)).r;
        float average = 0.25 * (yl + yr + yu + yd);
        float lo = min(y, min(min(yl, yr), min(yu, yd)));
        float hi = max(y, max(max(yl, yr), max(yu, yd)));
        y = clamp(y + (y - average) * u.sharp_data.x,
                  max(0.0, lo - u.sharp_data.y),
                  min(1.0, hi + u.sharp_data.y));
    }

    vec3 yuv = vec3(y,
                    texture(plane1, uv).r,
                    texture(plane1, uv).g) - u.offset;
    vec3 rgb = u.yuvmat * yuv;

    outColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
