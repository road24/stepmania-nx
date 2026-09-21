#version 460

// TextureMode_Glow - see Sprite.cpp:658 and RageTypes.h:31-32's doc comment
// ("color replaced with white, keep alpha, combined with BLEND_ADD").

layout (location = 0) in vec4 inColor;
layout (location = 1) in vec2 inTexCoord;
layout (location = 0) out vec4 outColor;

layout (binding = 0) uniform sampler2D tex;

void main()
{
	outColor = vec4(1.0, 1.0, 1.0, texture(tex, inTexCoord).a) * inColor;
}
