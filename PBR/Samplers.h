#pragma once
#include "Framework.h"

// helper to create a sampler state
inline ID3D11SamplerState* create_aniso_sampler(ID3D11Device* pDevice, D3D11_TEXTURE_ADDRESS_MODE mode)
{
	ID3D11SamplerState* pSampler = nullptr;

	D3D11_SAMPLER_DESC desc = {};
	desc.Filter = D3D11_FILTER_ANISOTROPIC;
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