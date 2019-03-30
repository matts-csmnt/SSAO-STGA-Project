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
	float g_maxDistance;
	float _pad1[1];
}

cbuffer BlurCB : register(b2)
{
	int   g_blurKernelSz;
	float g_blurSigma;
	int g_downsampleBlurFac;
	int g_kawaseIteration;
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
	//Range checking from Shadertoy Spiral Kernel Demo...
	float3 sample = getPosition(tcoord + uv);
	float3 diff = sample - p;
	float l = length(diff);
	float3 v = diff / l;
	float d = l * g_scale;

	float val = max(0.0, dot(cnorm, v) - g_bias)*(1.0 / (1.0 + d)) * g_intensity;
	val *= smoothstep(g_maxDistance, g_maxDistance * 0.5, l);
	return val;
}

float doAOJCRangeCheck(float2 tcoord, float2 uv, float3 p, float3 cnorm)
{
	float3 sample = getPosition(tcoord + uv);
	float3 diff = sample - p;
	const float3 v = normalize(diff);
	const float d = length(diff)*g_scale; //scales distance between occluders and occludee.;

	//range check it so we don't get grey halos around everything
	float val = max(0.0, dot(cnorm, v) - g_bias)*(1.0 / (1.0 + d)) * g_intensity;

	//https://john-chapman-graphics.blogspot.com/2013/01/ssao-tutorial.html
	//Check if sample is in range of geometry and is not behind something
	float rangeCheck = abs(p.z - sample.z) < g_sample_rad ? 1.0 : 0.0;
	return val *= (p.z <= sample.z ? 1.0 : 0.0) * rangeCheck;
}

float PS_SSAO_01(VertexOutput i) : SV_TARGET
{
	float o;
	const float2 vec[4] = { float2(1,0),float2(-1,0), float2(0,1),float2(0,-1) };
	float3 p = getPosition(i.uv);
	float3 n = getNormal(i.uv);
	float2 rand = getRandom(i.uv);
	float ao = 0.0f;
	float rad = g_sample_rad / p.z;

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
	o = ao;

	//TEST:
	//pow(occlusion, power);

	return ao;
}

float PS_SSAO_03(VertexOutput i) : SV_TARGET
{
	float o;
	const float2 vec[4] = { float2(1,0),float2(-1,0), float2(0,1),float2(0,-1) };
	float3 p = getPosition(i.uv);
	float3 n = getNormal(i.uv);
	float2 rand = getRandom(i.uv);
	float ao = 0.0f;
	float rad = g_sample_rad / p.z;

	int iterations = g_samples;
	for (int j = 0; j < iterations; ++j)
	{
		float2 coord1 = reflect(vec[j], rand)*rad;
		float2 coord2 = float2(coord1.x*0.707 - coord1.y*0.707, coord1.x*0.707 + coord1.y*0.707);

		//Try with different range checking method...
		ao += doAOJCRangeCheck(i.uv, coord1*0.25, p, n);
		ao += doAOJCRangeCheck(i.uv, coord2*0.5, p, n);
		ao += doAOJCRangeCheck(i.uv, coord1*0.75, p, n);
		ao += doAOJCRangeCheck(i.uv, coord2, p, n);
	}
	ao /= (float)iterations*4.0;
	o = ao;

	return ao;
}

//Spiral SSAO --
//---------------------------------------------------------------------------------------------------
//https://www.shadertoy.com/view/Ms33WB -- appears to give a smoother final image...
//---------------------------------------------------------------------------------------------------

float hash12(float2 p)
{
	float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.11369, 0.13787)); //MOD3
	p3 += dot(p3, p3.yzx + 19.19);
	return frac((p3.x + p3.y) * p3.z);
}

float doSpiralAmbientOcclusion(float2 tcoord, float2 uv, float3 p, float3 cnorm)
{
	float3 sample = getPosition(tcoord + uv);
	float3 diff = sample - p;
	float l = length(diff);
	float3 v = diff / l;
	float d = l * g_scale;

	float val = max(0.0, dot(cnorm, v) - g_bias)*(1.0 / (1.0 + d));
	val *= smoothstep(g_maxDistance, g_maxDistance * 0.5, l);
	return val;
}

