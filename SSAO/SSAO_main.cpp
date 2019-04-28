#include "Framework.h"

#include "ShaderSet.h"
#include "Mesh.h"
#include "Texture.h"
#include <vector>
#include <queue>

#include "..\Libraries\fbx_load.h"
#include "Samplers.h"

#define COLLECT_DATA 0	// flag for collecting data as csv

#if COLLECT_DATA == 1
#include <array>
#include <fstream>
#include <iostream>
constexpr int kEntriesToCollect = 100;
struct data_out {
	float frameTime;
	u16 sampler, ssao, blur;
	int blurDS, ssaoDS, ssaoSamp;
};
std::vector<data_out> g_dataCollection;
#endif

constexpr float kBlendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
constexpr UINT kSampleMask = 0xffffffff;
constexpr u32 kLightGridSize = 24;
constexpr u16 kRoomPlanes = 3;

constexpr u8 MAX_TARGET_DOWNSIZE = 4;
constexpr int MAX_FRAMES_FOR_PROFILE_QUEUE = 60;
constexpr float kTargetFrameTimeMs = 16.7f;

//================================================================================
// SSAO APPLICATION
//================================================================================
class SSAOApp : public FrameworkApp
{
public:

	//For profiling
	struct FrameProfile {
		ID3D11Query* startTime;
		ID3D11Query* endTime;
		ID3D11Query* disjoint;

		u64 sData, eData;
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dData;
	};

	struct PerFrameCBData
	{
		m4x4 m_matProjection;
		m4x4 m_matView;
		m4x4 m_matViewProjection;
		m4x4 m_matInverseProjection;
		m4x4 m_matInverseView;
		f32	 m_time;
		f32	 m_screenW;
		f32	 m_screenH;
		f32  m_padding;
	};

	struct PerDrawCBData
	{
		m4x4 m_matModel;
		m4x4 m_matMVP;
	};

	struct SSAOCBData
	{
		float random_size;
		float g_sample_rad;
		float g_intensity;
		float g_scale;
		float g_bias;
		int g_samples;
		float g_maxDistance;
		float g_pad[1];
	};

	struct BlurCBData
	{
		int   g_blurKernelSz;
		float g_blurSigma;
		int g_downsampleBlurFac;
		int g_kawaseIteration;
	};

	enum ELightType
	{
		kLightType_Directional,
		kLightType_Point,
		kLightType_Spot
	};

	// Light info presented to the shader constant buffer.
	struct LightInfo
	{
		v4 m_vPosition; // w == 0 then directional
		v4 m_vDirection; // for directional and spot , w == 0 then point.
		v4 m_vColour; // all types
		v4 m_vAtt; // attenuation factors + spot exponent in w
		// various spot params... to be added.
		v4 m_vAmbient = v4(0,0,0,0);
	};

	// A more general light management structure.
	struct Light
	{
		LightInfo m_shaderInfo;
		ELightType m_type;
	};

	void on_init(SystemsInterface& systems) override
	{
		m_position = v3(0.5f, 0.5f, 0.5f);
		m_size = 1.0f;
		systems.pCamera->eye = v3(44.f, 18.f, 32.f);
		systems.pCamera->look_at(v3(0.f, 3.f, 0.f));

		//profiling
		init_profile_queue();
#if COLLECT_DATA == 1
		g_dataCollection.reserve(kEntriesToCollect);
#endif
		create_shaders(systems);

		create_gbuffer(systems.pD3DDevice, systems.pD3DContext, systems.width, systems.height);

		create_postfx_resources(systems.pD3DDevice, systems.pD3DContext, systems.width / m_blurTargetDownSize, systems.height / m_blurTargetDownSize);
		
		create_ssao_resources(systems.pD3DDevice, systems.pD3DContext, systems.width / m_ssaoTargetDownSize, systems.height / m_ssaoTargetDownSize);

		create_blur_downsample_viewport(systems.width / m_blurTargetDownSize, systems.height / m_blurTargetDownSize);
		create_ssao_downsample_viewport(systems.width / m_ssaoTargetDownSize, systems.height / m_ssaoTargetDownSize);

		// create fullscreen quad for post-fx / lighting passes. (-1, 1) in XY
		create_mesh_quad_xy(systems.pD3DDevice, m_fullScreenQuad, 1.0f);

		// Create Per Frame Constant Buffer.
		m_pPerFrameCB = create_constant_buffer<PerFrameCBData>(systems.pD3DDevice);

		// Create Per Draw Constant Buffer.
		m_pPerDrawCB = create_constant_buffer<PerDrawCBData>(systems.pD3DDevice);

		// Create Per Light Constant Buffer.
		m_pLightInfoCB = create_constant_buffer<LightInfo>(systems.pD3DDevice);

		//create blur CB
		m_pBlurCBData = create_constant_buffer<BlurCBData>(systems.pD3DDevice);

		// Initialize a mesh from an .OBJ file
		create_mesh_from_obj(systems.pD3DDevice, m_plane, "../Assets/Models/plane.obj", 2.f);
		create_mesh_from_obj(systems.pD3DDevice, m_lightVolumeSphere, "../Assets/Models/unit_sphere.obj", 1.f);

		// We need a sampler state to define wrapping and mipmap parameters.
		m_pSamplerState[kLinear] = create_basic_sampler(systems.pD3DDevice, D3D11_TEXTURE_ADDRESS_WRAP);
		m_pSamplerState[kAniso] = create_aniso_sampler(systems.pD3DDevice, D3D11_TEXTURE_ADDRESS_WRAP);
		m_pSamplerState[kPoint] = create_point_sampler(systems.pD3DDevice, D3D11_TEXTURE_ADDRESS_WRAP);
		m_pSamplerState[kBiLinear] = create_bilinear_sampler(systems.pD3DDevice, D3D11_TEXTURE_ADDRESS_WRAP);

		// Setup per-frame data
		m_perFrameCBData.m_time = 0.0f;
		m_perFrameCBData.m_screenW = systems.width;
		m_perFrameCBData.m_screenH = systems.height;

		//SSAO---
		bool mOk = create_mesh_from_fbx(systems.pD3DDevice, m_s_dragon, "../Assets/Models/s_dragon/stanford-dragon.fbx");
		if (!mOk)
		{
			panicF("Error Loading FBX");
		}

		m_pSSAOCB = create_constant_buffer<SSAOCBData>(systems.pD3DDevice);

		//random normal lookup tex
		m_rndnrm.init_from_image(systems.pD3DDevice, "../Assets/Textures/rnd_nrm.png", false);

		//setup plane transforms
		m_mmRoomPlanes[0] = m4x4::CreateTranslation(0.f, 0.f, 0.f);
		m_mmRoomPlanes[1] = m4x4::CreateRotationX(degToRad(90)) * m4x4::CreateTranslation(v3(0, 15, -15));
		m_mmRoomPlanes[2] = m4x4::CreateRotationX(degToRad(90))* m4x4::CreateRotationY(degToRad(90)) * m4x4::CreateTranslation(v3(-15, 15, 0));

		// create additive render states.
		{
			// Additive
			D3D11_BLEND_DESC desc = {};
			desc.AlphaToCoverageEnable = FALSE;
			desc.IndependentBlendEnable = FALSE;
			desc.RenderTarget[0].BlendEnable = TRUE;
			desc.RenderTarget[0].SrcBlend = desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
			desc.RenderTarget[0].DestBlend = desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
			desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

			systems.pD3DDevice->CreateBlendState(&desc, &m_pBlendStates[BlendStates::kAdditive]);

			// Opaque
			desc.RenderTarget[0].BlendEnable = FALSE;
			systems.pD3DDevice->CreateBlendState(&desc, &m_pBlendStates[BlendStates::kOpaque]);
		}
	}

