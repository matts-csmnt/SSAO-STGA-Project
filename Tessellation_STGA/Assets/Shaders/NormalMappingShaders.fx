///////////////////////////////////////////////////////////////////////////////
// Normal Mapping Mesh Shader
///////////////////////////////////////////////////////////////////////////////


cbuffer PerFrameCB : register(b0)
{
	matrix matProjection;
	matrix matView;
    float4 lightPos;
	float  time;
	float  padding[3];
};

cbuffer PerDrawCB : register(b1)
{
    float4x4 matMVP;
    float4x4 matWorld;
    float3x3 matNormal; // e.g. inverse transpose (upper 3x3 of the world)
};

Texture2D texDiffuse : register(t0);
Texture2D texNormal : register(t1);

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
    float4 color : COLOUR;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD;
    float3 pos_ws : POSITION_WS;
};

// Sample the normal map and decode
float3 decode_normal(float2 uv)
{
    return texNormal.Sample(linearMipSampler, uv).rgb * 2.0f - 1.0f;
}

// Builds the 'TBN' matrix, a matrix that can transform from tangent space to world space.
float3x3 construct_TBN_matrix(float3 N, float3 T, float fSign)
{
    float3 B = cross(N, T) * fSign;
    return float3x3(T, B, N);
}

VertexOutput VS_Mesh(VertexInput input)
{
    VertexOutput output;
    output.vpos  = mul(float4(input.pos, 1.0f), matMVP);
    output.pos_ws = mul(float4(input.pos, 1.0f), matWorld).xyz;
    output.color = input.color;

    // Transform the normals and tangent.
    output.normal = mul(input.normal, matNormal);
    output.tangent.xyz = mul(input.tangent.xyz, matNormal);
    output.tangent.w = input.tangent.w; // sign is encoded pass through

    output.uv = input.uv;

    return output;
}

float4 PS_Mesh(VertexOutput input) : SV_TARGET
{

    float4 materialColor = texDiffuse.Sample(linearMipSampler, input.uv);

    // Get the light vector, point light.
    float3 L = normalize(lightPos.xyz - input.pos_ws);

    // build our per fragment TBN matrix.
    float3 N = normalize(input.normal);
    float3 T = normalize(input.tangent.xyz);
    float fSign = input.tangent.w;
    float3x3 matTBN = construct_TBN_matrix(N,T,fSign);
    
    // Grab the tangent space normal from the normal map
    // and transform it into world space.
    // The original input normal was only needed for calculating the TBN 
    N = mul( decode_normal(input.uv), matTBN);
    //return float4(N.xyz, 1.0f);

    // Perform some basic lighting using the normal map.
    float I = dot(L, N);
    //return float4(I,I,I, 1.0f);


    return float4(materialColor.xyz * I, 1.0f);
}

float4 PS_Debug(VertexOutput input) : SV_TARGET
{
	return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
///////////////////////////////////////////////////////////////////////////////
