#version 460

// HUD overlay fragment shader. Stage 1: sample the rasterized HUD texture (the
// semi-transparent panel background + white stats text, composited on the CPU)
// and emit it straight through -- the pass is alpha-blended over the video, so
// the texture's own alpha controls the overlay.
layout (location = 0) in vec2 vTextureCoord;
layout (location = 0) out vec4 outColor;

layout (binding = 0) uniform sampler2D hudtex;

void main()
{
    outColor = texture(hudtex, vTextureCoord);
}
