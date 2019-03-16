///////////////////////////////////////////////////////////////////////////////
// DEFERRED RENDER PASSES -- GBUFFER & DEBUG
///////////////////////////////////////////////////////////////////////////////

cbuffer PerFrameCB : register(b0)
{
	float4x4 matProjection;
	float4x4 matView;
	float4x4 matViewProjection;
	float4x4 matInverseProjection;
	float4x4 matInverseView;
	float  time;
	float screenW;
	float screenH;
	float  padding;
};

cbuffer PerDrawCB : register(b1)
{
	matrix matModel;
	matrix matMVP;
};

Texture2D t_diffuse : register(t0);
Texture2D t_normal : register(t1);
Texture2D t_specular : register(t2);

SamplerState linearMipSampler : register(s0);

//--------------------------------------------

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
	float4 wpos : WORLDPOS;
	float4 color : COLOUR;
	float3 normal : NORMAL;
	float4 tangent : TANGENT;
	float2 uv : TEXCOORD;
};

//----GBUFFER FILL----
VertexOutput VS_Geometry(VertexInput input)
{
	VertexOutput output;

	output.vpos = mul(float4(input.pos, 1.0f), matMVP);
	output.wpos = input.pos;

	output.color = input.color;

	output.normal = mul(input.normal, (float3x3)matModel);

	output.uv = input.uv;

	output.tangent = mul(input.tangent.xyz, (float3x3)matModel);
	output.tangent.w = input.tangent.w;

	return output;
}

//////////////////////////////////////////////////////////////////////////////
// SSAO Shaders -- Store the AO value in B channel of gBuffer Colour Tex    //
//////////////////////////////////////////////////////////////////////////////

cbuffer SSAOCB : register(b2)
{
	float random_size;
	float g_sample_rad;
	float g_intensity;
	float g_scale;
	float g_bias;
	float g_pad[3];
}

Texture2D randNormal			: register(t3);		//used in sampling SSAO

float3 getNormal(float2 uv)
{
	return normalize(gBufferNormalPow.Sample(linearMipSampler, uv)).xyz;
}

float2 getRandom(float2 uv)
{
	return normalize(randNormal.Sample(linearMipSampler, float2(screenW, screenH) * uv / random_size).xy * 2.0f - 1.0f);
}

//---------------------------------------------------------------------------------------------------
//https://www.gamedev.net/articles/programming/graphics/a-simple-and-practical-approach-to-ssao-r2753
//---------------------------------------------------------------------------------------------------
float doAmbientOcclusion(float2 tcoord, float2 uv, float3 p, float3 cnorm)
{
	float3 diff = getPosition(tcoord + uv) - p;
	const float3 v = normalize(diff);
	const float d = length(diff)*g_scale; //scales distance between occluders and occludee.;
	return max(0.0, dot(cnorm, v) - g_bias)*(1.0 / (1.0 + d)) * g_intensity;
}

float getSSAOValue(float2 uv, float3 p, float3 n)
{
	const float2 vec[4] = { float2(1,0),float2(-1,0), float2(0,1),float2(0,-1) };
	float2 rand = getRandom(uv);
	float ao = 0.0f;
	float rad = g_sample_rad / p.z;

	//**SSAO Calculation**// 
	int iterations = 4;
	for (int j = 0; j < iterations; ++j)
	{
		float2 coord1 = reflect(vec[j], rand)*rad;
		float2 coord2 = float2(coord1.x*0.707 - coord1.y*0.707, coord1.x*0.707 + coord1.y*0.707);

		ao += doAmbientOcclusion(uv, coord1*0.25, p, n);
		ao += doAmbientOcclusion(uv, coord2*0.5, p, n);
		ao += doAmbientOcclusion(uv, coord1*0.75, p, n);
		ao += doAmbientOcclusion(uv, coord2, p, n);
	}

	ao /= (float)iterations*4.0;

	//**END**// 
	//Do stuff here with your occlusion value ao modulate ambient lighting, write it to a buffer for later 
	//use, etc. 

	return ao;
}

//////////////////////////////////////////////////////////////////////////////
// DEFERRED SHADERS -- GBUFFER CONTINUED								    //
//////////////////////////////////////////////////////////////////////////////

//geometry rendering
struct GBufferOut {
	float4 vColourSpec : SV_TARGET0;
	float4 vNormalAOPow : SV_TARGET1;
};

//ENCODE NORMALS W/ SPHERICAL COORDINATES----
//http://www.garagegames.com/community/forums/viewthread/78938/1#comment-555096

static const float PI = 3.14159f;

float2 cartesianToSpGPU(float3 normalizedVec)
{
	float atanYX = atan2(normalizedVec.y, normalizedVec.x);
	float2 ret = float2(atanYX / PI, normalizedVec.z);
	return (ret + 1.0) * 0.5;
}

float3 spGPUToCartesian(float2 spGPUAngles)
{
	float2 expSpGPUAngles = spGPUAngles * 2.0 - 1.0;
	float2 scTheta;

	sincos(expSpGPUAngles.x * PI, scTheta.x, scTheta.y);
	float2 scPhi = float2(sqrt(1.0 - expSpGPUAngles.y * expSpGPUAngles.y), expSpGPUAngles.y);

	// Renormalization not needed  
	return float3(scTheta.y * scPhi.x, scTheta.x * scPhi.x, scPhi.y);
}
//----

