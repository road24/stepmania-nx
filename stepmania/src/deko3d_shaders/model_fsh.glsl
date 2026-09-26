#version 460

// Ambient+diffuse only (no specular yet - needs an eye-position uniform
// this doesn't have). Matches RageDisplay_Legacy::SetLightDirectional's
// GL_POSITION convention: lightDirWorld is used directly as the
// surface-to-light vector, not negated (RageDisplay_OGL.cpp:2055-2056,
// w=0 directional light).

layout (location = 0) in vec3 inNormalWorld;
layout (location = 1) in vec2 inTexCoord;
layout (location = 0) out vec4 outColor;

layout (binding = 0) uniform sampler2D tex;

layout (std140, binding = 1) uniform MaterialLight {
	vec4 matEmissive;
	vec4 matAmbient;
	vec4 matDiffuse;
	vec4 flags;       // x = lighting enabled, y = light 0 enabled
	vec4 lightAmbient;
	vec4 lightDiffuse;
	vec4 lightDirWorld; // xyz used
} u;

void main()
{
	vec4 texColor = texture(tex, inTexCoord);

	if( u.flags.x == 0.0 )
	{
		// Lighting off: flat emissive+ambient+diffuse, no lighting math -
		// matches RageDisplay_Legacy::SetMaterial's !bLighting fallback.
		outColor = texColor * vec4(u.matEmissive.rgb + u.matAmbient.rgb + u.matDiffuse.rgb, u.matDiffuse.a);
		return;
	}

	vec3 result = u.matEmissive.rgb;
	if( u.flags.y != 0.0 )
	{
		vec3 N = normalize(inNormalWorld);
		vec3 L = normalize(u.lightDirWorld.xyz);
		float NdotL = max(dot(N, L), 0.0);
		result += u.matAmbient.rgb * u.lightAmbient.rgb;
		result += u.matDiffuse.rgb * u.lightDiffuse.rgb * NdotL;
	}
	else
	{
		result += u.matAmbient.rgb + u.matDiffuse.rgb;
	}

	outColor = texColor * vec4(result, u.matDiffuse.a);
}
