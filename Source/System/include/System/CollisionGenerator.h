#pragma once

#include <string>

class CollisionGenerator final
{
public:
	CollisionGenerator() = delete;

	// Generates convex decomposition hulls for the mesh at pteroPath (or the
	// .ptero derived from fbxPath) and writes them to <stem>.collisions.json
	// next to the .ptero file.  Returns true on success.
	static bool GenerateCollisions(const std::string& fbxOrPteroPath);
};
