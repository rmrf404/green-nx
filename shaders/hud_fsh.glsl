#version 460

// HUD overlay fragment shader. Stage 0: a flat semi-transparent panel to prove
// the overlay pass + alpha blending work over the video. Stage 1 samples a text
// texture here instead.
layout (location = 0) in vec2 vTextureCoord;
layout (location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(0.0, 0.0, 0.0, 0.55);
}
