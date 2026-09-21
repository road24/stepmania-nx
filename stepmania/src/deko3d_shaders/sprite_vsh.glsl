#version 460

// Phase 1 sprite vertex shader - see docs/architecture/11-Deko3D-TextureFormatAndShaderContract.md
// Attribute layout matches RageSpriteVertex (RageTypes.h): p, n (unused/skipped), c, t.

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec4 inColor;
layout (location = 2) in vec2 inTexCoord;

layout (location = 0) out vec4 outColor;
layout (location = 1) out vec2 outTexCoord;

// RageMatrix's raw bytes need no transpose here - confirmed against
// RageDisplay_Legacy's own glLoadMatrixf() call sites, which use the same
// column-major layout GLSL mat4 uniforms expect.
layout (std140, binding = 0) uniform Transform
{
	mat4 mvp;
} u;

void main()
{
	gl_Position = u.mvp * vec4(inPos, 1.0);
	outColor = inColor;
	outTexCoord = inTexCoord;
}
