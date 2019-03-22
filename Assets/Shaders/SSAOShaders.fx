///////////////////////////////////////////////////////////////////////////////
// SSAO SHADERS
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

cbuffer SSAOCB : register(b1)
{
	float random_size;
	float g_sample_rad;
	float g_intensity;
	float g_scale;
	float g_bias;
	int	  g_samples;
	int   g_blurKernelSz;
	float g_pad;
}

SamplerState linearMipSampler : register(s0);

// Gbuffer textures for lighting and Debug passes.
Texture2D gBufferColourSpec : register(t0);
Texture2D gBufferNormalPow : register(t1);
Texture2D gBufferDepth : register(t2);

Texture2D randNormal : register(t3);

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
	float4 color : COLOUR;
	float3 normal : NORMAL;
	float2 uv : TEXCOORD;
};

VertexOutput VS_Passthrough(VertexInput input)
{
	VertexOutput output;
	output.vpos = float4(input.pos.xyz, 1.0f);
	output.color = input.color;
	output.normal = input.normal.xyz;
	output.uv = input.uv.xy;

	return output;
}

float4 PS_SSAO(VertexOutput input) : SV_TARGET
{

	float4 vColourSpec = gBufferColourSpec.Sample(linearMipSampler, input.uv);
	float4 vNormalPow = gBufferNormalPow.Sample(linearMipSampler, input.uv);
	float fDepth = gBufferDepth.Sample(linearMipSampler, input.uv).r;

	// decode the gbuffer.
	float3 materialColour = vColourSpec.rgb;
	float3 N = vNormalPow.xyz;

	return float4(materialColour, 1.f);
}

float4 PS_SSAO_DEBUG_NRM(VertexOutput input) : SV_TARGET
{
	float2 sUV = float2(input.uv.x / screenW, input.uv.y / screenH);

	float4 vColourSpec = gBufferColourSpec.Sample(linearMipSampler, input.uv);
	float4 vNormalPow = gBufferNormalPow.Sample(linearMipSampler, input.uv);
	float fDepth = gBufferDepth.Sample(linearMipSampler, input.uv).r;

	float3 rnd3 = randNormal.Sample(linearMipSampler, input.uv).xyz * 2 - 1;

	// decode the gbuffer.
	float3 materialColour = vColourSpec.rgb;
	float3 N = vNormalPow.xyz;

	return float4(N, 1.f);
}

//---------------------------------------------------------------------------------------------------
//https://www.gamedev.net/articles/programming/graphics/a-simple-and-practical-approach-to-ssao-r2753
//---------------------------------------------------------------------------------------------------
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
	//worldPos /= worldPos.w;

	return worldPos.xyz;
}

float3 getNormal(float2 uv)
{
	return normalize(gBufferNormalPow.Sample(linearMipSampler, uv)).xyz;
}

float2 getRandom(float2 uv)
{
	return normalize(randNormal.Sample(linearMipSampler, float2(screenW, screenH) * uv / random_size).xy * 2.0f - 1.0f);
}

float doAmbientOcclusion(float2 tcoord, float2 uv, float3 p, float3 cnorm)
{
	float3 sample = getPosition(tcoord + uv);
	float3 diff = sample - p;
	const float3 v = normalize(diff);
	const float d = length(diff)*g_scale; //scales distance between occluders and occludee.;
	
	//range check it so we don't get grey halos around everything
	float val =  max(0.0, dot(cnorm, v) - g_bias)*(1.0 / (1.0 + d)) * g_intensity;

	//https://john-chapman-graphics.blogspot.com/2013/01/ssao-tutorial.html
	//Check if sample is in range of geometry and is not behind something
	float rangeCheck = abs(getPosition(tcoord).z - sample.z) < g_sample_rad ? 1.0 : 0.0;
	return val *= (getPosition(tcoord).z <= sample.z ? 1.0 : 0.0) * rangeCheck;
}

float4 PS_SSAO_01(VertexOutput i) : SV_TARGET
{
	float4 o;
	const float2 vec[4] = { float2(1,0),float2(-1,0), float2(0,1),float2(0,-1) };
	float3 p = getPosition(i.uv);
	float3 n = getNormal(i.uv);
	float2 rand = getRandom(i.uv);
	float ao = 0.0f;
	float rad = g_sample_rad / p.z;

	//**SSAO Calculation**// 
	int iterations = g_samples;
	for (int j = 0; j < iterations; ++j)
	{
		float2 coord1 = reflect(vec[j], rand)*rad;
		float2 coord2 = float2(coord1.x*0.707 - coord1.y*0.707, coord1.x*0.707 + coord1.y*0.707);

		ao += doAmbientOcclusion(i.uv, coord1*0.25, p, n);
		ao += doAmbientOcclusion(i.uv, coord2*0.5, p, n);
		ao += doAmbientOcclusion(i.uv, coord1*0.75, p, n);
		ao += doAmbientOcclusion(i.uv, coord2, p, n);
	}

	ao /= (float)iterations*4.0;

	//**END**// 
	//Do stuff here with your occlusion value ao modulate ambient lighting, write it to a buffer for later 
	//use, etc. 

	float3 outCol = float3(gBufferColourSpec.Sample(linearMipSampler, i.uv).xyz);
	outCol -= ao;
	o = float4(outCol, 1.0f);

	return o;
}

///////////////////////////////////////////////////////////////////////////////
// Other Effects -- Blurs...
///////////////////////////////////////////////////////////////////////////////

//https://www.shadertoy.com/view/XdfGDH

float normpdf(float x, float sigma)
{
	return 0.39894*exp(-0.5*x*x / (sigma*sigma)) / sigma;
}

float4 PS_BLUR_GAUSS(VertexOutput input) : SV_TARGET
{

	//declare stuff
	const int mSize = 11;
	const int kSize = (mSize - 1) / 2;
	float kernel[mSize];
	float3 final_colour = float3(0.0, 0.0, 0.0);

	//stride early
	//if ((input.uv.x) % g_blurKernelSz != 0)
	//{
	//	final_colour += float3(0, 0, 1);
	//	return float4(final_colour, 1);
	//}

	//create the 1-D kernel
	float sigma = 7.0;
	float Z = 0.0;
	for (int i = 0; i <= kSize; ++i)
	{
		kernel[kSize + i] = kernel[kSize - i] = normpdf(i, sigma);
	}

	//get the normalization factor (as the gaussian has been clamped)
	for (int j = 0; j < mSize; ++j)
	{
		Z += kernel[j];
	}

	//read out the texels
	for (int k = -kSize; k <= kSize; ++k)
	{
		for (int l = -kSize; l <= kSize; ++l)
		{
			float4 samp = gBufferColourSpec.Sample(linearMipSampler, input.uv + (float2(k, l) / float2(screenW, screenH)));

			final_colour += kernel[kSize + l] * kernel[kSize + k] * samp.xyz;
		}
	}

	//return float4(final_colour, 1.0f);
	return float4(final_colour / (Z*Z), 1.0);
}