#pragma once
#include "Framework.h"

//https://docs.microsoft.com/en-us/visualstudio/debugger/graphics/point-bilinear-trilinear-and-anisotropic-texture-filtering-variants

// helper to create a sampler state
inline ID3D11SamplerState* create_aniso_sampler(ID3D11Device* pDevice, D3D11_TEXTURE_ADDRESS_MODE mode)
{
	//Anisotropic filtering (most expensive, best visual quality)
	ID3D11SamplerState* pSampler = nullptr;

	D3D11_SAMPLER_DESC desc = {};
	desc.Filter = D3D11_FILTER_ANISOTROPIC;
	desc.MaxAnisotropy = 16;
	desc.AddressU = mode;
	desc.AddressV = mode;
	desc.AddressW = mode;
	desc.MinLOD = 0.f;
	desc.MaxLOD = D3D11_FLOAT32_MAX;

	HRESULT hr = pDevice->CreateSamplerState(&desc, &pSampler);
	ASSERT(!FAILED(hr) && pSampler);

	return pSampler;
}

inline ID3D11SamplerState* create_point_sampler(ID3D11Device* pDevice, D3D11_TEXTURE_ADDRESS_MODE mode)
{
	ID3D11SamplerState* pSampler = nullptr;

	//Point filtering(least expensive, worst visual quality)
	D3D11_SAMPLER_DESC desc = {};
	desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	desc.AddressU = mode;
	desc.AddressV = mode;
	desc.AddressW = mode;
	desc.MinLOD = 0.f;
	desc.MaxLOD = D3D11_FLOAT32_MAX;

	HRESULT hr = pDevice->CreateSamplerState(&desc, &pSampler);
	ASSERT(!FAILED(hr) && pSampler);

	return pSampler;
}

inline ID3D11SamplerState* create_bilinear_sampler(ID3D11Device* pDevice, D3D11_TEXTURE_ADDRESS_MODE mode)
{
	ID3D11SamplerState* pSampler = nullptr;

	D3D11_SAMPLER_DESC desc = {};
	desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	desc.AddressU = mode;
	desc.AddressV = mode;
	desc.AddressW = mode;
	desc.MinLOD = 0.f;
	desc.MaxLOD = D3D11_FLOAT32_MAX;

	HRESULT hr = pDevice->CreateSamplerState(&desc, &pSampler);
	ASSERT(!FAILED(hr) && pSampler);

	return pSampler;
}