static float GOLDEN_ANGLE = 2.4;

float PS_SSAO_02(VertexOutput i) : SV_TARGET
{
	float3 p = getPosition(i.uv);
	float3 n = getNormal(i.uv);
	float2 rand = getRandom(i.uv);
	float ao = 0.0f;
	float rad = g_sample_rad / p.z;

	//float goldenAngle = 2.4;
	float inv = 1.0 / float(g_samples*4);

	float rotatePhase = hash12(i.uv*100.0f) * 6.28f;
	float rStep = inv * rad;
	float2 spiralUV;
	float radius = 0.0f;

	for (int j = 0; j < g_samples*4; j++) {
		spiralUV.x = sin(rotatePhase);
		spiralUV.y = cos(rotatePhase);
		radius += rStep;
		ao += doSpiralAmbientOcclusion(i.uv, spiralUV * radius, p, n);
		rotatePhase += GOLDEN_ANGLE;
	}
	ao *= inv;
	return ao;
}

//---------------------------------------------------------------------------------------------------
//GPU ZEN 1
//---------------------------------------------------------------------------------------------------

#define PI				3.1415f
#define TWO_PI			2.0f * PI

float2 VogelDiskOffset(int sampleIndex, float phi)
{
	float r = sqrt(sampleIndex + 0.5f) / sqrt((g_samples * 4));
	float theta = sampleIndex * GOLDEN_ANGLE + phi;

	float sine, cosine;
	sincos(theta, sine, cosine);

	return float2(r * cosine, r * sine);
}

float2 AlchemySpiralOffset(int sampleIndex, float phi)
{
	float alpha = float(sampleIndex + 0.5f) / (g_samples * 4);
	float theta = 7.0f*TWO_PI*alpha + phi;

	float sine, cosine;
	sincos(theta, sine, cosine);

	return float2(cosine, sine);
}

float InterleavedGradientNoise(float2 position_screen)
{
	float3 magic = float3(0.06711056f, 4.0f*0.00583715f, 52.9829189f);
	return frac(magic.z * frac(dot(position_screen, magic.xy)));
}


float AlchemyNoise(int2 position_screen)
{
	return 30.0f*(position_screen.x^position_screen.y) + 10.0f*(position_screen.x*position_screen.y);
}

float PS_SSAO_04(VertexOutput i) : SV_TARGET
{
	float2 radius_screen = 0.5;/*radius_world / i.vpos.z;
	radius_screen = min(radius_screen, maxRadius_screen);
	radius_screen.y *= aspect;*/

	float noise = InterleavedGradientNoise(i.vpos.xy);
	float alchemyNoise = AlchemyNoise((int2)i.vpos.xy);

	float ao = 0.0f;

	for (int j = 0; j < g_samples*4; j++)
	{
		float2 sampleOffset = 0.0f;
		
		//sampleOffset = VogelDiskOffset(i, TWO_PI*noise);
		sampleOffset = AlchemySpiralOffset(j, alchemyNoise);

		float2 sampleTexCoord = i.uv + radius_screen * sampleOffset;

		float3 samplePosition = getPosition(sampleTexCoord);
		float3 v = samplePosition - i.vpos;

		ao += max(0.0f, dot(v, getNormal(sampleTexCoord)) + 0.002f*i.vpos.z) / (dot(v, v) + 0.001f);
	}

	ao = saturate(ao / (g_samples*4));
	//ao = pow(ao, g_intensity);

	return ao;
}

///////////////////////////////////////////////////////////////////////////////
// Other Effects -- Blurs...
///////////////////////////////////////////////////////////////////////////////

//https://www.shadertoy.com/view/XdfGDH

Texture2D ssaoBuffer : register(t0);

float normpdf(float x, float sigma)
{
	return 0.39894*exp(-0.5*x*x / (sigma*sigma)) / sigma;
}

