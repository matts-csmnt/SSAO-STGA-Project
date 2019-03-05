#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>	//yikes!
#include <stdio.h>
#include <cstdint>
#include "Framework.h"
#include "Mesh.h"

namespace Heightmap {

	static const u32 MAP_COLOUR = 0xFF800000;
	static const v2 test_uvs = v2(0, 0);

	//////////////////////////////////////////////////////////////////////
	// LoadHeightMap
	// Original code sourced from rastertek.com
	//////////////////////////////////////////////////////////////////////
	bool LoadHeightMap(ID3D11Device* device, Mesh* output, char* filename, float gridSize)
	{
		FILE* filePtr;
		int error;
		unsigned int count;
		BITMAPFILEHEADER bitmapFileHeader;
		BITMAPINFOHEADER bitmapInfoHeader;
		int imageSize, i, j, k, index;
		unsigned char* bitmapImage;
		unsigned char height;


		// Open the height map file in binary.
		error = fopen_s(&filePtr, filename, "rb");
		if (error != 0)
		{
			return false;
		}

		// Read in the file header.
		count = fread(&bitmapFileHeader, sizeof(BITMAPFILEHEADER), 1, filePtr);
		if (count != 1)
		{
			return false;
		}

		// Read in the bitmap info header.
		count = fread(&bitmapInfoHeader, sizeof(BITMAPINFOHEADER), 1, filePtr);
		if (count != 1)
		{
			return false;
		}

		// Save the dimensions of the terrain.
		u32 HeightMapWidth = bitmapInfoHeader.biWidth;
		u32 HeightMapLength = bitmapInfoHeader.biHeight;

		// Calculate the size of the bitmap image data.
		imageSize = HeightMapWidth * HeightMapLength * 3;

		// Allocate memory for the bitmap image data.
		bitmapImage = new unsigned char[imageSize];
		if (!bitmapImage)
		{
			return false;
		}

		// Move to the beginning of the bitmap data.
		fseek(filePtr, bitmapFileHeader.bfOffBits, SEEK_SET);

		// Read in the bitmap image data.
		count = fread(bitmapImage, 1, imageSize, filePtr);
		if (count != imageSize)
		{
			return false;
		}

		// Close the file.
		error = fclose(filePtr);
		if (error != 0)
		{
			return false;
		}

		// Create the structure to hold the height map data.
		v3* pHeightMap = new v3[HeightMapWidth * HeightMapLength];
		if (!pHeightMap)
		{
			return false;
		}

		// Initialize the position in the image data buffer.
		k = 0;

		// Read the image data into the height map.
		for (j = 0; j < HeightMapLength; j++)
		{
			for (i = 0; i < HeightMapWidth; i++)
			{
				height = bitmapImage[k];

				index = (HeightMapLength * j) + i;

				pHeightMap[index].x = (float)(i - (HeightMapWidth / 2))*gridSize;
				pHeightMap[index].y = (float)height / 16 * gridSize;
				pHeightMap[index].z = (float)(j - (HeightMapLength / 2))*gridSize;

				k += 3;
			}
		}

		// Release the bitmap image data.
		delete[] bitmapImage;
		bitmapImage = 0;

		/////////////////////////////////////////////////////////////////
		// Begin construction of the buffers & precalc normals
		/////////////////////////////////////////////////////////////////

		//TRIANGLE STRIP
		u32 HeightMapVtxCount = (HeightMapLength) * (HeightMapWidth * 2) + (HeightMapLength - 2);
		//m_HeightMapVtxCount = HeightMapLength * HeightMapWidth * 2;

		MeshVertex* m_pMapVtxs = new MeshVertex[HeightMapVtxCount];
		v3 normalOne, normalTwo;

		int vertexNo;
		vertexNo = 0;

		//Pre calculate Normals (might need 8 directions for normals to be shaded fully?)
		int bufferSize = HeightMapLength * HeightMapWidth;
		v3 initValue = { 0,1,0 };
		v3* normalStorage = new v3[bufferSize];
		for (int i = 0; i < bufferSize; ++i)
		{
			normalStorage[i] = initValue;
		}

		for (int yVtx = 0; yVtx < HeightMapLength - 1; ++yVtx)
		{
			for (int xVtx = 0; xVtx < HeightMapWidth; ++xVtx)
			{
				//check if points above, left, right, below are there

				// for each pHeightMap[idx] check for surrounding points and get avg normal
				int currentPoint = (yVtx * HeightMapWidth) + xVtx;
				int pointDirection[8] = { -1,-1,-1,-1,-1,-1,-1,-1 };

				//pHeightMap[(yVtx * HeightMapWidth) + xVtx]

				//check up
				if (currentPoint - HeightMapWidth >= 0)
				{
					pointDirection[0] = currentPoint - HeightMapWidth;
				}

				//check nw
				if (currentPoint - (HeightMapWidth - 1) >= 0 && (currentPoint - 1 % HeightMapWidth) > 0)
				{
					pointDirection[1] = currentPoint - (HeightMapWidth + 1);
				}

				//check left
				if (currentPoint - 1 >= 0 && (currentPoint % HeightMapWidth) > 0)
				{
					pointDirection[2] = currentPoint - 1;
				}

				//check sw
				if (currentPoint + (HeightMapWidth - 1) >= 0 && currentPoint + HeightMapWidth - 1 < bufferSize && (currentPoint + HeightMapWidth - 1 % HeightMapWidth) > 0)
				{
					pointDirection[3] = currentPoint - (HeightMapWidth + 1);
				}

				//check below
				if (currentPoint + HeightMapWidth >= 0 && currentPoint + HeightMapWidth < bufferSize)
				{
					pointDirection[4] = currentPoint + HeightMapWidth;
				}

				//check se
				if (currentPoint + (HeightMapWidth + 1) < bufferSize && (currentPoint % HeightMapWidth) > 0 && (currentPoint + 1 + HeightMapWidth % HeightMapWidth) <= HeightMapWidth - 1)
				{
					pointDirection[5] = currentPoint - (HeightMapWidth + 1);
				}

				//check right
				if (currentPoint + 1 < bufferSize && (currentPoint + 1 % HeightMapWidth) <= HeightMapWidth - 1)
				{
					pointDirection[6] = currentPoint + 1;
				}

				//check ne
				if (currentPoint - (HeightMapWidth + 1) < bufferSize && (currentPoint + 1 - HeightMapWidth % HeightMapWidth) <= HeightMapWidth - 1)
				{
					pointDirection[7] = currentPoint - (HeightMapWidth + 1);
				}

				//if point(direction) is > -1 calculate vectors to current point
				int numOfNorms = 0;
				v3 vectors[8];
				for (int i = 0; i < 8; ++i)
				{
					vectors[i] = v3(0, 0, 0);
				}

				for (int j = 0; j < 8; ++j)
				{
					if (pointDirection[j] >= 0 && pointDirection[j] < bufferSize)
					{
						v3 vec = { pHeightMap[pointDirection[j]].x - pHeightMap[currentPoint].x,
								   pHeightMap[pointDirection[j]].y - pHeightMap[currentPoint].y,
								   pHeightMap[pointDirection[j]].z - pHeightMap[currentPoint].z };
						vectors[j] = XMLoadFloat3(&vec);
						numOfNorms++;
					}
				}

				//get normals to planes (div by no of normals collected
				//up left
				v3 cross = { 0,1,0 };
				cross += XMVector3Cross(vectors[0], vectors[1]);
				//left down
				cross += XMVector3Cross(vectors[1], vectors[2]);
				//down right
				cross += XMVector3Cross(vectors[2], vectors[3]);
				//up right
				cross += XMVector3Cross(vectors[3], vectors[4]);
				//
				cross += XMVector3Cross(vectors[4], vectors[5]);
				//left down
				cross += XMVector3Cross(vectors[5], vectors[6]);
				//down right
				cross += XMVector3Cross(vectors[6], vectors[7]);
				//up right
				cross += XMVector3Cross(vectors[7], vectors[0]);
				//normalise
				cross = cross / numOfNorms;

				XMStoreFloat3(&normalStorage[currentPoint], cross);
			}
		}

		for (int yVtx = 0; yVtx < HeightMapLength - 1; ++yVtx)
		{
			int mapIdx(0);
			mapIdx = (yVtx * HeightMapWidth);

			//Normal calculation
			v3 yNorm = { 0,1,0 };

			//--BEGIN TRIANGLE STRIP--

			//beginning of new row degen
			m_pMapVtxs[vertexNo] = MeshVertex(pHeightMap[yVtx * HeightMapWidth], MAP_COLOUR, normalStorage[yVtx * HeightMapWidth], test_uvs);
			m_pMapVtxs[vertexNo++] = MeshVertex(pHeightMap[yVtx * HeightMapWidth], MAP_COLOUR, normalStorage[yVtx * HeightMapWidth], test_uvs);
			//m_pMapVtxs[vertexNo] = Vertex_Pos3fColour4ubNormal3f(pHeightMap[yVtx * HeightMapWidth], MAP_COLOUR, yNorm);
			//m_pMapVtxs[vertexNo++] = Vertex_Pos3fColour4ubNormal3f(pHeightMap[yVtx * HeightMapWidth], MAP_COLOUR, yNorm);

			//loop rest of row (use x vtx somewhere here)
			for (int xVtx = 0; xVtx < HeightMapWidth; ++xVtx)
			{
				//vertex pairs e.g. (point 0 row 0) and (point 0 row 1)
				m_pMapVtxs[vertexNo++] = MeshVertex(pHeightMap[mapIdx + xVtx], MAP_COLOUR, normalStorage[mapIdx + xVtx], test_uvs);
				m_pMapVtxs[vertexNo++] = MeshVertex(pHeightMap[mapIdx + xVtx + HeightMapWidth], MAP_COLOUR, normalStorage[mapIdx + xVtx + HeightMapWidth], test_uvs);

				if (xVtx == HeightMapWidth - 1)
				{
					//end of row degen
					m_pMapVtxs[vertexNo++] = MeshVertex(pHeightMap[mapIdx + xVtx + HeightMapWidth], MAP_COLOUR, normalStorage[mapIdx + xVtx + HeightMapWidth], test_uvs);
					m_pMapVtxs[vertexNo++] = MeshVertex(pHeightMap[mapIdx + xVtx + HeightMapWidth], MAP_COLOUR, normalStorage[mapIdx + xVtx + HeightMapWidth], test_uvs);
					//m_pMapVtxs[vertexNo++] = Vertex_Pos3fColour4ubNormal3f(pHeightMap[mapIdx + xVtx + HeightMapWidth], MAP_COLOUR, yNorm);
					//m_pMapVtxs[vertexNo++] = Vertex_Pos3fColour4ubNormal3f(pHeightMap[mapIdx + xVtx + HeightMapWidth], MAP_COLOUR, yNorm);
				}
			}

		}
		
		//Populate mesh
		output->init_buffers(device, m_pMapVtxs, HeightMapVtxCount, 0, 0);

		//--END TRIANGLE STRIP--
		return true;
	}
}