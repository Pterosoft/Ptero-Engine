#define NOMINMAX
#define DISABLE_SPDLOG
#include "System/CollisionGenerator.h"
#include "System/FbxCompiler.h"
#include "System/PteroMeshFormat.h"

#include "coacd.h"
#include "..\..\SDKs\CoACD\src\quickhull\QuickHull.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <string>
#include <vector>
#include <cstring>

namespace
{
	struct VertexPos
	{
		float x, y, z;
		// pad fields so we can skip the full Vertex stride
	};

	struct PackedCollisionVertex
	{
		float x, y, z;
	};

	// Minimal vertex stride in the .ptero binary: Position(12) + Normal(12) + TexCoord(8) + Color(16) = 48 bytes
	static constexpr std::size_t kVertexStride = 48;

	struct LegacyPteroMeshHeader
	{
		char magic[4] = { 'P', 'T', 'R', 'O' };
		std::uint32_t version = 2;
		std::uint32_t vertexCount = 0;
		std::uint32_t indexCount = 0;
		std::uint32_t subMeshCount = 0;
	};

	bool LoadPteroBaseVerticesAndIndices(
		const std::string& pteroPath,
		std::vector<std::array<double, 3>>& outVertices,
		std::vector<std::array<int, 3>>& outIndices)
	{
		std::ifstream f(pteroPath, std::ios::binary);
		if (!f) return false;

		// Read header (matches PteroMeshFormat.h)
		char magic[4] = {};
		f.read(magic, 4);
		if (std::memcmp(magic, "PTRO", 4) != 0) return false;

		std::uint32_t version = 0, vertexCount = 0, indexCount = 0, subMeshCount = 0;
		f.read(reinterpret_cast<char*>(&version), 4);
		f.read(reinterpret_cast<char*>(&vertexCount), 4);
		f.read(reinterpret_cast<char*>(&indexCount), 4);
		f.read(reinterpret_cast<char*>(&subMeshCount), 4);

		std::uint32_t lodCount = 1;
		if (version >= 3)
		{
			f.read(reinterpret_cast<char*>(&lodCount), 4);
		}

		// Skip sub-mesh table (12 bytes per entry)
		if (subMeshCount > 0)
		{
			f.seekg(static_cast<std::streamoff>(subMeshCount) * 12, std::ios::cur);
		}

		// Read vertex positions (stride = 48 bytes, position is first 12)
		outVertices.resize(vertexCount);
		for (std::uint32_t i = 0; i < vertexCount; ++i)
		{
			float pos[3] = {};
			f.read(reinterpret_cast<char*>(pos), 12);
			// Skip rest of vertex (Normal 12 + TexCoord 8 + Color 16 = 36 bytes)
			f.seekg(36, std::ios::cur);
			outVertices[i] = { static_cast<double>(pos[0]), static_cast<double>(pos[1]), static_cast<double>(pos[2]) };
		}

		// Read indices and form triangles
		const std::uint32_t triangleCount = indexCount / 3;
		std::vector<std::uint32_t> rawIndices(indexCount);
		f.read(reinterpret_cast<char*>(rawIndices.data()), static_cast<std::streamsize>(indexCount * 4));

		outIndices.resize(triangleCount);
		for (std::uint32_t i = 0; i < triangleCount; ++i)
		{
			outIndices[i] = { static_cast<int>(rawIndices[i * 3 + 0]),
							  static_cast<int>(rawIndices[i * 3 + 1]),
							  static_cast<int>(rawIndices[i * 3 + 2]) };
		}

		return f.good() || f.eof();
	}