float PS_BLUR_GAUSS(VertexOutput input) : SV_TARGET
{
	float2 RTSize = float2(screenW / g_downsampleBlurFac, screenH / g_downsampleBlurFac);
	float2 RTPixelSz = float2(1.f / RTSize.x,1.f / RTSize.y);

	const int mSize = 11;
	const int kSize = (mSize - 1) / 2;
	float kernel[mSize];
	float final = 0.0f;

	//create the 1-D kernel
	float sigma = g_blurSigma; //7.0;
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

	//make sure we are targetting the correct resolution for blurs
	float2 targetSize = float2(screenW, screenH);

	//read out the texels
	for (int k = -kSize; k <= kSize; ++k)
	{
		for (int l = -kSize; l <= kSize; ++l)
		{
			float samp = ssaoBuffer.Sample(linearMipSampler, input.uv + (float2(k, l) / targetSize));
			final += kernel[kSize + l] * kernel[kSize + k] * samp.x;
		}
	}

	return float(final / (Z*Z));
}

//http://rastergrid.com/blog/2010/09/efficient-gaussian-blur-with-linear-sampling/
//Fast Gaussian Blur, 7 tap -- X Y Seperation
static float offset[3] = { 0.0, 1.3846153846, 3.2307692308 };
static float weight[3] = { 0.2270270270, 0.3162162162, 0.0702702703 };

float PS_BLUR_GAUSS_X(VertexOutput input) : SV_TARGET
{
	float2 RTSize = float2(screenW / g_downsampleBlurFac, screenH / g_downsampleBlurFac);
	float2 RTPixelSz = float2(1.f / RTSize.x,1.f / RTSize.y);

	float output = ssaoBuffer.Sample(linearMipSampler, input.vpos.xy / RTSize) * weight[0];

	for (int i = 1; i < 3; i++)
	{
		output += ssaoBuffer.Sample(linearMipSampler, (input.vpos.xy + float2(offset[i], 0.0)) / RTSize) * weight[i];
		output += ssaoBuffer.Sample(linearMipSampler, (input.vpos.xy - float2(offset[i], 0.0)) / RTSize) * weight[i];
	}

	return output;
}