	void create_shaders(SystemsInterface &systems)
	{
		// Geometry pass shaders.
		m_geometryPassShader.init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("../Assets/Shaders/DeferredShaders.fx", "VS_Geometry", "PS_Geometry")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);
		m_geometryNoTex.init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("../Assets/Shaders/DeferredShaders.fx", "VS_Geometry", "PS_Geometry_NoTex")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);

		// Lighting pass shaders
		m_directionalLightShader.init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("../Assets/Shaders/DeferredShaders.fx", "VS_Passthrough", "PS_DirectionalLight")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);
		m_pointLightShader.init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("../Assets/Shaders/DeferredShaders.fx", "VS_LightVolume", "PS_PointLight")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		); 

		//SSAO---
		m_SSAOShaders[kStandardSSAO].init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("../Assets/Shaders/SSAOShaders.fx", "VS_Passthrough", "PS_SSAO_01")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);
		m_SSAOShaders[kSpiralSSAO].init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("../Assets/Shaders/SSAOShaders.fx", "VS_Passthrough", "PS_SSAO_02")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);
		m_SSAOShaders[KGPUZENAlchemy].init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("../Assets/Shaders/SSAOShaders.fx", "VS_Passthrough", "PS_SSAO_04")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);
		
		m_GaussBlur.init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("../Assets/Shaders/SSAOShaders.fx", "VS_Passthrough", "PS_BLUR_GAUSS")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);
		m_GaussX.init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("../Assets/Shaders/SSAOShaders.fx", "VS_Passthrough", "PS_BLUR_GAUSS_X")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		); 
		m_GaussY.init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("../Assets/Shaders/SSAOShaders.fx", "VS_Passthrough", "PS_BLUR_GAUSS_Y")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);
		m_kawase.init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("../Assets/Shaders/SSAOShaders.fx", "VS_Passthrough", "PS_BLUR_KAWASE")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);

		m_ssaoDebugShader.init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("../Assets/Shaders/DeferredShaders.fx", "VS_Passthrough", "PS_SSAODebug")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);
		create_lights();
	}

	void create_lights()
	{	
		// A directional light.
		{
			Light l = {};
			l.m_shaderInfo.m_vDirection = v4(0.5773, 0.5773, 0.5773, 0);
			l.m_shaderInfo.m_vColour = v4(1.f, 0.7f, .6f, 0.f) * 0.2f;
			l.m_shaderInfo.m_vPosition = v4(0, 0, 0, 0);
			l.m_type = kLightType_Directional;
			l.m_shaderInfo.m_vAmbient = v4(0.15,0.15,0.2,1);

			m_lights.push_back(l);
		}

		// Lots of point lights.
		v4 colours[] =
		{
			v4(1,1,1,0),
			v4(1,1,0,0),
			v4(0,1,1,0),
			v4(1,0,1,0)
		};

		for (u32 i = 0; i < kLightGridSize; ++i)
		{
			for (u32 j = 0; j < kLightGridSize; ++j)
			{
				Light l = {};
				l.m_shaderInfo.m_vDirection = v4(0, 0, 0, 0);
				l.m_shaderInfo.m_vColour = colours[j % 5] * 0.9f;
				l.m_shaderInfo.m_vPosition = v4(i - 5.0, 0.5f, j - 5.0, 1.0f);
				l.m_shaderInfo.m_vAtt = v4(0.001f, 0.1f, 5.0f, 2.0f);
				l.m_type = kLightType_Point;

				m_lights.push_back(l);
			}
		}
	}

	void on_update(SystemsInterface& systems) override
	{
		//////////////////////////////////////////////////////////////////////////
		// You can use features from the ImGui library.
		// Investigate the ImGui::ShowDemoWindow() function for ideas.
		// see also : https://github.com/ocornut/imgui
		//////////////////////////////////////////////////////////////////////////

		ImGui::SetNextWindowSize(ImVec2(200, 100), ImGuiSetCond_FirstUseEver);
		ImGui::Begin("Frame Analysis");
			ImGui::Checkbox("Enable Profiling", &m_enableProfiling);

			//Copy data over for now... 
			int elm = 0;
			for (float f : m_frameData._Get_container())
			{
				test_data[elm++] = f;
			}

			ImGui::TextColored(ImVec4(0, 1, 0, 1), "Technique Timing (ms):", m_profilingDataQueue.size());
			ImVec2 plotextent(ImGui::GetContentRegionAvailWidth(), 100);
			ImGui::PlotLines("Technique (ms)", &test_data[0], m_frameData.size(), 0, nullptr, 0, kTargetFrameTimeMs, plotextent);

			ImGui::TextColored(ImVec4(0, 1, 0, 1), "Profiling Query Queue Sz: %i", m_profilingDataQueue.size());

#if COLLECT_DATA == 1	//send data to csv
			if (m_enableProfiling) {
				static bool collect = false;
				static u64 frame = 0;
				ImGui::Checkbox("Collect Data as .CSV", &collect);
				ImGui::TextColored(ImVec4(1, 1, 0, 1), "Entries Collected: %d", g_dataCollection.size());

				if (collect && ((frame & 63) < 1) && (g_dataCollection.size() < kEntriesToCollect))	//every 64 frames collect data
				{
					data_out d;
					//Record data
					d.frameTime = m_frameData.front();
					d.blur = m_blurSelect;
					d.ssao = m_ssaoSelect;
					d.sampler = m_samplerSelect;
					d.blurDS = m_blurTargetDownSize;
					d.ssaoDS = m_ssaoTargetDownSize;
					d.ssaoSamp = m_samples_mult;

					g_dataCollection.push_back(d);
				}
				++frame;
			}
#endif
		ImGui::End();


		// This function displays some useful debugging values, camera positions etc.
		DemoFeatures::editorHud(systems.pDebugDrawContext);

		//SSAO
		ImGui::TextColored(ImVec4(1, 1, 0, 1), "SSAO Shader Variables");
		if (ImGui::Button("Next Sampler"))
		{
			m_samplerSelect < kMaxSamplers - 1 ? ++m_samplerSelect : m_samplerSelect = 0;
		}
		ImGui::TextColored(ImVec4(1, 0, 1, 1), "Sampler: %s", m_samplerNames[m_samplerSelect].c_str());

		if (ImGui::Button("Next Technique"))
		{
			m_ssaoSelect < kMaxSSAOTypes - 1 ? ++m_ssaoSelect : m_ssaoSelect = 0;
		}
		ImGui::TextColored(ImVec4(0, 1, 1, 1), "Technique: %s", m_ssaoNames[m_ssaoSelect].c_str());

		ImGui::SliderFloat("Sample Radius", &m_sample_rad, 0.0f, 2.0f);
		ImGui::SliderFloat("Intensity", &m_intensity, 0.0f, 6.0f);
		ImGui::SliderFloat("Scale", &m_scale, 0.0f, 6.0f);
		ImGui::SliderFloat("Bias", &m_bias, 0.0f, 1.0f);
		ImGui::SliderInt("Samples", &m_samples_mult, 1, 16, "%.0f * 4");
		
		//Another value to play with for comparison with spiral kernel
		ImGui::SliderFloat("Max Distance", &m_maxDistance, 0.0f, 4.0f);

		ImGui::TextColored(ImVec4(1, 1, 0, 1), "PostFx Pipeline");
		if (ImGui::SliderInt("SSAO Target DownSize ^(n)", &m_ssaoTargetDownSize, 1, MAX_TARGET_DOWNSIZE))
		{
			create_ssao_resources(systems.pD3DDevice, systems.pD3DContext, systems.width / m_ssaoTargetDownSize, systems.height / m_ssaoTargetDownSize);
			create_ssao_downsample_viewport(systems.width / m_ssaoTargetDownSize, systems.height / m_ssaoTargetDownSize);
		}
		if (ImGui::SliderInt("Blur Target DownSize ^(n)", &m_blurTargetDownSize, 1, MAX_TARGET_DOWNSIZE))
		{
			create_postfx_resources(systems.pD3DDevice, systems.pD3DContext, systems.width / m_blurTargetDownSize, systems.height / m_blurTargetDownSize);
			create_blur_downsample_viewport(systems.width / m_blurTargetDownSize, systems.height / m_blurTargetDownSize);
		}

		//Blur
		ImGui::TextColored(ImVec4(1, 1, 0, 1), "Blur Shader Variables");
		ImGui::Checkbox("Blur ON", &m_blurOn);
		if (m_blurOn)
		{
			if (ImGui::Button("Next Blur"))
			{
				m_blurSelect < kMaxBlurs - 1 ? ++m_blurSelect : m_blurSelect = 0;
			}
			ImGui::TextColored(ImVec4(1, 0, 1, 1), "Blur: %s", m_blurNames[m_blurSelect].c_str());
		}

		ImGui::TextColored(ImVec4(1, 1, 0, 1), "Framework Variables");

		// Update Per Frame Data.
		// calculate view project and inverse so we can project back from depth buffer into world coordinates.
		m4x4 matViewProj = systems.pCamera->viewMatrix * systems.pCamera->projMatrix;
		m4x4 matInverseProj = systems.pCamera->projMatrix.Invert();
		m4x4 matInverseView = systems.pCamera->viewMatrix.Invert();

		m_perFrameCBData.m_matProjection = systems.pCamera->projMatrix.Transpose();
		m_perFrameCBData.m_matView = systems.pCamera->viewMatrix.Transpose();
		m_perFrameCBData.m_matViewProjection = matViewProj.Transpose();
		m_perFrameCBData.m_matInverseProjection = matInverseProj.Transpose();
		m_perFrameCBData.m_matInverseView = matInverseView.Transpose();

		m_perFrameCBData.m_time += 0.001f;

		// move our lights
		for (u32 i = 0; i < kLightGridSize; ++i)
		{
			for (u32 j = 0; j < kLightGridSize; ++j)
			{
				float control = (i*j+1)/(j + i+1);
				m_lights[i* kLightGridSize + j + 1].m_shaderInfo.m_vPosition = v4(
					i + sin(i * m_perFrameCBData.m_time) - 5.0
					, (cos(control * m_perFrameCBData.m_time) + 1) * 10
					, j + cos(j * m_perFrameCBData.m_time) - 5.0
					, 1.0f
				);
			}
		}
	}

	void on_render(SystemsInterface& systems) override
	{
		//////////////////////////////////////////////////////////////////////////
		// Imgui can also be used inside the render function.
		//////////////////////////////////////////////////////////////////////////


		//////////////////////////////////////////////////////////////////////////
		// You can use features from the DebugDrawlibrary.
		// Investigate the following functions for ideas.
		// see also : https://github.com/glampert/debug-draw
		//////////////////////////////////////////////////////////////////////////

		auto ctx = systems.pDebugDrawContext;
		dd::axisTriad(ctx, (const float*)& m4x4::Identity, 0.1f, 15.0f);

		// Push Per Frame Data to GPU
		D3D11_MAPPED_SUBRESOURCE subresource;
		if (!FAILED(systems.pD3DContext->Map(m_pPerFrameCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &subresource)))
		{
			memcpy(subresource.pData, &m_perFrameCBData, sizeof(PerFrameCBData));
			systems.pD3DContext->Unmap(m_pPerFrameCB, 0);
		}

		//=======================================================================================
		// The Geometry Pass.
		// Draw our scene into the GBuffer, capturing all the information we need for lighting.
		//=======================================================================================

		// Bind the G Buffer to the output merger
		// Here we are binding multiple render targets (MRT)
		systems.pD3DContext->OMSetRenderTargets(kMaxGBufferColourTargets, m_pGBufferTargetViews, m_pGBufferDepthView);

		// Clear colour and depth
		f32 clearValue[] = { 0.f, 0.f, 0.f, 0.f };
		systems.pD3DContext->ClearRenderTargetView(m_pGBufferTargetViews[kGBufferColourSpec], clearValue);
		f32 normalClearValue[] = { 0.5f, 0.5f, 0.5f, 0.f };
		systems.pD3DContext->ClearRenderTargetView(m_pGBufferTargetViews[kGBufferNormalPow], normalClearValue);
		systems.pD3DContext->ClearDepthStencilView(m_pGBufferDepthView, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.f, 0);

		// Bind Constant Buffers, to both PS and VS stages
		ID3D11Buffer* buffers[] = { m_pPerFrameCB, m_pPerDrawCB };
		systems.pD3DContext->VSSetConstantBuffers(0, 2, buffers);
		systems.pD3DContext->PSSetConstantBuffers(0, 2, buffers);

		// Bind a sampler state
		ID3D11SamplerState* samplers[] = { m_pSamplerState[m_samplerSelect] };
		systems.pD3DContext->PSSetSamplers(0, 1, samplers);

		// Opaque blend
		systems.pD3DContext->OMSetBlendState(m_pBlendStates[BlendStates::kOpaque], kBlendFactor, kSampleMask);

		// draw a plane
		{
			//No Texture Shader
			m_geometryNoTex.bind(systems.pD3DContext);

			m_plane.bind(systems.pD3DContext);

			for (int i(0); i < kRoomPlanes; ++i)
			{
				// Compute MVP matrix.
				m4x4 matMVP = m_mmRoomPlanes[i] * systems.pCamera->vpMatrix;

				// Update Per Draw Data
				m_perDrawCBData.m_matModel = m_mmRoomPlanes[i].Transpose();
				m_perDrawCBData.m_matMVP = matMVP.Transpose();

				// Push to GPU
				push_constant_buffer(systems.pD3DContext, m_pPerDrawCB, m_perDrawCBData);

				// Draw the mesh.
				m_plane.draw(systems.pD3DContext);
			}
		}

		// draw stanford dragon
		for (int i(0); i < 3; ++i)
		{
			m_s_dragon.bind(systems.pD3DContext);

			// Compute MVP matrix.
			m4x4 matModel = m4x4::CreateRotationY(degToRad(-135)) * m4x4::CreateTranslation(v3(i * 4, 0, i * 4));
			m4x4 matMVP = matModel * systems.pCamera->vpMatrix;

			// Update Per Draw Data
			m_perDrawCBData.m_matModel = matModel.Transpose();
			m_perDrawCBData.m_matMVP = matMVP.Transpose();

			// Push to GPU
			push_constant_buffer(systems.pD3DContext, m_pPerDrawCB, m_perDrawCBData);

			// Draw the mesh.
			m_s_dragon.draw(systems.pD3DContext);
		}

		//=======================================================================================
		// SSAO
		// Read the GBuffer textures, reconstruct depth and do AO
		//=======================================================================================

		FrameProfile frame;

		//Begin profiling for the technique
		if (m_enableProfiling)
		{
			begin_profile_frame(frame, systems.pD3DDevice, systems.pD3DContext);
		}

		//Move into ssao viewport dimensions
		systems.pD3DContext->RSSetViewports(1, &m_ssaoViewport);

		systems.pD3DContext->ClearRenderTargetView(m_pSSAORTV, clearValue);

		// Here we are binding the SSAO buffer as render target
		ID3D11RenderTargetView* views[] = { m_pSSAORTV, 0 };
		systems.pD3DContext->OMSetRenderTargets(2, views, NULL);

		systems.pD3DContext->OMSetBlendState(m_pBlendStates[BlendStates::kOpaque], kBlendFactor, kSampleMask);

		//Fill the CB
		m_SSAOCBData.g_sample_rad = m_sample_rad;
		m_SSAOCBData.g_intensity = m_intensity;
		m_SSAOCBData.g_scale = m_scale;
		m_SSAOCBData.g_bias = m_bias;
		m_SSAOCBData.random_size = 64.0f;
		m_SSAOCBData.g_samples = m_samples_mult;
		m_SSAOCBData.g_maxDistance = m_maxDistance;

		// Push Data to GPU
		D3D11_MAPPED_SUBRESOURCE sr;
		if (!FAILED(systems.pD3DContext->Map(m_pSSAOCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &sr)))
		{
			memcpy(sr.pData, &m_SSAOCBData, sizeof(SSAOCBData));
			systems.pD3DContext->Unmap(m_pSSAOCB, 0);
		}

		// Bind Constant Buffers, to both PS and VS stages
		ID3D11Buffer* ssaoBuffers[] = { m_pPerFrameCB, m_pSSAOCB, NULL };
		systems.pD3DContext->PSSetConstantBuffers(0, 2, ssaoBuffers);

		// Bind our GBuffer textures as inputs to the pixel shader
		systems.pD3DContext->PSSetShaderResources(0, 3, m_pGBufferTextureViews);

		// Bind a random normal map for help with sampling
		m_rndnrm.bind(systems.pD3DContext, ShaderStage::kPixel, 3);
		{
			m_SSAOShaders[m_ssaoSelect].bind(systems.pD3DContext);

			m_fullScreenQuad.bind(systems.pD3DContext);
			m_fullScreenQuad.draw(systems.pD3DContext);
		}

		//=======================================================================================
		// Blur Post FX
		// Read the SSAO texture, Blur the result in the same buffer
		//=======================================================================================

		//Move into blur texture viewport dimensions
		systems.pD3DContext->RSSetViewports(1, &m_blurViewport);

		m_BlurCBData.g_blurKernelSz = m_samples_mult;
		m_BlurCBData.g_blurSigma = m_blurSigma;
		m_BlurCBData.g_downsampleBlurFac = m_blurTargetDownSize;
		m_BlurCBData.g_kawaseIteration = 0;

		// Push Data to GPU
		D3D11_MAPPED_SUBRESOURCE sr_blur;
		if (!FAILED(systems.pD3DContext->Map(m_pBlurCBData, 0, D3D11_MAP_WRITE_DISCARD, 0, &sr_blur)))
		{
			memcpy(sr_blur.pData, &m_BlurCBData, sizeof(BlurCBData));
			systems.pD3DContext->Unmap(m_pBlurCBData, 0);
		}

		// Bind Constant Buffers, to both PS and VS stages
		ssaoBuffers[2] = { m_pBlurCBData };
		systems.pD3DContext->PSSetConstantBuffers(0, 3, ssaoBuffers);

		//use this to figure out which blur to use after ping-ponging
		int useBlurSRV = 1;

		if (m_blurOn)
		{
			switch (m_blurSelect)
			{
			case BlurType::kKawase:
				DoKawase(5, systems, sr_blur);
				break;
			case BlurType::kKawaseMedium:
				DoKawase(4, systems, sr_blur);
				break;
			case BlurType::kKawaseSmall:
				DoKawase(3, systems, sr_blur);
				break;
			case BlurType::kSlowGauss:
				systems.pD3DContext->ClearRenderTargetView(m_pBlurSSAORTV[1], clearValue);

				// Here we are binding the Blur RTV buffer as render target
				views[0] = m_pBlurSSAORTV[1];
				systems.pD3DContext->OMSetRenderTargets(2, views, NULL);

				// Bind our ssao texture as input to the pixel shader
				systems.pD3DContext->PSSetShaderResources(0, 1, &m_pSSAOSRV);

				{
					m_GaussBlur.bind(systems.pD3DContext);

					m_fullScreenQuad.bind(systems.pD3DContext);
					m_fullScreenQuad.draw(systems.pD3DContext);
				}
				break;

			case BlurType::kFastGauss:
			default:
				DoFastGauss(3, systems);
				break;
			}
		}

		//End the technique and try profile
		if (m_enableProfiling)
		{
			end_profile_frame(frame, systems.pD3DContext);
			profile_oldest_frame(systems.pD3DContext);
		}

		//=======================================================================================
		// The Lighting
		// Read the GBuffer textures, and "draw" light volumes for each of our lights.
		// We use additive blending on the result.
		//=======================================================================================

		//Back to fullscreen pass viewport
		systems.pD3DContext->RSSetViewports(1, &m_fsViewport);

		systems.pD3DContext->ClearRenderTargetView(systems.pSwapRenderTarget, clearValue);

		// Bind the swapchain target to the render target
		// Make sure to unbind other gbuffer targets and depth
		views[0] = systems.pSwapRenderTarget;
		systems.pD3DContext->OMSetRenderTargets(2, views, 0);

		// Bind our GBuffer textures & ssao buffer as inputs to the pixel shader
		systems.pD3DContext->PSSetShaderResources(0, 3, m_pGBufferTextureViews);

		if (m_blurOn)
		{
			//Set the blur view as resource for next pass
			systems.pD3DContext->PSSetShaderResources(3, 1, &m_pBlurSSAOSRV[useBlurSRV]);
		}
		else
		{
			//Direct bind the ssao buffer to the final lighting stage
			systems.pD3DContext->PSSetShaderResources(3, 1, &m_pSSAOSRV);
		}

		// Bind SSAO Debugging shader.
		static bool bDebugEnabled = false;
		ImGui::Checkbox("SSAO Buffer Debug", &bDebugEnabled);
		if (bDebugEnabled)
		{
			m_ssaoDebugShader.bind(systems.pD3DContext);

			// ... and draw a full screen quad.
			m_fullScreenQuad.bind(systems.pD3DContext);
			m_fullScreenQuad.draw(systems.pD3DContext);
		}
		else
		{
			// if we are not debugging the we bind the lighting shader and start accumulating light volumes.
			// bind the light constant buffer
			systems.pD3DContext->PSSetConstantBuffers(2, 1, &m_pLightInfoCB);

			static v4 tuneAtt(0.001f, 0.1f, 15.0f, 0.5f);
			ImGui::DragFloat4("Light Att", (float*)&tuneAtt, 0.0001, 5.0f);

			static int maxLights = m_lights.size();
			ImGui::SliderInt("Lights", &maxLights, 0, m_lights.size());


			for (u32 i = 0; i < (u32)maxLights; ++i)
			{
				auto& rLight(m_lights[i]);
				// For drawing a directional light which hits everywhere we draw a full screen quad.

				// Update and the light info constants.
				push_constant_buffer(systems.pD3DContext, m_pLightInfoCB, rLight.m_shaderInfo);

				switch (rLight.m_type)
				{
				case kLightType_Directional:
				{
					m_directionalLightShader.bind(systems.pD3DContext);
					m_fullScreenQuad.bind(systems.pD3DContext);
					m_fullScreenQuad.draw(systems.pD3DContext);

					// Additive blend so we accumulate for later lights
					systems.pD3DContext->OMSetBlendState(m_pBlendStates[BlendStates::kAdditive], kBlendFactor, kSampleMask);
				}
				break;
				case kLightType_Point:
				{
					m_pointLightShader.bind(systems.pD3DContext);

					// Compute Light MVP matrix.
					m4x4 matModel = m4x4::CreateScale(rLight.m_shaderInfo.m_vAtt.w);
					matModel *= m4x4::CreateTranslation(v3(rLight.m_shaderInfo.m_vPosition));
					m4x4 matMVP = matModel * systems.pCamera->vpMatrix;

					// Update Per Draw Data
					m_perDrawCBData.m_matMVP = matMVP.Transpose();
					push_constant_buffer(systems.pD3DContext, m_pPerDrawCB, m_perDrawCBData);

					m_lightVolumeSphere.bind(systems.pD3DContext);
					m_lightVolumeSphere.draw(systems.pD3DContext);
				}
				break;
				case kLightType_Spot:
					break;
				default:
					break;

				}
			}
		}
		
		//=======================================================================================
		// End all draws...
		//=======================================================================================

		// Unbind all the SRVs because we need them as targets next frame
		ID3D11ShaderResourceView* srvClear[] = { 0,0,0 };
		systems.pD3DContext->PSSetShaderResources(0, 3, srvClear);

		// re-bind depth for debugging output.
		systems.pD3DContext->OMSetRenderTargets(2, views, m_pGBufferDepthView);
	}

	void on_resize(SystemsInterface& systems) override
	{
		create_gbuffer(systems.pD3DDevice, systems.pD3DContext, systems.width, systems.height);
		create_postfx_resources(systems.pD3DDevice, systems.pD3DContext, systems.width / m_blurTargetDownSize, systems.height / m_blurTargetDownSize);
		create_ssao_resources(systems.pD3DDevice, systems.pD3DContext, systems.width / m_ssaoTargetDownSize, systems.height / m_ssaoTargetDownSize);
		create_blur_downsample_viewport(systems.width / m_blurTargetDownSize, systems.height / m_blurTargetDownSize);
		create_ssao_downsample_viewport(systems.width / m_ssaoTargetDownSize, systems.height / m_ssaoTargetDownSize);
	}

	void on_shutdown(SystemsInterface& systems) override
	{
#if COLLECT_DATA == 1
		//TO THE DATA FILE FOR TIMING ANALYSIS!
		std::fstream datalog;
		datalog.open("Analysis/datalog.csv", std::ios::in | std::ios::app);
		datalog.seekp(0, std::ios::end);

		int f = datalog.tellg();
		if (f == 0)  //if we have an empty file, add the col heads
		{
			datalog << "Technique Time (ms), SSAO, SSAO DownSample x, SSAO Num Samples, Blur, Blur DownSample x, Sampler Type\n";
		}

		for(const auto& d : g_dataCollection)
		{
		datalog << std::fixed << d.frameTime << ","
			<< m_ssaoNames[d.ssao] << ","
			<< d.ssaoDS << ","
			<< d.ssaoSamp * 4 << ","
			<< m_blurNames[d.blur] << ","
			<< d.blurDS << ","
			<< m_samplerNames[d.sampler]
#if defined _DEBUG
			<< ", Debug"
#else
			<< ", Release"
#endif
			<< std::endl;
		}
#endif
	}

