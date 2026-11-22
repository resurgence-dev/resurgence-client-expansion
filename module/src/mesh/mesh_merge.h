#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/*
	The merged-mesh builder (gamemode docs/mesh-combine-design.md): merges a character's part models (MDL v48, VVD v4,
	VTX v7) into one model the engine loads like any other. Pure: bytes in, bytes out, no engine or file access, so it
	runs on a worker thread and in the golden test against the Python prototype (tools/mesh_prototype/merge.py).

	The merged skeleton has one bone per name and bind pose: a part bone keeps its own bind, a bone the bake folds away
	becomes its live ancestor with the bind that reproduces it there, and bones of the same name and bind are shared.
	Bonemerge drives every same-named bone from the body's, so no skinned vertex moves. A part placed on a bone or an
	attachment (not bonemerged) is rigid: its vertices move into the body's bind pose on that one bone.

	Every input is untrusted (any server's Lua can call rcx.mesh): every offset and count is bounds-checked, and a
	malformed input is refused with a reason, never repaired.
*/
namespace rcx::mesh
{
	using Bytes = std::vector<uint8_t>;

	struct Placement
	{
		std::string bone;
		std::string attachment;
		double pos[3]{};
		double ang[3]{};
		double scale = 1.0;
	};

	struct PartInput
	{
		Bytes mdl;
		Bytes vvd;
		Bytes vtx;
		std::vector<std::pair<std::string, int>> bodygroups;
		int skin = 0;
		std::string material;
		std::vector<std::string> materials;
		bool positioned = false;
		Placement placement;
	};

	struct MergeInput
	{
		// The file name (the hash of the cache key); also seeds the checksum shared by the three files.
		std::string name;
		Bytes body;
		std::vector<PartInput> parts;
		// Skin bones the bake folds into their live ancestor (FoChar.MergedBones), lowercase name to ancestor name.
		std::vector<std::pair<std::string, std::string>> bake;
	};

	struct MergeOutput
	{
		Bytes mdl;
		Bytes vvd;
		Bytes vtx;
		int bones = 0;
		int baked = 0;
		int rebinds = 0;
		int meshes = 0;
		int vertices = 0;
	};

	// Caps on what a merge takes, so hostile input can't make it allocate without bound.
	constexpr size_t kMaxParts = 24;
	constexpr size_t kMaxFileBytes = 32 * 1024 * 1024;
	constexpr int kMaxBones = 255;
	constexpr int kMaxVertices = 512 * 1024;

	bool Merge(const MergeInput& input, MergeOutput& output, std::string& error);
}