float PS_BLUR_GAUSS_Y(VertexOutput input) : SV_TARGET
{
	float2 RTSize = float2(screenW / g_downsampleBlurFac, screenH / g_downsampleBlurFac);
	float2 RTPixelSz = float2(1.f / RTSize.x,1.f / RTSize.y);

	float output = ssaoBuffer.Sample(linearMipSampler, input.vpos.xy / RTSize) * weight[0];
	
	for (int i = 1; i < 3; i++) 
	{
		output += ssaoBuffer.Sample(linearMipSampler, (input.vpos.xy + float2(0.0, offset[i])) / RTSize) * weight[i];
		output += ssaoBuffer.Sample(linearMipSampler, (input.vpos.xy - float2(0.0, offset[i])) / RTSize) * weight[i];
	}
	
	return output;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Developed by Masaki Kawase, Bunkasha Games
// Used in DOUBLE-S.T.E.A.L. (aka Wreckless)
// From his GDC2003 Presentation: Frame Buffer Postprocessing Effects in  DOUBLE-S.T.E.A.L (Wreckless)
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
static int kawaseKernel[5] = { 0, 1, 2, 2, 3 };
static int medKawaseKernel[4] = { 0, 1, 1, 2 };
static int smolKawaseKernel[3] = { 0, 1, 1 };

float DoKawase(float2 uv, float2 dUV)
{
	float cOut;
	float2 texCoordSample;

	// Sample top left pixel
	texCoordSample.x = uv.x - dUV.x;
	texCoordSample.y = uv.y + dUV.y;
	cOut = ssaoBuffer.Sample(linearMipSampler, texCoordSample);

	// Sample top right pixel
	texCoordSample.x = uv.x + dUV.x;
	texCoordSample.y = uv.y + dUV.y;
	cOut += ssaoBuffer.Sample(linearMipSampler, texCoordSample);

	// Sample bottom right pixel
	texCoordSample.x = uv.x + dUV.x;
	texCoordSample.y = uv.y - dUV.y;
	cOut += ssaoBuffer.Sample(linearMipSampler, texCoordSample);

	// Sample bottom left pixel
	texCoordSample.x = uv.x - dUV.x;
	texCoordSample.y = uv.y - dUV.y;
	cOut += ssaoBuffer.Sample(linearMipSampler, texCoordSample);

	// Average 
	return cOut *= 0.25f;
}

float PS_BLUR_KAWASE(VertexOutput input) : SV_TARGET
{
	float2 RTSize = float2(screenW / g_downsampleBlurFac, screenH / g_downsampleBlurFac);
	float2 RTPixelSz = float2(1.f / RTSize.x, 1.f / RTSize.y);

	//float2 texCoordSample;
	float2 halfPixelSize = RTPixelSz / 2.0f;
	float2 dUV = (RTPixelSz * float2(kawaseKernel[g_kawaseIteration], kawaseKernel[g_kawaseIteration])) + halfPixelSize.xy;

	float cOut = DoKawase(input.uv, dUV);

	//// Sample top left pixel
	//texCoordSample.x = input.uv.x - dUV.x;
	//texCoordSample.y = input.uv.y + dUV.y;
	//cOut = ssaoBuffer.Sample(linearMipSampler, texCoordSample);

	//// Sample top right pixel
	//texCoordSample.x = input.uv.x + dUV.x;
	//texCoordSample.y = input.uv.y + dUV.y;
	//cOut += ssaoBuffer.Sample(linearMipSampler, texCoordSample);

	//// Sample bottom right pixel
	//texCoordSample.x = input.uv.x + dUV.x;
	//texCoordSample.y = input.uv.y - dUV.y;
	//cOut += ssaoBuffer.Sample(linearMipSampler, texCoordSample);

	//// Sample bottom left pixel
	//texCoordSample.x = input.uv.x - dUV.x;
	//texCoordSample.y = input.uv.y - dUV.y;
	//cOut += ssaoBuffer.Sample(linearMipSampler, texCoordSample);

	//scale test
	//texCoordSample.x = input.vpos/RTSize.x - dUV.x;
	//texCoordSample.y = input.vpos/RTSize.y + dUV.y;
	//cOut = ssaoBuffer.Sample(linearMipSampler, texCoordSample);

	//// Sample top right pixel
	//texCoordSample.x = input.vpos.x/RTSize.x + dUV.x;
	//texCoordSample.y = input.vpos.y/RTSize.y + dUV.y;
	//cOut += ssaoBuffer.Sample(linearMipSampler, texCoordSample);

	//// Sample bottom right pixel
	//texCoordSample.x = input.vpos.x/RTSize.x + dUV.x;
	//texCoordSample.y = input.vpos.y/RTSize.y - dUV.y;
	//cOut += ssaoBuffer.Sample(linearMipSampler, texCoordSample);

	//// Sample bottom left pixel
	//texCoordSample.x = input.vpos.x/RTSize.x - dUV.x;
	//texCoordSample.y = input.vpos.y/RTSize.y - dUV.y;
	//cOut += ssaoBuffer.Sample(linearMipSampler, texCoordSample);

	// Average 
	//cOut *= 0.25f;

	return cOut;
}

float PS_BLUR_KAWASE_MED(VertexOutput input) : SV_TARGET
{
	float2 RTSize = float2(screenW / g_downsampleBlurFac, screenH / g_downsampleBlurFac);
	float2 RTPixelSz = float2(1.f / RTSize.x, 1.f / RTSize.y);

	float2 halfPixelSize = RTPixelSz / 2.0f;
	float2 dUV = (RTPixelSz * float2(medKawaseKernel[g_kawaseIteration], medKawaseKernel[g_kawaseIteration])) + halfPixelSize.xy;

	float cOut = DoKawase(input.uv, dUV);

	return cOut;
}

float PS_BLUR_KAWASE_SML(VertexOutput input) : SV_TARGET
{
	float2 RTSize = float2(screenW / g_downsampleBlurFac, screenH / g_downsampleBlurFac);
	float2 RTPixelSz = float2(1.f / RTSize.x, 1.f / RTSize.y);

	float2 halfPixelSize = RTPixelSz / 2.0f;
	float2 dUV = (RTPixelSz * float2(smolKawaseKernel[g_kawaseIteration], smolKawaseKernel[g_kawaseIteration])) + halfPixelSize.xy;

	float cOut = DoKawase(input.uv, dUV);

	return cOut;
}