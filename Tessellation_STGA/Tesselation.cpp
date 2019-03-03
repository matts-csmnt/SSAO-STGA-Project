#include "Framework.h"

#include "ShaderSet.h"
#include "Mesh.h"
#include "Texture.h"

//================================================================================
// Normal Mapping Application
// An example of how to work with normal maps.
//================================================================================
class TesselationDemo : public FrameworkApp
{
public:

	struct PerFrameCBData
	{
		m4x4 m_matProjection;
		m4x4 m_matView;
		v4 m_lightPos;
		f32		m_time;
		f32     m_padding[3];
	};

	struct PerDrawCBData
	{
		m4x4 m_matMVP;
		m4x4 m_matWorld;
		v4 m_matNormal[3]; // because of structure packing rules this represents a float3x3 in HLSL.
	};

	void on_init(SystemsInterface& systems) override
	{
		m_position = v3(0.5f, 0.5f, 0.5f);
		m_size = 1.0f;
		systems.pCamera->eye = v3(10.f, 5.f, 7.f);
		systems.pCamera->look_at(v3(3.f, 0.5f, 0.f));

		// compile a set of shaders
		m_meshShader.init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("Assets/Shaders/NormalMappingShaders.fx", "VS_Mesh", "PS_Mesh")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);

		// Create Per Frame Constant Buffer.
		m_pPerFrameCB = create_constant_buffer<PerFrameCBData>(systems.pD3DDevice);

		// Create Per Frame Constant Buffer.
		m_pPerDrawCB = create_constant_buffer<PerDrawCBData>(systems.pD3DDevice);

		// Initialize a mesh directly.
		create_mesh_cube(systems.pD3DDevice, m_cube, 0.5f);
		create_mesh_quad_xy(systems.pD3DDevice, m_floorPlane, 50.f);

		// Initialise some textures;
		m_textures[0].init_from_dds(systems.pD3DDevice, "Assets/Textures/Terrain/mask_with_height.dds");
		m_textures[1].init_from_dds(systems.pD3DDevice, "Assets/Textures/Terrain/NormalMap.dds");

		// We need a sampler state to define wrapping and mipmap parameters.
		m_pLinearMipSamplerState = create_basic_sampler(systems.pD3DDevice, D3D11_TEXTURE_ADDRESS_WRAP);

