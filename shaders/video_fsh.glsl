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
    vec4 contrast_data;
} u;

void main()
{
    vec2 uv = (vTextureCoord - u.uv_data.xy) * u.uv_data.zw;

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

    rgb = clamp(rgb, 0.0, 1.0);
    // Endpoint-protected S-curve: deepens shadows and lifts highlights while
    // preserving true black, true white, and the mid-point.
    rgb += u.contrast_data.x * (rgb - vec3(0.5)) *
           (vec3(1.0) - abs(rgb * 2.0 - vec3(1.0)));
    rgb = clamp(rgb, 0.0, 1.0);

    // Strobe-test overlay. A tiny 5x7 bitmap font is generated in the shader
    // so deko3d does not need SDL/font ownership while streaming.
    if (u.sharp_data.z > 0.0) {
        vec2 p = vec2(gl_FragCoord.x - 10.0, gl_FragCoord.y - 10.0);
        if (p.x >= -6.0 && p.x < 250.0 && p.y >= -6.0 && p.y < 30.0)
            rgb = mix(rgb, vec3(0.0), 0.72);

        ivec2 cell = ivec2(floor(p / 3.0));
        int slot = cell.x / 6;
        ivec2 gp = ivec2(cell.x - slot * 6, cell.y);
        int mode = int(u.sharp_data.z) - 1;
        int ch = -1;
        // Character ids: A,D,E,F,G,H,I,L,M,O,P,R,S,W,X,U,T.
        if (slot == 0) ch = 12;      // S
        else if (slot == 1) ch = 5;  // H
        else if (slot == 2) ch = 0;  // A
        else if (slot == 3) ch = 11; // R
        else if (slot == 4) ch = 10; // P
        else if (slot >= 6) {
            int n = slot - 6;
            if (mode == 0) { int s[3] = int[3](9, 3, 3); if (n < 3) ch=s[n]; }
            if (mode == 1) { int s[3] = int[3](7, 9, 13); if (n < 3) ch=s[n]; }
            if (mode == 2) { int s[6] = int[6](8, 2, 1, 6, 15, 8); if(n<6)ch=s[n]; }
            if (mode == 3) { int s[4] = int[4](5, 6, 4, 5); if (n < 4) ch=s[n]; }
            if (mode == 4) { int s[7] = int[7](2, 14, 16, 11, 2, 8, 2); if(n<7)ch=s[n]; }
        }
        if (ch >= 0 && gp.x >= 0 && gp.x < 5 && gp.y >= 0 && gp.y < 7) {
            const uint rows[140] = uint[140](
                14,17,17,31,17,17,17, 30,17,17,17,17,17,30,
                31,16,16,30,16,16,31, 31,16,16,30,16,16,16,
                14,17,16,23,17,17,15, 17,17,17,31,17,17,17,
                31,4,4,4,4,4,31, 16,16,16,16,16,16,31,
                17,27,21,17,17,17,17, 14,17,17,17,17,17,14,
                30,17,17,30,16,16,16, 30,17,17,30,20,18,17,
                15,16,16,14,1,1,30, 17,17,17,17,21,21,10,
                31,1,2,4,8,16,31, 17,17,17,17,17,17,14,
                31,4,4,4,4,4,4, 14,17,16,16,16,17,14,
                17,25,21,19,17,17,17, 31,4,4,4,4,4,4);
            uint row = rows[ch * 7 + gp.y];
            if (((row >> uint(4 - gp.x)) & 1u) != 0u)
                rgb = vec3(1.0, 0.9, 0.2);
        }
    }

    if (u.contrast_data.y > 0.0) {
        vec2 p = vec2(gl_FragCoord.x - 10.0, gl_FragCoord.y - 50.0);
        if (p.x >= -6.0 && p.x < 310.0 && p.y >= -6.0 && p.y < 30.0)
            rgb = mix(rgb, vec3(0.0), 0.72);
        ivec2 cell = ivec2(floor(p / 3.0));
        int slot = cell.x / 6;
        ivec2 gp = ivec2(cell.x - slot * 6, cell.y);
        int mode = int(u.contrast_data.y) - 1;
        int ch = -1;
        // CONTRAST prefix. ids 17=C, 18=N, 19=T.
        if (slot == 0) ch = 17;
        else if (slot == 1) ch = 9;
        else if (slot == 2) ch = 18;
        else if (slot == 3) ch = 19;
        else if (slot == 4) ch = 11;
        else if (slot == 5) ch = 0;
        else if (slot == 6) ch = 12;
        else if (slot == 7) ch = 19;
        else if (slot >= 9) {
            int n = slot - 9;
            if (mode == 0) { int s[3] = int[3](9, 3, 3); if (n < 3) ch=s[n]; }
            if (mode == 1) { int s[3] = int[3](7, 9, 13); if (n < 3) ch=s[n]; }
            if (mode == 2) { int s[6] = int[6](8, 2, 1, 6, 15, 8); if(n<6)ch=s[n]; }
            if (mode == 3) { int s[4] = int[4](5, 6, 4, 5); if (n < 4) ch=s[n]; }
        }
        if (ch >= 0 && gp.x >= 0 && gp.x < 5 && gp.y >= 0 && gp.y < 7) {
            const uint rows[140] = uint[140](
                14,17,17,31,17,17,17, 30,17,17,17,17,17,30,
                31,16,16,30,16,16,31, 31,16,16,30,16,16,16,
                14,17,16,23,17,17,15, 17,17,17,31,17,17,17,
                31,4,4,4,4,4,31, 16,16,16,16,16,16,31,
                17,27,21,17,17,17,17, 14,17,17,17,17,17,14,
                30,17,17,30,16,16,16, 30,17,17,30,20,18,17,
                15,16,16,14,1,1,30, 17,17,17,17,21,21,10,
                31,1,2,4,8,16,31, 17,17,17,17,17,17,14,
                31,4,4,4,4,4,4, 14,17,16,16,16,17,14,
                17,25,21,19,17,17,17, 31,4,4,4,4,4,4);
            uint row = rows[ch * 7 + gp.y];
            if (((row >> uint(4 - gp.x)) & 1u) != 0u)
                rgb = vec3(0.3, 0.9, 1.0);
        }
    }

    outColor = vec4(rgb, 1.0);
}
