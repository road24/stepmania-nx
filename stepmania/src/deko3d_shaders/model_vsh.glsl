#version 460

layout (location = 0) in vec3 inPosition;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec2 inTexCoord;

layout (location = 0) out vec3 outNormalWorld;
layout (location = 1) out vec2 outTexCoord;

layout (std140, binding = 0) uniform Transform {
	mat4 mvp;
	mat4 world;
} u;

void main()
{
	gl_Position = u.mvp * vec4(inPosition, 1.0);
	outNormalWorld = mat3(u.world) * inNormal;
	outTexCoord = inTexCoord;
}
