#pragma once
#include "Framework.h"
#include "Mesh.h"

#include <assimp/Importer.hpp>      // C++ importer interface
#include <assimp/scene.h>			// Output data structure
#include <assimp/postprocess.h>		// Post processing flags
#include <vector>

//Adapted from - https://github.com/DanielJHart/AssetLoaderDirectX/
//With permission from Dan Hart.

using MeshVertex = Vertex_Pos3fColour4ubNormal3fTangent3fTex2f; // vertex type

bool create_mesh_from_fbx(ID3D11Device* pDevice, Mesh& meshOut, const std::string& pFile)
{
	// Create an instance of the Importer class
	Assimp::Importer importer;

	// And have it read the given file with some example postprocessing
	// Usually - if speed is not the most important aspect for you - you'll 
	// propably to request more postprocessing than we do in this example.
	const aiScene* scene = importer.ReadFile(pFile,
		aiProcess_GenSmoothNormals |
		aiProcess_MakeLeftHanded |			// This is for DirectX
		aiProcess_Triangulate |
		aiProcess_JoinIdenticalVertices |
		aiProcess_GenUVCoords |
		aiProcess_FlipUVs |
		aiProcess_SortByPType);

	// If the import failed, report it
	if (!scene)
	{
		// Log an error
		panicF("Could not load scene.");
		return false;
	}

	std::vector<MeshVertex> vertices;
	std::vector<u16> indices;
	int indexOffset = 0;

	if (scene->HasMeshes())
	{
		for (int i = 0; i < scene->mNumMeshes; ++i)
		{
			aiMesh* mesh = scene->mMeshes[i];

			for (int vertex = 0; vertex < mesh->mNumVertices; ++vertex)
			{
				auto vert = mesh->mVertices[vertex];
				auto normal = mesh->mNormals[vertex].Normalize();
				auto texCoord = mesh->mTextureCoords[0][vertex];

				vertices.push_back(MeshVertex(DirectX::XMFLOAT3(vert.x, vert.y, vert.z), //DirectX::XMFLOAT3(vert.x * 0.1f, vert.y * 0.1f, vert.z * 0.1f),
					VertexColour(0xffffffff),
					DirectX::XMFLOAT3(normal.x, normal.y, normal.z),
					DirectX::XMFLOAT2(texCoord.x, texCoord.y)));
			}

			if (mesh->HasFaces())
			{
				for (int f = 0; f < mesh->mNumFaces; ++f)
				{
					aiFace face = mesh->mFaces[f];

					for (int index = 0; index < face.mNumIndices; ++index)
					{
						indices.push_back(face.mIndices[index] + indexOffset);
					}
				}

				indexOffset += mesh->mNumVertices;
			}
			else
			{
				// Log - No faces found on mesh.
			}
		}
	}
	else
	{
		panicF("No mesh found in file.");
		return false;
	}

	meshOut.init_buffers(pDevice, &vertices[0], vertices.size(), &indices[0], indices.size());
}