///////////////////////////////////////////////////////////////////////////////
// Skybox Shader
///////////////////////////////////////////////////////////////////////////////


cbuffer PerFrameCB : register(b0)
{
	matrix matProjection;
	matrix matView;
	float  time;
	float  padding[3];
};

cbuffer PerDrawCB : register(b1)
{
    matrix matMVP;
};

TextureCube cubeTexture : register(t0);

SamplerState linearMipSampler : register(s0);


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
    float3 uvw : TEXCOORD;
};

VertexOutput VS_Skybox(VertexInput input)
{
    VertexOutput output;

    output.vpos  = mul(float4(input.pos * 100, 1.f), matMVP);
	output.uvw = input.pos;

	// push verts to extremes of NDC --- set 0.f in w coord.
	output.vpos = output.vpos.xyww; // < neat trick to push to extremes, but this needs changes to depth comp.

	// optionally pass through everything else
	/*
    output.color = input.color;
    output.normal = input.normal;
    output.tangent = input.tangent;
    output.uv = input.uv;
	*/

    return output;
}

float4 PS_Skybox(VertexOutput input) : SV_TARGET
{
 	float4 materialColor = cubeTexture.Sample(linearMipSampler, input.uvw);

 	return float4(materialColor.xyz , 1.0f);

}

///////////////////////////////////////////////////////////////////////////////