		//init skybox
		m_skyboxShaders.init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("Assets/Shaders/SkyboxShaders.fx", "VS_Skybox", "PS_Skybox")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);
		m_skyboxCubeMap.init_from_dds(systems.pD3DDevice, "Assets/Skyboxes/CloudsCubeMap.dds");

		// Setup per-frame data
		m_perFrameCBData.m_time = 0.0f;
	}

	void on_update(SystemsInterface& systems) override
	{
		//////////////////////////////////////////////////////////////////////////
		// You can use features from the ImGui library.
		// Investigate the ImGui::ShowDemoWindow() function for ideas.
		// see also : https://github.com/ocornut/imgui
		//////////////////////////////////////////////////////////////////////////

		// This function displays some useful debugging values, camera positions etc.
		DemoFeatures::editorHud(systems.pDebugDrawContext);

		ImGui::SliderFloat3("Position", (float*)&m_position, -1.f, 1.f);
		ImGui::SliderFloat("Size", &m_size, 0.1f, 10.f);

		//Demo Options
		ImGui::Checkbox("Wireframe Pass On", &m_wireframe_pass);
		ImGui::Checkbox("Debug Draw On", &m_debugDraw);

		// Update Per Frame Data.
		m_perFrameCBData.m_matProjection = systems.pCamera->projMatrix.Transpose();
		m_perFrameCBData.m_matView = systems.pCamera->viewMatrix.Transpose();
		m_perFrameCBData.m_time += 0.001f;
		m_perFrameCBData.m_lightPos = v4(sin(m_perFrameCBData.m_time*5.0f) * 4.f + 3.0f, 1.f, 1.f, 0.f);
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

		if (m_debugDraw)
		{
			// Grid from -50 to +50 in both X & Z
			auto ctx = systems.pDebugDrawContext;

			dd::xzSquareGrid(ctx, -50.0f, 50.0f, 0.0f, 1.f, dd::colors::DimGray);
			dd::axisTriad(ctx, (const float*)& m4x4::Identity, 0.1f, 15.0f);
			dd::cross(ctx, (const float*)& m_perFrameCBData.m_lightPos, 0.1f, 15.0f);
		}
		// Push Per Frame Data to GPU
		push_constant_buffer(systems.pD3DContext, m_pPerFrameCB, m_perFrameCBData);

		// Bind our set of shaders.
		m_meshShader.bind(systems.pD3DContext);

		// Bind Constant Buffers, to both PS and VS stages
		ID3D11Buffer* buffers[] = { m_pPerFrameCB, m_pPerDrawCB };
		systems.pD3DContext->VSSetConstantBuffers(0, 2, buffers);
		systems.pD3DContext->PSSetConstantBuffers(0, 2, buffers);

		// Bind a sampler state
		ID3D11SamplerState* samplers[] = { m_pLinearMipSamplerState };
		systems.pD3DContext->PSSetSamplers(0, 1, samplers);


		constexpr f32 kGridSpacing = 1.5f;
		constexpr u32 kNumInstances = 5;
		constexpr u32 kNumModelTypes = 2;

		//Draw terrain plane
		{
			// Bind a mesh and texture.
			m_floorPlane.bind(systems.pD3DContext);
			m_textures[0].bind(systems.pD3DContext, ShaderStage::kPixel, 0);
			m_textures[1].bind(systems.pD3DContext, ShaderStage::kPixel, 1);

			// Compute MVP matrix. -- 90 deg x rot * origin
			m4x4 matWorld = m4x4::CreateRotationX(1.5708f) * m4x4::CreateTranslation(v3(0.0f, 0.0f, 0.f));
			m4x4 matMVP = matWorld * systems.pCamera->vpMatrix;

			// Update Per Draw Data
			m_perDrawCBData.m_matMVP = matMVP.Transpose();
			m_perDrawCBData.m_matWorld = matWorld.Transpose();

			// Inverse transpose,  but since we didn't do any shearing or non-uniform scaling then we simple grab the upper 3x3 in the shader.
			pack_upper_float3x3(m_perDrawCBData.m_matWorld, m_perDrawCBData.m_matNormal);

			// Push to GPU
			push_constant_buffer(systems.pD3DContext, m_pPerDrawCB, m_perDrawCBData);

			// Draw the mesh.
			m_floorPlane.draw(systems.pD3DContext);
		}

		//Debug wireframe pass
		if(m_wireframe_pass)
		{
			// Bind a mesh.
			//m_floorPlane.bind(systems.pD3DContext);

			//set topology
			systems.pD3DContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);

			ID3D11Buffer* buffers[] = { m_floorPlane.vertex_buffer_notconst() };
			UINT strides[] = { sizeof(MeshVertex) };
			UINT offsets[] = { 0 };
			systems.pD3DContext->IASetVertexBuffers(0, 1, buffers, strides, offsets);

			if (m_floorPlane.index_buffer())
			{
				systems.pD3DContext->IASetIndexBuffer(m_floorPlane.index_buffer_notconst(), DXGI_FORMAT_R16_UINT, 0);
			}

			// Compute MVP matrix. -- 90 deg x rot * origin + shift up
			m4x4 matWorld = m4x4::CreateRotationX(1.5708f) * m4x4::CreateTranslation(v3(0.0f, 0.001f, 0.f));
			m4x4 matMVP = matWorld * systems.pCamera->vpMatrix;

			// Update Per Draw Data
			m_perDrawCBData.m_matMVP = matMVP.Transpose();
			m_perDrawCBData.m_matWorld = matWorld.Transpose();

			// Inverse transpose,  but since we didn't do any shearing or non-uniform scaling then we simple grab the upper 3x3 in the shader.
			pack_upper_float3x3(m_perDrawCBData.m_matWorld, m_perDrawCBData.m_matNormal);

			// Push to GPU
			push_constant_buffer(systems.pD3DContext, m_pPerDrawCB, m_perDrawCBData);

			// Draw the mesh.
			m_floorPlane.draw(systems.pD3DContext);
		}

		// Draw the sky box.
		{

			// Bind our set of shaders.
			m_skyboxShaders.bind(systems.pD3DContext);

			m4x4 matView = systems.pCamera->viewMatrix;
			matView.m[3][0] = 0.f;
			matView.m[3][1] = 0.f;
			matView.m[3][2] = 0.f;
			matView.m[3][3] = 1.f;

			m4x4 matMVP = matView * systems.pCamera->projMatrix;
			m_perDrawCBData.m_matMVP = matMVP.Transpose();
			push_constant_buffer(systems.pD3DContext, m_pPerDrawCB, m_perDrawCBData);


			// Bind the texture
			{
				m_skyboxCubeMap.bind(systems.pD3DContext, ShaderStage::kPixel, 0);
			}

			// Bind a sampler state
			{
				ID3D11SamplerState* samplers[] = { m_pLinearMipSamplerState };
				systems.pD3DContext->PSSetSamplers(0, 1, samplers);
			}

			// draw the box at camera center.
			// shader does the rest.
			m_cube.bind(systems.pD3DContext);
			m_cube.draw(systems.pD3DContext);
		}
	}

	void on_resize(SystemsInterface& ) override
	{
		
	}

private:

	PerFrameCBData m_perFrameCBData;
	ID3D11Buffer* m_pPerFrameCB = nullptr;

	PerDrawCBData m_perDrawCBData;
	ID3D11Buffer* m_pPerDrawCB = nullptr;

	ShaderSet m_meshShader;
	
	Mesh m_meshArray[2];
	Texture m_textures[2];
	ID3D11SamplerState* m_pLinearMipSamplerState = nullptr;

	v3 m_position;
	f32 m_size;

	//Tesselation Assets
	Mesh m_floorPlane;
	bool m_wireframe_pass = false;

	//Skybox
	ShaderSet	m_skyboxShaders;
	Mesh		m_cube;
	Texture		m_skyboxCubeMap;

	//DebugDraw
	bool m_debugDraw = false;
};

TesselationDemo g_app;

FRAMEWORK_IMPLEMENT_MAIN(g_app, "Tesselation Demo")