	bool ReplaceEmbeddedCollisionsInPtero(
		const std::filesystem::path& pteroPath,
		const std::vector<coacd::Mesh>& collisionHulls)
	{
		std::ifstream input(pteroPath, std::ios::binary);
		if (!input)
		{
			return false;
		}

		LegacyPteroMeshHeader legacyHeader{};
		input.read(reinterpret_cast<char*>(&legacyHeader), sizeof(legacyHeader));
		if (!input || std::memcmp(legacyHeader.magic, "PTRO", 4) != 0)
		{
			return false;
		}

		std::uint32_t lodCount = 1;
		if (legacyHeader.version >= 3)
		{
			input.read(reinterpret_cast<char*>(&lodCount), sizeof(lodCount));
			if (!input)
			{
				return false;
			}
		}

		lodCount = (std::max)(lodCount, 1u);
		for (std::uint32_t lodIndex = 0; lodIndex < lodCount; ++lodIndex)
		{
			std::uint32_t vertexCount = legacyHeader.vertexCount;
			std::uint32_t indexCount = legacyHeader.indexCount;
			std::uint32_t subMeshCount = legacyHeader.subMeshCount;
			if (lodIndex > 0)
			{
				PteroLodEntry lodEntry{};
				input.read(reinterpret_cast<char*>(&lodEntry), sizeof(lodEntry));
				if (!input)
				{
					return false;
				}

				vertexCount = lodEntry.vertexCount;
				indexCount = lodEntry.indexCount;
				subMeshCount = lodEntry.subMeshCount;
			}

			const std::streamoff payloadBytes =
				static_cast<std::streamoff>(subMeshCount) * static_cast<std::streamoff>(sizeof(PteroSubMeshEntry)) +
				static_cast<std::streamoff>(vertexCount) * static_cast<std::streamoff>(sizeof(VertexPos) + 36) +
				static_cast<std::streamoff>(indexCount) * static_cast<std::streamoff>(sizeof(std::uint32_t));
			input.seekg(payloadBytes, std::ios::cur);
			if (!input)
			{
				return false;
			}
		}

		const std::streamoff meshPayloadEnd = input.tellg();
		if (meshPayloadEnd < 0)
		{
			return false;
		}

		input.clear();
		input.seekg(0, std::ios::beg);
		std::vector<char> meshBytes(static_cast<std::size_t>(meshPayloadEnd));
		input.read(meshBytes.data(), static_cast<std::streamsize>(meshBytes.size()));
		if (!input)
		{
			return false;
		}
		input.close();

		if (meshBytes.size() >= sizeof(PteroMeshHeader))
		{
			auto* header = reinterpret_cast<PteroMeshHeader*>(meshBytes.data());
			header->version = kPteroMeshVersion;
		}

		std::ofstream output(pteroPath, std::ios::binary | std::ios::trunc);
		if (!output)
		{
			return false;
		}

		output.write(meshBytes.data(), static_cast<std::streamsize>(meshBytes.size()));
		if (!output)
		{
			return false;
		}

		const PteroCollisionHeader collisionHeader{ static_cast<std::uint32_t>(collisionHulls.size()) };
		output.write(reinterpret_cast<const char*>(&collisionHeader), sizeof(collisionHeader));
		if (!output)
		{
			return false;
		}

		for (const coacd::Mesh& hull : collisionHulls)
		{
			const PteroCollisionHullEntry hullEntry{
				static_cast<std::uint32_t>(hull.vertices.size()),
				static_cast<std::uint32_t>(hull.indices.size() * 3) };
			output.write(reinterpret_cast<const char*>(&hullEntry), sizeof(hullEntry));
			if (!output)
			{
				return false;
			}

			for (const auto& vertex : hull.vertices)
			{
				const PackedCollisionVertex packedVertex{
					static_cast<float>(vertex[0]),
					static_cast<float>(vertex[1]),
					static_cast<float>(vertex[2]) };
				output.write(reinterpret_cast<const char*>(&packedVertex), sizeof(packedVertex));
				if (!output)
				{
					return false;
				}
			}

			for (const auto& tri : hull.indices)
			{
				const std::uint32_t packedIndices[3] = {
					static_cast<std::uint32_t>(tri[0]),
					static_cast<std::uint32_t>(tri[1]),
					static_cast<std::uint32_t>(tri[2]) };
				output.write(reinterpret_cast<const char*>(packedIndices), sizeof(packedIndices));
				if (!output)
				{
					return false;
				}
			}
		}

		return static_cast<bool>(output);
	}

	std::vector<coacd::Mesh> BuildSingleConvexHullFallback(const coacd::Mesh& input)
	{
		if (input.vertices.size() < 4)
		{
			return {};
		}

		std::vector<quickhull::Vector3<double>> points;
		points.reserve(input.vertices.size());
		for (const auto& vertex : input.vertices)
		{
			points.emplace_back(vertex[0], vertex[1], vertex[2]);
		}

		quickhull::QuickHull<double> quickHull;
		bool success = false;
		auto hull = quickHull.getConvexHull(points, true, false, success);
		if (!success)
		{
			return {};
		}

		coacd::Mesh fallbackHull;
		for (const auto& vertex : hull.getVertexBuffer())
		{
			fallbackHull.vertices.push_back({ vertex.x, vertex.y, vertex.z });
		}

		const auto& indexBuffer = hull.getIndexBuffer();
		for (std::size_t index = 0; index + 2 < indexBuffer.size(); index += 3)
		{
			fallbackHull.indices.push_back({
				static_cast<int>(indexBuffer[index + 0]),
				static_cast<int>(indexBuffer[index + 1]),
				static_cast<int>(indexBuffer[index + 2]) });
		}

		if (fallbackHull.vertices.empty() || fallbackHull.indices.empty())
		{
			return {};
		}

		return { std::move(fallbackHull) };
	}
}

bool CollisionGenerator::GenerateCollisions(const std::string& fbxOrPteroPath)
{
	if (fbxOrPteroPath.empty()) return false;

	const std::filesystem::path inputPath(fbxOrPteroPath);
	const std::filesystem::path pteroPath =
		_stricmp(inputPath.extension().string().c_str(), ".ptero") == 0
		? inputPath
		: std::filesystem::path(fbxOrPteroPath + ".ptero");

	if (!std::filesystem::exists(pteroPath)) return false;

	// Load mesh data
	coacd::Mesh input;
	if (!LoadPteroBaseVerticesAndIndices(pteroPath.string(), input.vertices, input.indices))
	{
		return false;
	}

	if (input.vertices.empty() || input.indices.empty()) return false;

	// Suppress verbose CoACD logging
	coacd::set_log_level("error");

	// Run convex decomposition with default parameters
	std::vector<coacd::Mesh> parts;
	try
	{
		parts = coacd::CoACD(input);
	}
	catch (...)
	{
		parts = BuildSingleConvexHullFallback(input);
	}

	if (parts.empty())
	{
		parts = BuildSingleConvexHullFallback(input);
	}

	if (parts.empty()) return false;

	return ReplaceEmbeddedCollisionsInPtero(pteroPath, parts);
}
