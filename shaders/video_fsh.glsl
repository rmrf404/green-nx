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
} u;

void main()
{
    vec2 uv = (vTextureCoord - u.uv_data.xy) * u.uv_data.zw;

    vec3 yuv = vec3(texture(plane0, uv).r,
                    texture(plane1, uv).r,
                    texture(plane1, uv).g) - u.offset;
    vec3 rgb = u.yuvmat * yuv;

    outColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