GBufferOut PS_Geometry(VertexOutput input) : SV_TARGET
{
	GBufferOut gbuffer;
	
	// build the per fragment TBN matrix.
	float3 N = normalize(input.normal);
	float3 T = normalize(input.tangent.xyz);
	float fSign = input.tangent.w;

	float3 B = cross(N, T) * fSign;
	float3x3 matTBN = float3x3(T, B, N);

	//Get values
	float4 diffuse = t_diffuse.Sample(linearMipSampler, input.uv);
	float3 normal = t_normal.Sample(linearMipSampler, input.uv).rgb;
	float  specular = t_specular.Sample(linearMipSampler, input.uv).r;

	//decode & calculate normal
	float3 map = normal * 2.0f - 1.0f;
	map = normalize(map);
	N = mul(map, matTBN);
	N = normalize(N);

	//Colour
	gbuffer.vColourSpec.rgb = diffuse.rgb;
	//Specular Intensity
	gbuffer.vColourSpec.a = specular;
	//Encoded RG Normals
	gbuffer.vNormalAOPow.rg = cartesianToSpGPU(N);
	//SSAO value
	gbuffer.vNormalAOPow.b = getSSAOValue(input.uv, input.wpos, N);
	//Specular Power
	gbuffer.vNormalAOPow.a = specular;	//find format for powers...

	return gbuffer;
}

GBufferOut PS_Geometry_NoTex(VertexOutput input) : SV_TARGET
{
	GBufferOut gbuffer;

	// build the per fragment TBN matrix.
	float3 N = normalize(input.normal);
	float3 T = normalize(input.tangent.xyz);
	float fSign = input.tangent.w;

	float3 B = cross(N, T) * fSign;
	float3x3 matTBN = float3x3(T, B, N);

	//Get values
	float4 diffuse = input.color;
	float3 normal = normalize(input.normal);
	float  specular = 1.0f;

	//Colour
	gbuffer.vColourSpec.rgb = diffuse.rgb;
	//Specular Intensity
	gbuffer.vColourSpec.a = specular;
	//Encoded RG Normals
	gbuffer.vNormalAOPow.rg = cartesianToSpGPU(normal);
	//SSAO value
	gbuffer.vNormalAOPow.b = getSSAOValue(input.uv, input.wpos, normal);
	//Specular Power
	gbuffer.vNormalAOPow.a = specular;	//find format for powers...

	return gbuffer;
}

//////////////////////////////////////////////////////////////////////////////
// DEFERRED SHADERS -- FULL SCREEN PASSES								    //
//////////////////////////////////////////////////////////////////////////////

// Gbuffer textures for lighting and Debug passes.
Texture2D gBufferColourSpec		: register(t0);		//Colour: rgb Specular Intensity: a
Texture2D gBufferNormalAOPow	: register(t1);		//Normal: rg  AO: b  Specular Power: a
Texture2D gBufferDepth			: register(t2);		//Depth: r

VertexOutput VS_Passthrough(VertexInput input)
{
	VertexOutput output;
	output.vpos = float4(input.pos.xyz, 1.0f);
	output.color = input.color;
	output.normal = input.normal.xyz;
	output.uv = input.uv.xy;
	output.tangent = input.tangent;
	return output;
}

float4 PS_GBufferDebug_Diffuse(VertexOutput input) : SV_TARGET
{
	float4 vColourSpec = gBufferColourSpec.Sample(linearMipSampler, input.uv);
	return float4(vColourSpec.xyz, 1.0f);
}

float4 PS_GBufferDebug_Normals(VertexOutput input) : SV_TARGET
{
	float4 vNormalPow = gBufferNormalAOPow.Sample(linearMipSampler, input.uv);
	float3 normal = spGPUToCartesian(vNormalPow.xy);
	return float4(normal * 0.5f + 0.5f, 1.0f);
}

float4 PS_GBufferDebug_Specular(VertexOutput input) : SV_TARGET
{
	float4 vColourSpec = gBufferColourSpec.Sample(linearMipSampler, input.uv);
	float4 vNormalPow = gBufferNormalAOPow.Sample(linearMipSampler, input.uv);
	return float4(vColourSpec.w, vNormalPow.w, 0.0f, 1.0f);
}

float3 getPosition(float2 uv)
{
	float fDepth = gBufferDepth.Sample(linearMipSampler, uv).r;

	// discard fragments we didn't write in the Geometry pass.
	clip(0.99999f - fDepth);

	float2 flipUV = uv.xy * float2(1, -1) + float2(0, 1);

	float4 clipPos = float4(flipUV * 2.0f - 1.0f, fDepth, 1.0f);
	float4 viewPos = mul(clipPos, matInverseProjection);
	viewPos /= viewPos.w;
	float4 worldPos = mul(viewPos, matInverseView);

	return worldPos.xyz;
}

float4 PS_GBufferDebug_Position(VertexOutput input) : SV_TARGET
{
	float3 worldPos = getPosition(input.uv);
	return float4(worldPos, 1.0f);
}

float4 PS_GBufferDebug_Depth(VertexOutput input) : SV_TARGET
{
	float fDepth = gBufferDepth.Sample(linearMipSampler, input.uv).r;
	float fScaledDepth = (fDepth - 0.9) * 4.0f;
	return float4(fScaledDepth,fScaledDepth,fScaledDepth, 1.0f);
}

float4 PS_GBufferDebug_SSAO(VertexOutput input) : SV_TARGET
{
	float ao = gBufferNormalAOPow.Sample(linearMipSampler, input.uv).b;
	float scaled = 1.0f - ao;
	return float4(scaled, scaled, scaled, 1.0f);
}