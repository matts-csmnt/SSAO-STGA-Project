struct VertexInput
{
	float3 pos   : POSITION;
	float4 color : COLOUR;
	float3 normal : NORMAL;
	float4 tangent : TANGENT;
	float2 uv : TEXCOORD;
};

struct VertexOutput
{
	float4 vpos  : SV_POSITION;
	float4 color : COLOUR;
	float3 normal : NORMAL;
	float2 uv : TEXCOORD;
	float3 wpos : POSITION0;
};

//materials
struct material
{
	float3 albedo;
	float metallic;
	float roughness;
	float ao;
	float _pad[2];
};

cbuffer PerFrameCB : register(b0)
{
	matrix matProjection;
	matrix matView;
	matrix matViewProjection;
	matrix matInverseProjection;
	matrix matInverseView;
	float time;
	float screenW;
	float screenH;
	float3 camPos;
	float padding[2];
};

cbuffer PerDrawCB : register(b1)
{
	matrix matModel;
	matrix matMVP;
	material mat;
};

//geometry rendering
struct GBufferOut {
	float4 vAlbedoMetallic : SV_TARGET0;
	float4 vNormalRoughAO : SV_TARGET1;
};

SamplerState linearMipSampler : register(s0);

//Shaders-----------------------------------------------------
//https://mynameismjp.wordpress.com/2009/06/17/storing-normals-using-spherical-coordinates/
// Converts a normalized cartesian direction vector
// to spherical coordinates.
float2 CartesianToSpherical(float3 cartesian)
{
	float2 spherical;

	spherical.x = atan2(cartesian.y, cartesian.x) / 3.14159f;
	spherical.y = cartesian.z;

	return spherical * 0.5f + 0.5f;
}

// Converts a spherical coordinate to a normalized
// cartesian direction vector.
float3 SphericalToCartesian(float2 spherical)
{
	float2 sinCosTheta, sinCosPhi;

	spherical = spherical * 2.0f - 1.0f;
	sincos(spherical.x * 3.14159f, sinCosTheta.x, sinCosTheta.y);
	sinCosPhi = float2(sqrt(1.0 - spherical.y * spherical.y), spherical.y);

	return float3(sinCosTheta.y * sinCosPhi.x, sinCosTheta.x * sinCosPhi.x, sinCosPhi.y);
}

//Untextured
VertexOutput PBR_VS(VertexInput i)
{
	VertexOutput o;
	o.vpos = mul(float4(i.pos, 1.0f), matMVP);
	o.color = i.color;
	o.normal = mul(i.normal, (float3x3)matModel);
	o.uv = i.uv;

	o.wpos = i.pos;	//passthrough world pos
	return o;
}

GBufferOut PBR_PS(VertexOutput i)
{
	GBufferOut o;

	float3 N = normalize(i.normal);
	float3 V = normalize(camPos - i.wpos);
	
	o.vAlbedoMetallic = float4(mat.albedo,1);

	o.vNormalRoughAO.xy = CartesianToSpherical(N);
	o.vNormalRoughAO.zw = float2(1, 1);

	return o;
}

//textured
VertexOutput PBRTex_VS(VertexInput i)
{
	VertexOutput o;
	o.vpos = mul(float4(i.pos, 1.0f), matMVP);
	o.color = i.color;
	o.normal = mul(i.normal, (float3x3)matModel);
	o.uv = i.uv;

	o.wpos = i.pos;	//passthrough world pos
	return o;
}

GBufferOut PBRTex_PS(VertexOutput i)
{
	GBufferOut o;

	float3 N = normalize(i.normal);
	float3 V = normalize(camPos - i.wpos);

	o.vAlbedoMetallic = float4(1, 1, 1, 1);
	o.vNormalRoughAO = float4(1, 1, 1, 1);
	return o;
}

//Deferred lighting---------------------------------------------------------

