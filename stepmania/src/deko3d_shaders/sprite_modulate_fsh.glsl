#version 460

// TextureMode_Modulate - see Sprite.cpp:633 and
// docs/architecture/09-Deko3D-SpritePipelineCache.md S6.

layout (location = 0) in vec4 inColor;
layout (location = 1) in vec2 inTexCoord;
layout (location = 0) out vec4 outColor;

layout (binding = 0) uniform sampler2D tex;

void main()
{
	outColor = texture(tex, inTexCoord) * inColor;
}