private:

	enum EGBufferConstants
	{
		kGBufferColourSpec, // f16 Target, Albido Colour RGB + Specular Intensity.
		kGBufferNormalPow, // f16 Target Nsormal + Specular Power.
		kGBufferDepth, // f32 Depth Target.

		kMaxGBufferColourTargets = 2,
		kMaxGBufferTextures = 3
	};

	void create_gbuffer(ID3D11Device* pD3DDevice, ID3D11DeviceContext* pD3DContext, u32 width, u32 height)
	{
		HRESULT hr;

		// Release all outstanding references to the swap chain's buffers.
		pD3DContext->OMSetRenderTargets(0, 0, 0);

		// destroy old g-buffer views.
		SAFE_RELEASE(m_pGBufferDepthView);

		for (u32 i = 0; i < kMaxGBufferColourTargets; ++i)
		{
			SAFE_RELEASE(m_pGBufferTargetViews[i]);
		}

		// destroy old g-buffer textures.
		for (u32 i = 0; i < kMaxGBufferTextures; ++i)
		{
			SAFE_RELEASE(m_pGBufferTexture[i]);
			SAFE_RELEASE(m_pGBufferTextureViews[i]);
		}

		// Create a colour buffers
		for (u32 i = 0; i < kMaxGBufferColourTargets; ++i)
		{
			D3D11_TEXTURE2D_DESC desc;
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // 4 component f16 targets
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = 0;
			desc.MiscFlags = 0;

			hr = pD3DDevice->CreateTexture2D(&desc, NULL, &m_pGBufferTexture[i]);
			if (FAILED(hr))
			{
				panicF("Failed colour texture for GBuffer");
			}

			// render target views.
			hr = pD3DDevice->CreateRenderTargetView(m_pGBufferTexture[i], NULL, &m_pGBufferTargetViews[i]);
			if (FAILED(hr))
			{
				panicF("Failed colour target view for GBuffer");
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;

			hr = pD3DDevice->CreateShaderResourceView(m_pGBufferTexture[i], &srvDesc, &m_pGBufferTextureViews[i]);
			if (FAILED(hr))
			{
				panicF("Failed to create SRV of Target for GBuffer");
			}
		}

		// Create a depth buffer
		{
			D3D11_TEXTURE2D_DESC desc;
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R24G8_TYPELESS; // Typeless because we are binding as SRV and DepthStencilView
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = 0;
			desc.MiscFlags = 0;

			hr = pD3DDevice->CreateTexture2D(&desc, NULL, &m_pGBufferTexture[kGBufferDepth]);
			if (FAILED(hr))
			{
				panicF("Failed to create Depth Buffer for GBuffer");
			}

			D3D11_DEPTH_STENCIL_VIEW_DESC depthDesc = {};
			depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; // View suitable for writing depth
			depthDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			depthDesc.Texture2D.MipSlice = 0;

			hr = pD3DDevice->CreateDepthStencilView(m_pGBufferTexture[kGBufferDepth], &depthDesc, &m_pGBufferDepthView);
			if (FAILED(hr))
			{
				panicF("Failed to create Depth Stencil View for GBuffer");
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; // View suitable for decoding full 24bits of depth to red channel.
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;

			hr = pD3DDevice->CreateShaderResourceView(m_pGBufferTexture[kGBufferDepth], &srvDesc, &m_pGBufferTextureViews[kGBufferDepth]);
			if (FAILED(hr))
			{
				panicF("Failed to create SRV of Depth for GBuffer");
			}
		}

		//Create the full screen viewport
		m_fsViewport.Width = width;
		m_fsViewport.Height = height;
		m_fsViewport.TopLeftX = 0;
		m_fsViewport.TopLeftY = 0;
		m_fsViewport.MinDepth = 0;
		m_fsViewport.MaxDepth = 1;
	}

	void create_blur_downsample_viewport(float width, float height)
	{
		//Create the downsample screen viewport
		m_blurViewport.Width = width;
		m_blurViewport.Height = height;
		m_blurViewport.TopLeftX = 0;
		m_blurViewport.TopLeftY = 0;
		m_blurViewport.MinDepth = 0;
		m_blurViewport.MaxDepth = 1;
	}

	void create_ssao_downsample_viewport(float width, float height)
	{
		//Create the downsample screen viewport
		m_ssaoViewport.Width = width;
		m_ssaoViewport.Height = height;
		m_ssaoViewport.TopLeftX = 0;
		m_ssaoViewport.TopLeftY = 0;
		m_ssaoViewport.MinDepth = 0;
		m_ssaoViewport.MaxDepth = 1;
	}

	void create_postfx_resources(ID3D11Device* pD3DDevice, ID3D11DeviceContext* pD3DContext, u32 width, u32 height)
	{
		HRESULT hr;

		for (int i(0); i < 2; ++i)
		{
			SAFE_RELEASE(m_pBlurSSAORTV[i]);
			SAFE_RELEASE(m_pBlurSSAOTextures[i]);
			SAFE_RELEASE(m_pBlurSSAOSRV[i]);

			// Create a colour buffers
			D3D11_TEXTURE2D_DESC desc;
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // 4 component f16 targets
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = 0;
			desc.MiscFlags = 0;

			hr = pD3DDevice->CreateTexture2D(&desc, NULL, &m_pBlurSSAOTextures[i]);
			if (FAILED(hr))
			{
				panicF("Failed colour texture for PostFx");
			}

			// render target views.
			hr = pD3DDevice->CreateRenderTargetView(m_pBlurSSAOTextures[i], NULL, &m_pBlurSSAORTV[i]);
			if (FAILED(hr))
			{
				panicF("Failed colour target view for PostFx");
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;

			hr = pD3DDevice->CreateShaderResourceView(m_pBlurSSAOTextures[i], &srvDesc, &m_pBlurSSAOSRV[i]);
			if (FAILED(hr))
			{
				panicF("Failed to create SRV of Target for PostFx");
			}
		}
	}

	void create_ssao_resources(ID3D11Device* pD3DDevice, ID3D11DeviceContext* pD3DContext, u32 width, u32 height)
	{
		HRESULT hr;

		SAFE_RELEASE(m_pSSAORTV);
		SAFE_RELEASE(m_pSSAOTexture);
		SAFE_RELEASE(m_pSSAOSRV);

		// Create a colour buffers
		D3D11_TEXTURE2D_DESC desc;
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R16_FLOAT; // 1 component f16 target
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;	//Allows mip generation with context->GenerateMips()

		hr = pD3DDevice->CreateTexture2D(&desc, NULL, &m_pSSAOTexture);
		if (FAILED(hr))
		{
			panicF("Failed colour texture for SSAO");
		}

		// render target views.
		hr = pD3DDevice->CreateRenderTargetView(m_pSSAOTexture, NULL, &m_pSSAORTV);
		if (FAILED(hr))
		{
			panicF("Failed colour target view for SSAO");
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;

		hr = pD3DDevice->CreateShaderResourceView(m_pSSAOTexture, &srvDesc, &m_pSSAOSRV);
		if (FAILED(hr))
		{
			panicF("Failed to create SRV of Target for SSAO");
		}
	}

	//Blurs
	void DoKawase(int iterations, SystemsInterface& systems, D3D11_MAPPED_SUBRESOURCE& blurBuffer)
	{
		int useBlurSRV = 1;	//which SRV to use in ping-ponging the blur targets
		f32 clearValue[] = { 0.f, 0.f, 0.f, 0.f };	//clear rtv to...
		ID3D11RenderTargetView* views[] = { 0, 0 };

		for (int i(0); i < iterations; ++i)
		{
			m_BlurCBData.g_kawaseIteration = i;

			// Push Data to GPU
			if (!FAILED(systems.pD3DContext->Map(m_pBlurCBData, 0, D3D11_MAP_WRITE_DISCARD, 0, &blurBuffer)))
			{
				memcpy(blurBuffer.pData, &m_BlurCBData, sizeof(BlurCBData));
				systems.pD3DContext->Unmap(m_pBlurCBData, 0);
			}

			//set the blur data constant buffer
			systems.pD3DContext->PSSetConstantBuffers(2, 1, &m_pBlurCBData);

			//select Target
			(i % 2 == 0) ?
				systems.pD3DContext->ClearRenderTargetView(m_pBlurSSAORTV[1], clearValue) :
				systems.pD3DContext->ClearRenderTargetView(m_pBlurSSAORTV[0], clearValue);

			// Here we are binding the Blur RTV buffer as render target
			views[0] = (i % 2 == 0) ? m_pBlurSSAORTV[1] : m_pBlurSSAORTV[0];
			systems.pD3DContext->OMSetRenderTargets(2, views, NULL);

			// Bind our ssao texture as input to the pixel shader
			if (i != 0)
			{
				(i % 2 == 0) ?
					systems.pD3DContext->PSSetShaderResources(0, 1, &m_pBlurSSAOSRV[0]) :
					systems.pD3DContext->PSSetShaderResources(0, 1, &m_pBlurSSAOSRV[1]);
				(i % 2 == 0) ?
					useBlurSRV = 1 :
					useBlurSRV = 0;
			}
			else
			{
				systems.pD3DContext->PSSetShaderResources(0, 1, &m_pSSAOSRV);
			}

			//do the blur pass
			{
				m_kawase.bind(systems.pD3DContext);

				m_fullScreenQuad.bind(systems.pD3DContext);
				m_fullScreenQuad.draw(systems.pD3DContext);
			}
		}

		//unbind for safety
		views[0] = 0;
		systems.pD3DContext->OMSetRenderTargets(2, views, NULL);
	}

	void DoFastGauss(int iterations, SystemsInterface& systems)
	{
		ID3D11RenderTargetView* views[] = { 0, 0 };
		f32 clearValue[] = { 0.f, 0.f, 0.f, 0.f };	//clear rtv to...

		for (int i(0); i < iterations; ++i)
		{
			//X
			systems.pD3DContext->ClearRenderTargetView(m_pBlurSSAORTV[0], clearValue);

			// Here we are binding the Blur RTV buffer as render target
			views[0] = m_pBlurSSAORTV[0];
			systems.pD3DContext->OMSetRenderTargets(2, views, NULL);

			// Bind our ssao texture as input to the pixel shader
			if (i != 0)
			{
				systems.pD3DContext->PSSetShaderResources(0, 1, &m_pBlurSSAOSRV[1]);
			}
			else
			{
				systems.pD3DContext->PSSetShaderResources(0, 1, &m_pSSAOSRV);
			}

			{
				m_GaussX.bind(systems.pD3DContext);

				m_fullScreenQuad.bind(systems.pD3DContext);
				m_fullScreenQuad.draw(systems.pD3DContext);
			}

			//Y
			systems.pD3DContext->ClearRenderTargetView(m_pBlurSSAORTV[1], clearValue);

			// Here we are binding the Blur RTV buffer as render target
			views[0] = m_pBlurSSAORTV[1];
			systems.pD3DContext->OMSetRenderTargets(2, views, NULL);

			// Bind our ssao texture as input to the pixel shader
			systems.pD3DContext->PSSetShaderResources(0, 1, &m_pBlurSSAOSRV[0]);

			{
				m_GaussY.bind(systems.pD3DContext);

				m_fullScreenQuad.bind(systems.pD3DContext);
				m_fullScreenQuad.draw(systems.pD3DContext);
			}
		}

		//unbind for safety
		views[0] = 0;
		systems.pD3DContext->OMSetRenderTargets(2, views, NULL);
	}

	//Profiling
	void init_profile_queue()
	{
		m_profilingDataQueue.empty();
		m_frameData.empty();
		m_frameData.push(0.0f);
	}

	void begin_profile_frame(FrameProfile& data, ID3D11Device* pD3DDevice, ID3D11DeviceContext* pD3DContext)
	{
		HRESULT res;
		//create and start profiling a frame
		D3D11_QUERY_DESC queryDesc;
		ZeroMemory(&queryDesc, sizeof(queryDesc));

		queryDesc.Query = D3D11_QUERY_TIMESTAMP;
		res = pD3DDevice->CreateQuery(&queryDesc, &data.startTime);
		res = pD3DDevice->CreateQuery(&queryDesc, &data.endTime);
		queryDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
		res = pD3DDevice->CreateQuery(&queryDesc, &data.disjoint);

		//Begin events
		pD3DContext->Begin(data.disjoint);
		pD3DContext->End(data.startTime);
	}

	void end_profile_frame(FrameProfile& data, ID3D11DeviceContext* pD3DContext)
	{
		//push frame to queue after ending the event
		pD3DContext->End(data.endTime);
		pD3DContext->End(data.disjoint);

		//try immediate profiling		
		if (S_OK != pD3DContext->GetData(data.disjoint, &data.dData, sizeof(data.dData), 0) ||
			S_OK != pD3DContext->GetData(data.startTime, &data.sData, sizeof(data.sData), 0) ||
			S_OK != pD3DContext->GetData(data.endTime, &data.eData, sizeof(data.eData), 0))
		{
			add_to_profile_queue(data);
		}
		else
		{
			//profile
			get_profile_timings(data);
		}
	}

	void add_to_profile_queue(FrameProfile& data)
	{
		m_profilingDataQueue.push(data);
	}

	void profile_oldest_frame(ID3D11DeviceContext* pD3DContext)
	{
		FrameProfile* pFrame = &m_profilingDataQueue.front();

		if (S_OK != pD3DContext->GetData(pFrame->disjoint, &pFrame->dData, sizeof(pFrame->dData), 0) ||
			S_OK != pD3DContext->GetData(pFrame->startTime, &pFrame->sData, sizeof(pFrame->sData), 0) ||
			S_OK != pD3DContext->GetData(pFrame->endTime, &pFrame->eData, sizeof(pFrame->eData), 0))
		{
			//skip and try again next time
		}
		else
		{ 
			//profile
			get_profile_timings(*pFrame);
			m_profilingDataQueue.pop();
		}
	}

	void get_profile_timings(FrameProfile& data)
	{
		//do timings and push to the visual data queue
		float d;

		if (!data.dData.Disjoint)
		{
			u64 Delta = data.eData - data.sData;
			float Frequency = static_cast<float>(data.dData.Frequency);
			d = (Delta / Frequency) * 1000.0f;
		}

		//pop oldest timing...
		if (m_frameData.size() > MAX_FRAMES_FOR_PROFILE_QUEUE)
		{
			m_frameData.pop();
		}

		//push newest...
		m_frameData.push(d);
	}

private:

	enum BlendStates
	{
		kOpaque,
		kAdditive,
		kMaxBlendStates
	};
	ID3D11BlendState* m_pBlendStates[kMaxBlendStates];

	PerFrameCBData m_perFrameCBData;
	ID3D11Buffer* m_pPerFrameCB = nullptr;

	PerDrawCBData m_perDrawCBData;
	ID3D11Buffer* m_pPerDrawCB = nullptr;


	std::vector<Light> m_lights;
	ID3D11Buffer* m_pLightInfoCB = nullptr;


	ShaderSet m_geometryPassShader;
	ShaderSet m_geometryNoTex;
	ShaderSet m_directionalLightShader;
	ShaderSet m_pointLightShader;
	ShaderSet m_ssaoDebugShader;

	// Scene related objects
	Mesh m_meshArray[2];
	Texture m_textureArray[2];

	//Samplers
	enum SamplerType {
		kLinear = 0,
		kAniso,
		kPoint,
		kBiLinear,
		kMaxSamplers
	};
	std::string m_samplerNames[kMaxSamplers] = {
		"Linear",
		"Anisotropic",
		"Point",
		"BiLinear"
	};
	ID3D11SamplerState* m_pSamplerState[kMaxSamplers] = { nullptr };
	u16 m_samplerSelect = kBiLinear;

	//Room Resources
	Mesh m_plane;
	m4x4 m_mmRoomPlanes[kRoomPlanes];

	//Cool Meshes
	Mesh m_s_dragon;

	// Screen quad : for deferred passes
	Mesh m_fullScreenQuad;
	Mesh m_lightVolumeSphere;

	//SSAO Shader Resources
	enum SSAOType {
		kStandardSSAO = 0,
		kSpiralSSAO,
		KGPUZENAlchemy,
		kMaxSSAOTypes
	};
	std::string m_ssaoNames[kMaxSSAOTypes] = {
		"Default Technique",
		"Spiral Kernel",
		"GPU ZEN: Alchemy Spiral"
	};
	u16 m_ssaoSelect = 0;
	Texture m_rndnrm;
	ShaderSet m_SSAOShaders[kMaxSSAOTypes];
	ShaderSet m_GaussBlur;

	//FastBlurs
	ShaderSet m_GaussX;
	ShaderSet m_GaussY;
	ShaderSet m_kawase;

	BlurCBData m_BlurCBData;
	ID3D11Buffer* m_pBlurCBData = nullptr;

	enum BlurType {
		kSlowGauss = 0,
		kFastGauss,
		kKawase,
		kKawaseSmall,
		kKawaseMedium,
		kMaxBlurs
	};
	std::string m_blurNames[kMaxBlurs] = {
		"Slow Gaussian: 25 Sample",
		"Fast Gaussian: 9 tap using 5 texel fetches XY",
		"Kawase LRG: 0, 1, 2, 2, 3",
		"Kawase SML: 0, 1, 1",
		"Kawase MED: 0, 1, 1, 2"
	};
	u16 m_blurSelect = 0;

	SSAOCBData m_SSAOCBData;
	ID3D11Buffer* m_pSSAOCB = nullptr;
	
	//SSAO Vars
	float m_random_size;

	//Some Defaults
	float m_sample_rad = 0.1;
	float m_intensity = 2.0;
	float m_scale = 0.121;
	float m_bias = 0.01;
	int m_samples_mult = 2;
	float m_maxDistance = 2.0;

	//Blur vars
	int m_blurKernel = 5;
	float m_blurSigma = 7.0f;
	bool m_blurOn = true;

	int m_blurTargetDownSize = 1;
	int m_ssaoTargetDownSize = 1;

	D3D11_VIEWPORT m_blurViewport;
	D3D11_VIEWPORT m_ssaoViewport;
	D3D11_VIEWPORT m_fsViewport;

	ID3D11Texture2D*			m_pSSAOTexture = nullptr;
	ID3D11RenderTargetView*		m_pSSAORTV = nullptr;
	ID3D11ShaderResourceView*	m_pSSAOSRV = nullptr;

	//PostFx -- Blurred SSAO Buffer
	ID3D11Texture2D*			m_pBlurSSAOTextures[2] = { nullptr, nullptr };
	ID3D11RenderTargetView*		m_pBlurSSAORTV[2] = { nullptr, nullptr };
	ID3D11ShaderResourceView*	m_pBlurSSAOSRV[2] = { nullptr, nullptr };

	// GBuffer objects
	ID3D11Texture2D*		m_pGBufferTexture[kMaxGBufferTextures];
	ID3D11RenderTargetView* m_pGBufferTargetViews[kMaxGBufferColourTargets];
	ID3D11DepthStencilView* m_pGBufferDepthView;
	ID3D11ShaderResourceView* m_pGBufferTextureViews[kMaxGBufferTextures];

	v3 m_position;
	f32 m_size;

	//Profiling
	bool m_enableProfiling = true;
	std::queue<FrameProfile> m_profilingDataQueue;
	std::queue<float> m_frameData;
	float test_data[MAX_FRAMES_FOR_PROFILE_QUEUE] = { 0 };
};

SSAOApp g_app;

FRAMEWORK_IMPLEMENT_MAIN(g_app, "SSAO")