static float3  lightColor = float3(23.47, 21.31, 20.79);
//float3  wi = normalize(lightPos - fragPos);
//float cosTheta = max(dot(N, Wi), 0.0);
//float attenuation = calculateAttenuation(fragPos, lightPos);
//float radiance = lightColor * attenuation * cosTheta;

cbuffer LightInfo : register(b2)
{
	float4 vLightPosition; // w == 0 then directional
	float4 vLightDirection; // for directional and spot , w == 0 then point.
	float4 vLightColour; // all types
	float4 vLightAtt; // light attenuation factors spot and point.
	// various spot params... to be added.
	float4 vLightAmbient;
};

// Gbuffer textures for lighting and Debug passes.
Texture2D gBufferColourSpec : register(t0);
Texture2D gBufferNormalPow : register(t1);
Texture2D gBufferDepth : register(t2);

VertexOutput VS_Passthrough(VertexInput input)
{
	VertexOutput output;
	output.vpos = float4(input.pos.xyz, 1.0f);
	output.color = input.color;
	output.normal = input.normal.xyz;
	output.uv = input.uv.xy;

	return output;
}

float4 PS_DirectionalLight(VertexOutput input) : SV_TARGET
{
	float4 AlMt = gBufferColourSpec.Sample(linearMipSampler, input.uv);
	float4 NRAO = gBufferNormalPow.Sample(linearMipSampler, input.uv);
	float3 n = SphericalToCartesian(NRAO.xy);
	return float4(AlMt.xyz,1);
}

struct LightVolumeVertexOutput
{
	float4 vpos  : SV_POSITION;
	float4 vScreenPos : TEXCOORD0;
};

LightVolumeVertexOutput VS_LightVolume(VertexInput input)
{
	LightVolumeVertexOutput output;
	output.vpos = mul(float4(input.pos.xyz, 1.0f), matMVP);
	output.vScreenPos = output.vpos;
	return output;
}

float4 PS_PointLight(LightVolumeVertexOutput input) : SV_TARGET
{
	//float2 ScreenUV = (input.vScreenPos.xy / input.vScreenPos.w * 0.5 + 0.5) * float2(1, -1) + float2(0, 1);

	//float4 vColourSpec = gBufferColourSpec.Sample(linearMipSampler, ScreenUV);
	//float4 vNormalPow = gBufferNormalPow.Sample(linearMipSampler, ScreenUV);
	//float fDepth = gBufferDepth.Sample(linearMipSampler, ScreenUV).r;

	//// discard fragments we didn't write in the Geometry pass.
	//clip(0.99999f - fDepth);

	//// decode the gbuffer.
	//float3 materialColour = vColourSpec.xyz;
	//float3 N = vNormalPow.xyz;

	//// Decode world position for uv
	//float4 clipPos = float4(input.vScreenPos.xy / input.vScreenPos.w, fDepth, 1.0f);
	//float4 viewPos = mul(clipPos, matInverseProjection);
	//viewPos /= viewPos.w;
	//float4 worldPos = mul(viewPos, matInverseView);

	//// obtain vector to point light
	//float3 vToLight = vLightPosition.xyz - worldPos.xyz;
	//float3 lightDir = normalize(vToLight);
	//float lightDistance = length(vToLight);

	////Compute light attenuation
	//float kAtt = 1.0 / (vLightAtt.x + vLightAtt.y*lightDistance + vLightAtt.z*lightDistance*lightDistance);
	//kAtt *= 1.0f - smoothstep(vLightAtt.w - 0.25f, vLightAtt.w, lightDistance);

	//// clip outside of volume radius.
	//clip(vLightAtt.w - lightDistance);

	//float kDiffuse = max(dot(lightDir, N),0) * kAtt;

	//float3 diffuseColour = kDiffuse * materialColour * vLightColour.rgb;

	//return float4(diffuseColour.xyz, 1.f);
	return float4(1,1,1,1);
}